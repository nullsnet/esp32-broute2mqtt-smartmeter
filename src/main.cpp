#include "BP35A1.hpp"
#include "LowVoltageSmartElectricEnergyMeter.hpp"
#include "SmartMeterConfig.h"
#include <ArduinoOTA.h>
#include <M5StickC.h>
#include <PubSubClient.h>
#include <WiFi.h>

BP35A1 wisun = BP35A1(B_ROUTE_ID, B_ROUTE_PASSWORD);
WiFiClient client;
PubSubClient mqtt(MQTT_SERVER_URL, MQTT_SERVER_PORT, client);
LowVoltageSmartElectricEnergyMeterClass echonet;

void mqttLoop() {
    if (!mqtt.loop() || !client.connected()) {
        static int failed_counter = 0;
        while (!client.connected()) {
            if (mqtt.connect(MQTT_CONNECT_ID, MQTT_CONNECT_USER, NULL, "SmartMeter/Status", 0, true, "offline")) {
                failed_counter = 0;
            } else {
                failed_counter++;
            }
            if (failed_counter > 10) {
                esp_restart();
            }
            delay(1000);
        }
        mqtt.loop();
    }
}

void Bp35a1StatusChangedCallback(BP35A1::SkStatus status) {
    mqttLoop();
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
}

typedef struct {
    int32_t instantaneousPower;
    float instantCurrent_R;
    float instantCurrent_T;
    float cumulativeEnergyPositive;
} SmartMeterData;

bool getData(SmartMeterData *const data, uint32_t delayms = 100, uint32_t timeoutms = 3000) {
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

bool initConstantData(uint32_t delayms = 100, uint32_t timeoutms = 3000) {
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

void WiFiEvent(WiFiEvent_t event) {
    switch (event) {
        case SYSTEM_EVENT_STA_GOT_IP:
            mqtt.publish("SmartMeter/Status", "online");
            break;
        case SYSTEM_EVENT_STA_DISCONNECTED:
            WiFi.disconnect();
            WiFi.begin(WIFI_SSID, WIFI_PASSPHRASE);
            delay(WIFI_DELAY);
            break;
        default:
            break;
    }
}

void setup() {
    M5.begin();
    M5.Axp.ScreenBreath(0);
    M5.Lcd.setRotation(3);
    WiFi.onEvent(WiFiEvent);
    mqtt.setKeepAlive(60);
    wisun.begin(115200, SERIAL_8N1, BP35A1_RX, BP35A1_TX, false, 20000UL);
    wisun.setStatusChangeCallback(Bp35a1StatusChangedCallback);
    WiFi.setHostname("esp32-broute2mqtt-smartmeter");
}

void loop() {
    static bool display                   = false;
    static int failed_counter             = 0;
    static bool initialized_constant_data = false;

    M5.update();
    if (M5.BtnA.wasPressed()) {
        display = !display;
    }
    M5.Axp.ScreenBreath(display ? 12 : 0);

    if (WiFi.status() != WL_CONNECTED) {
        WiFi.begin(WIFI_SSID, WIFI_PASSPHRASE);
        while (WiFi.status() != WL_CONNECTED) {
            delay(WIFI_DELAY);
        }
        ArduinoOTA.begin();
    }

    ArduinoOTA.handle();

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

    mqttLoop();

    SmartMeterData data = {0, 0, 0, 0};
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
        failed_counter = 0;
    }
    if (display) {
        M5.Lcd.setCursor(0, 0, 2);
        M5.Lcd.printf("Power  : %d W\n", data.instantaneousPower);
        M5.Lcd.printf("Energy : %.2f kWh\n", data.cumulativeEnergyPositive);
    }

    if (failed_counter > 10) {
        wisun.resetSkStatus();
        failed_counter = 0;
    }

    delay(5000);
}
