#include "BP35A1.hpp"
#include "LowVoltageSmartElectricEnergyMeter.hpp"
#include "SmartMeterConfig.h"
#include <ArduinoOTA.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <M5StickC.h>
#include <PubSubClient.h>
#include <WebSerial.h>
#include <WiFi.h>

typedef enum {
    EventInvalid = 0,
    EventPressHomeButton,
} EventType;

BP35A1 wisun = BP35A1(B_ROUTE_ID, B_ROUTE_PASSWORD);
WiFiClient client;
PubSubClient mqtt(MQTT_SERVER_URL, MQTT_SERVER_PORT, client);
LowVoltageSmartElectricEnergyMeterClass echonet;
AsyncWebServer server(80);
struct SmartMeterData {
    int32_t instantaneousPower;
    float instantCurrent_R;
    float instantCurrent_T;
    float cumulativeEnergyPositive;
} data = {0, 0, 0, 0};
QueueHandle_t queue;

bool getData(struct SmartMeterData *const data, const uint32_t delayms = 100, const uint32_t timeoutms = 3000) {
    const auto properties = std::vector<LowVoltageSmartElectricEnergyMeterClass::Property>({
        LowVoltageSmartElectricEnergyMeterClass::Property::InstantaneousPower,
        LowVoltageSmartElectricEnergyMeterClass::Property::InstantaneousCurrents,
        LowVoltageSmartElectricEnergyMeterClass::Property::CumulativeEnergyPositive,
    });
    echonet.generateGetRequest(properties);
    if (wisun.sendUdpData(echonet.getRawData().data(), echonet.size(), 100, properties.size() * 3000)) {
        if (echonet.load(wisun.getUdpData(100, properties.size() * 3000).payload.c_str()) &&
            echonet.getInstantaneousPower(&data->instantaneousPower) &&
            echonet.getInstantaneousCurrent(&data->instantCurrent_R, &data->instantCurrent_T) &&
            echonet.getCumulativeEnergyPositive(&data->cumulativeEnergyPositive)) {
            return true;
        }
    }
    return false;
}

bool initConstantData(const uint32_t delayms = 100, const uint32_t timeoutms = 3000) {
    const auto properties = std::vector<LowVoltageSmartElectricEnergyMeterClass::Property>({
        LowVoltageSmartElectricEnergyMeterClass::Property::Coefficient,
        LowVoltageSmartElectricEnergyMeterClass::Property::CumulativeEnergyUnit,
    });
    echonet.generateGetRequest(properties);
    if (wisun.sendUdpData(echonet.getRawData().data(), echonet.size(), 100, properties.size() * 3000)) {
        if (echonet.load(wisun.getUdpData(100, properties.size() * 3000).payload.c_str()) && echonet.initConstantData()) {
            return true;
        }
    }
    return false;
}

void switchDisplay(const bool display) {
    M5.Axp.ScreenBreath(display ? 12 : 0);
    digitalWrite(32, display ? LOW : HIGH);
    M5.Lcd.writecommand(display ? 0x11 : 0x10);
    if (display) {
        M5.Lcd.writecommand(0x29);
    }
}

void updateDisplay() {
    BP35A1::SkStatus status = wisun.getSkStatus();
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setTextColor(WHITE);
    M5.Lcd.setCursor(0, 0, 2);
    if (status == BP35A1::SkStatus::connected) {
        M5.Lcd.printf("Power  : %d W\n", data.instantaneousPower);
        M5.Lcd.printf("Energy : %.2f kWh\n", data.cumulativeEnergyPositive);
        WebSerial.printf("InstantaneousPower       : %d W\n", data.instantaneousPower);
        WebSerial.printf("CumulativeEnergyPositive : %f kWh\n", data.cumulativeEnergyPositive);
        WebSerial.printf("InstantaneousCurrentR    : %f A\n", data.instantCurrent_R);
        WebSerial.printf("InstantaneousCurrentT    : %f A\n", data.instantCurrent_T);
    } else {
        M5.Lcd.printf("Status  : %d\n", status);
        WebSerial.printf("Status  : %d\n", status);
    }
}

