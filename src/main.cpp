#include "BP35A1.hpp"
#include "HardwareSerialAdapter.h"
#include "LowVoltageSmartElectricEnergyMeter.hpp"
#include "SmartMeterConfig.h"
#include <ArduinoOTA.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <HardwareSerial.h>
#include <M5Unified.h>
#include <PubSubClient.h>
#include <WebSerial.h>
#include <WiFi.h>

typedef enum {
    EventInvalid = 0,
    EventPressHomeButton,
} EventType;

HardwareSerial wisunSerial(1);
HardwareSerialAdapter wisunAdapter(wisunSerial);
BP35A1 wisun(B_ROUTE_ID, B_ROUTE_PASSWORD, wisunAdapter);
WiFiClient client;
PubSubClient mqtt(MQTT_SERVER_URL, MQTT_SERVER_PORT, client);
AsyncWebServer server(80);
typedef struct {
    int32_t instantaneousPower;
    float instantaneousCurrentR;
    float instantaneousCurrentT;
    float cumulativeEnergyPositive;
} SmartMeterData;
QueueHandle_t queue;

void switchDisplay(const bool display) {
    M5.Display.setBrightness(display ? 128 : 0);
    digitalWrite(32, display ? LOW : HIGH);
    M5.Lcd.writecommand(display ? 0x11 : 0x10);
    if (display) {
        M5.Lcd.writecommand(0x29);
    }
}

void updateData(const SmartMeterData &data) {
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_WHITE);
    M5.Display.setCursor(0, 0);
    M5.Display.setFont(&fonts::Font2);

    log_i("InstantaneousPower       : %d W", data.instantaneousPower);
    log_i("CumulativeEnergyPositive : %f kWh", data.cumulativeEnergyPositive);
    log_i("InstantaneousCurrentR    : %f A", data.instantaneousCurrentR);
    log_i("InstantaneousCurrentT    : %f A", data.instantaneousCurrentT);

    M5.Display.printf("Power  : %d W\n", data.instantaneousPower);
    M5.Display.printf("Energy : %.2f kWh\n", data.cumulativeEnergyPositive);

    WebSerial.printf("InstantaneousPower       : %d W\n", data.instantaneousPower);
    WebSerial.printf("CumulativeEnergyPositive : %f kWh\n", data.cumulativeEnergyPositive);
    WebSerial.printf("InstantaneousCurrentR    : %f A\n", data.instantaneousCurrentR);
    WebSerial.printf("InstantaneousCurrentT    : %f A\n", data.instantaneousCurrentT);

    mqtt.publish("SmartMeter/Power/Instantaneous", String(data.instantaneousPower).c_str());
    mqtt.publish("SmartMeter/Energy/Cumulative/Positive", String(data.cumulativeEnergyPositive).c_str());
    mqtt.publish("SmartMeter/Current/Instantaneous/R", String(data.instantaneousCurrentR).c_str());
    mqtt.publish("SmartMeter/Current/Instantaneous/T", String(data.instantaneousCurrentT).c_str());
}

void smartMeterLoop(LowVoltageSmartElectricEnergyMeterClass smartMeter) {
    SmartMeterData data;
    if (smartMeter.getInstantaneousPower(&data.instantaneousPower) &&
        smartMeter.getInstantaneousCurrent(&data.instantaneousCurrentR, &data.instantaneousCurrentT) &&
        smartMeter.getCumulativeEnergyPositive(&data.cumulativeEnergyPositive)) {
        updateData(data);
    }
}

void setup() {
    // Init M5StickC
    m5::M5Unified::config_t cfg = M5.config();
    M5.begin(cfg);
    M5.Display.setBrightness(128);
    M5.Display.setRotation(3);
    switchDisplay(false);
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
        log_i("WiFi connecting to %s failed", WIFI_SSID);
        delay(10000);
    }

    // Init BP35A1
    wisunSerial.begin(115200, SERIAL_8N1, BP35A1_RX, BP35A1_TX, false, 20000UL);
    wisun.setStatusChangeCallback([](const BP35A1::InitializeState status) -> void {
        mqtt.publish("SmartMeter/Status", status == BP35A1::InitializeState::readySmartMeter ? "connected" : "initializing");
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
    pinMode(37, INPUT_PULLUP);
    attachInterrupt(37, []() -> void {
        EventType event = EventPressHomeButton;
        xQueueSend(queue, &event, portMAX_DELAY); }, FALLING);
}

void loop() {
    static uint32_t initStart = 0;
    static uint32_t waitStart = 0;

    if (wisun.getInitializeState() != BP35A1::InitializeState::readySmartMeter) {
        wisun.initializeLoop();
        if (initStart == 0) {
            initStart = millis();
        } else if ((millis() - initStart) >= 180000UL) {
            log_e("Initialization timeout, restarting...");
            ESP.restart();
        }
        return;
    }

    mqtt.loop();

    if (wisun.getCommunicationState() == BP35A1::CommunicationState::ready) {
        wisun.sendPropertyRequest(std::vector<LowVoltageSmartElectricEnergyMeterClass::Property>({
            LowVoltageSmartElectricEnergyMeterClass::Property::InstantaneousPower,
            LowVoltageSmartElectricEnergyMeterClass::Property::InstantaneousCurrents,
            LowVoltageSmartElectricEnergyMeterClass::Property::CumulativeEnergyPositive,
        }));
    } else {
        if (wisun.communicationLoop(smartMeterLoop, BP35A1::CommunicationState::ready)) {
            waitStart = 0;
        } else {
            if (waitStart == 0) {
                waitStart = millis();
            }
            uint32_t elapsed = millis() - waitStart;
            log_d("wait %lu sec...", elapsed / 1000);
            if (elapsed >= 10000UL) {
                log_i("retry...");
                wisun.resetCommunicationState();
                waitStart = 0;
            }
        }
    }

    WebSerial.loop();
    delay(1000);
}
