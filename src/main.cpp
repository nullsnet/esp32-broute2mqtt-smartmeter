#include "BP35A1.hpp"
#include "LowVoltageSmartElectricEnergyMeter.hpp"
#include "SmartMeterConfig.h"
#include <ArduinoOTA.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <M5StickC.h>
#include <PubSubClient.h>
#include <WebSerial.h>
#include <WiFi.h>

typedef struct {
    int32_t instantaneousPower;
    float instantCurrent_R;
    float instantCurrent_T;
    float cumulativeEnergyPositive;
} SmartMeterData;

typedef enum {
    EventInvalid = 0,
    EventPressHomeButton,
} EventType;

BP35A1 wisun = BP35A1(B_ROUTE_ID, B_ROUTE_PASSWORD);
WiFiClient client;
PubSubClient mqtt(MQTT_SERVER_URL, MQTT_SERVER_PORT, client);
LowVoltageSmartElectricEnergyMeterClass echonet;
AsyncWebServer server(80);
SmartMeterData data = {0, 0, 0, 0};
QueueHandle_t queue;
bool display = false;

extern void otaTask(void *const param);
extern ArduinoOTAClass ArduinoOTA;

bool getData(SmartMeterData *const data, const uint32_t delayms = 100, const uint32_t timeoutms = 3000) {
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

void updateDisplay() {
    if (display) {
        M5.Lcd.fillScreen(BLACK);
        M5.Lcd.setTextColor(WHITE);
        M5.Lcd.setCursor(0, 0, 2);
        M5.Lcd.printf("Power  : %d W\n", data.instantaneousPower);
        M5.Lcd.printf("Energy : %.2f kWh\n", data.cumulativeEnergyPositive);
    }
}

void setup() {
    // Init M5StickC
    M5.begin();
    M5.Axp.ScreenBreath(0);
    M5.Lcd.setRotation(3);

    mqtt.setKeepAlive(60);
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/plain", "Hi! This is WebSerial demo. You can access webserial interface at http://" + WiFi.localIP().toString() + "/webserial");
    });

    esp_log_set_vprintf([](const char *fmt, va_list args) -> int {
        static char logBuffer[512];
        int len = vsnprintf(logBuffer, sizeof(logBuffer), fmt, args);
        if (len > 0) {
            WebSerial.println(logBuffer);
        }
        return len;
    });

    // Init OTA
    ArduinoOTA.setTimeout(60000);
    ArduinoOTA
        .setHostname(HOSTNAME)
        .setPort(3232)
        .onStart([]() { log_i("Start updating %s", ArduinoOTA.getCommand() == U_FLASH ? "sketch" : "filesystem"); })
        .onEnd([]() { log_i("\nEnd"); })
        .onProgress([](const unsigned int progress, const unsigned int total) { log_i("Progress: %u%%\r", (progress / (total / 100))); })
        .onError([](const ota_error_t error) { log_i("Error[%u]: ", error); });

    // Init WiFi
    WiFi.setHostname(HOSTNAME);
    WiFi.setAutoReconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.onEvent([](const WiFiEvent_t event) {
        log_i("WiFi event: %s(%d), WiFi status : %d", WiFi.eventName(event), event, WiFi.status());
        switch (event) {
            case SYSTEM_EVENT_STA_GOT_IP:
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
                break;
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
    wisun.setStatusChangeCallback([](const BP35A1::SkStatus status) {
        switch (status) {
            case BP35A1::SkStatus::uninitialized:
                mqtt.publish("SmartMeter/Status", "uninitialized", true);
                break;
            case BP35A1::SkStatus::connecting:
                mqtt.publish("SmartMeter/Status", "connecting", true);
                break;
            case BP35A1::SkStatus::scanning:
                mqtt.publish("SmartMeter/Status", "scanning", true);
                break;
            case BP35A1::SkStatus::connected:
                mqtt.publish("SmartMeter/Status", "connected", true);
                break;
            default:
                break;
        }
    });

    queue = xQueueCreate(10, sizeof(EventType));

    // Event Task
    const TaskFunction_t eventTask = [](void *const param) {
        EventType event = EventInvalid;
        while (1) {
            if (xQueueReceive(queue, &event, portMAX_DELAY) == pdTRUE) {
                switch (event) {
                    case EventPressHomeButton:
                        display = !display;
                        M5.Axp.ScreenBreath(display ? 12 : 0);
                        M5.Lcd.setRotation(display ? 3 : 0);
                        updateDisplay();
                        break;
                    default:
                        break;
                }
            }
        }
    };
    xTaskCreatePinnedToCore(eventTask, "EventTask", 4096, NULL, 1, NULL, 1);

    // OTA Task
    const TaskFunction_t otaTaskFunction = [](void *const param) {
        while (true) {
            ArduinoOTA.handle();
            delay(100);
        }
    };
    xTaskCreatePinnedToCore(otaTaskFunction, "OtaTask", 4096, NULL, 1, NULL, 1);

    // Home button ISR
    pinMode(M5_BUTTON_HOME, INPUT_PULLUP);
    const auto m5ButtonHomeIsr = []() {
        EventType event = EventPressHomeButton;
        xQueueSend(queue, &event, portMAX_DELAY);
    };
    attachInterrupt(M5_BUTTON_HOME, m5ButtonHomeIsr, FALLING);
}

void loop() {
    static int failed_counter             = 0;
    static bool initialized_constant_data = false;

    if (wisun.getSkStatus() != BP35A1::SkStatus::connected) {
        if (wisun.initialize(50) == false) {
            failed_counter++;
            return;
        }
    }

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