void setup() {
    // Init M5StickC
    M5.begin();
    M5.Axp.ScreenBreath(12);
    M5.Lcd.setRotation(3);
    pinMode(32, OUTPUT);

    mqtt.setKeepAlive(60);
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->redirect("/webserial");
    });

    // Init OTA
    ArduinoOTA
        .setHostname(HOSTNAME)
        .setPort(3232)
        .onStart([]() { log_i("Start updating %s", ArduinoOTA.getCommand() == U_FLASH ? "sketch" : "filesystem"); })
        .onEnd([]() { log_i("\nEnd"); })
        .onProgress([](const unsigned int progress, const unsigned int total) { log_i("Progress: %u%%\r", (progress / (total / 100))); })
        .onError([](const ota_error_t error) { log_i("Error[%u]: ", error); })
        .setTimeout(60000);

    // Init WiFi
    WiFi.setHostname(HOSTNAME);
    WiFi.setAutoReconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.onEvent([](const WiFiEvent_t event) {
        log_i("WiFi event: %s(%d), WiFi status : %d", WiFi.eventName(event), event, WiFi.status());
        if (event == (WiFiEvent_t)SYSTEM_EVENT_STA_GOT_IP) {
            log_i("WiFi connected, IP: %s", WiFi.localIP().toString().c_str());
            MDNS.begin(HOSTNAME);
            ArduinoOTA.begin();
            log_i("OTA Ready");
            while (mqtt.connected() == false) {
                mqtt.connect(MQTT_CONNECT_ID, MQTT_CONNECT_USER, NULL, "SmartMeter/Status", 0, true, "offline");
                delay(1000);
            }
            log_i("MQTT Ready");
            mqtt.publish("SmartMeter/Status", "online");
            WebSerial.begin(&server);
            server.begin();
        }
    });
    log_i("connecting to %s", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSPHRASE);
    while (WiFi.waitForConnectResult() != WL_CONNECTED) {
        log_i("WiFi connection failed");
        delay(10000);
    }

    // Init BP35A1
    wisun.begin(115200, SERIAL_8N1, BP35A1_RX, BP35A1_TX, false, 20000UL);
    wisun.setStatusChangeCallback([](const BP35A1::SkStatus status) -> void {
        mqtt.publish("SmartMeter/Status", ((const char *const[]){"uninitialized", "scanning", "connecting", "connected"})[(int)status], true);
        status == BP35A1::SkStatus::connected ? switchDisplay(false) : updateDisplay();
    });

    queue = xQueueCreate(10, sizeof(EventType));

    // Event Task
    const TaskFunction_t eventTask = [](void *const param) -> void {
        EventType event = EventInvalid;
        bool display    = false;
        while (1) {
            if (xQueueReceive(queue, &event, portMAX_DELAY) == pdTRUE) {
                if (event == EventPressHomeButton) {
                    display = !display;
                    switchDisplay(display);
                    updateDisplay();
                }
            }
        }
    };
    xTaskCreatePinnedToCore(eventTask, "EventTask", 4096, NULL, 1, NULL, 1);

    // OTA Task
    const TaskFunction_t otaTaskFunction = [](void *const param) -> void {
        while (true) {
            ArduinoOTA.handle();
            delay(100);
        }
    };
    xTaskCreatePinnedToCore(otaTaskFunction, "OtaTask", 4096, NULL, 1, NULL, 1);

    // Home button ISR
    pinMode(M5_BUTTON_HOME, INPUT_PULLUP);
    const auto m5ButtonHomeIsr = []() -> void {
        EventType event = EventPressHomeButton;
        xQueueSend(queue, &event, portMAX_DELAY);
    };
    attachInterrupt(M5_BUTTON_HOME, m5ButtonHomeIsr, FALLING);
}

void loop() {
    static int failed_counter = 0;
    if (wisun.getSkStatus() != BP35A1::SkStatus::connected) {
        if (wisun.initialize(50) == false) {
            failed_counter++;
            return;
        }
    }

    static bool initialized_constant_data = false;
    if (initialized_constant_data == false) {
        initialized_constant_data = initConstantData();
        if (initialized_constant_data == false) {
            failed_counter++;
            return;
        }
        log_i("ConvertCumulativeEnergyUnit : %f", echonet.cumulativeEnergyUnit);
        log_i("SyntheticTransformationRatio: %d", echonet.syntheticTransformationRatio);
    }

    mqtt.loop();

    if (getData(&data) == false ||
        mqtt.publish("SmartMeter/Power/Instantaneous", String(data.instantaneousPower).c_str()) == false ||
        mqtt.publish("SmartMeter/Energy/Cumulative/Positive", String(data.cumulativeEnergyPositive).c_str()) == false ||
        mqtt.publish("SmartMeter/Current/Instantaneous/R", String(data.instantCurrent_R).c_str()) == false ||
        mqtt.publish("SmartMeter/Current/Instantaneous/T", String(data.instantCurrent_T).c_str()) == false) {
        failed_counter++;
    } else {
        log_i("InstantaneousPower       : %d W", data.instantaneousPower);
        log_i("CumulativeEnergyPositive : %f kWh", data.cumulativeEnergyPositive);
        log_i("InstantaneousCurrentR    : %f A", data.instantCurrent_R);
        log_i("InstantaneousCurrentT    : %f A", data.instantCurrent_T);
        updateDisplay();
        failed_counter = 0;
    }

    if (failed_counter > 10) {
        wisun.resetSkStatus();
        failed_counter = 0;
    }

    WebSerial.loop();
    delay(1000);
}
