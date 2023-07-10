#include "BP35A1.hpp"
#include "LowVoltageSmartElectricEnergyMeter.hpp"
#include "SmartMeterConfig.h"
#include <M5StickC.h>
#include <PubSubClient.h>
#include <WiFi.h>

BP35A1 wisun = BP35A1(B_ROUTE_ID, B_ROUTE_PASSWORD);
WiFiClient client;
PubSubClient mqtt(MQTT_SERVER_URL, MQTT_SERVER_PORT, client);
LowVoltageSmartElectricEnergyMeterClass echonet;

void mqttLoop() {
    if (!mqtt.loop()) {
        static int failed_counter = 0;
        while (!client.connected()) {
            if (mqtt.connect(MQTT_CONNECT_ID, MQTT_CONNECT_USER, NULL, "SmartMeter/Status/BRoute", 0, true, "offline")) {
                failed_counter = 0;
                mqtt.publish("SmartMeter/Status/mqtt", "connected");
            } else {
                failed_counter++;
            }
            if (failed_counter > 10)
                esp_restart();
            delay(1000);
        }
    }
}

void Bp35a1StatusChangedCallback(BP35A1::SkStatus status) {
    mqttLoop();
    switch (status) {
        case BP35A1::SkStatus::uninitialized:
        case BP35A1::SkStatus::connecting:
        case BP35A1::SkStatus::scanning:
            mqtt.publish("SmartMeter/Status/BRoute", "offline", true);
            break;
        case BP35A1::SkStatus::connected:
            mqtt.publish("SmartMeter/Status/BRoute", "online", true);
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
    if (wisun.sendUdpData(echonet.getRawData().data(), echonet.size(), 100, properties.size() * 3000))
        if (echonet.load(wisun.getUdpData(100, properties.size() * 3000).payload.c_str()) && echonet.initConstantData())
            return true;
    return false;
}

void setup() {
    M5.begin();
    M5.Axp.ScreenBreath(0);

    WiFi.begin(WIFI_SSID, WIFI_PASSPHRASE);
    while (WiFi.status() != WL_CONNECTED)
        delay(500);

    wisun.begin(115200, SERIAL_8N1, BP35A1_RX, BP35A1_TX, false, 20000UL);
    wisun.setStatusChangeCallback(Bp35a1StatusChangedCallback);

    if (!wisun.initialize())
        esp_restart();

    while (!initConstantData())
        delay(1000);

    log_i("ConvertCumulativeEnergyUnit : %f", echonet.cumulativeEnergyUnit);
    log_i("SyntheticTransformationRatio: %d", echonet.syntheticTransformationRatio);
}

void loop() {
    mqttLoop();

    SmartMeterData data       = {0, 0, 0, 0};
    static int failed_counter = 0;

    if (WiFi.status() != WL_CONNECTED ||
        !getData(&data) ||
        !mqtt.publish("SmartMeter/Power/Instantaneous", String(data.instantaneousPower).c_str()) ||
        !mqtt.publish("SmartMeter/Energy/Cumulative/Positive", String(data.cumulativeEnergyPositive).c_str()) ||
        !mqtt.publish("SmartMeter/Current/Instantaneous/R", String(data.instantCurrent_R).c_str()) ||
        !mqtt.publish("SmartMeter/Current/Instantaneous/T", String(data.instantCurrent_T).c_str())) {
        failed_counter++;
    } else {
        log_i("InstantaneousPower       : %d W", data.instantaneousPower);
        log_i("CumulativeEnergyPositive : %f kWh", data.cumulativeEnergyPositive);
        log_i("InstantaneousCurrentR    : %f A", data.instantCurrent_R);
        log_i("InstantaneousCurrentT    : %f A", data.instantCurrent_T);
        failed_counter = 0;
    }

    if (failed_counter > 10)
        esp_restart();

    delay(5000);
}
