
#include "SmartMeterConfig.h"
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <PubSubClient.h>

extern PubSubClient mqtt;

void reconnectWiFi() {
    log_i("connecting to %s", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSPHRASE);
    while (WiFi.status() != WL_CONNECTED) {
        log_i("waiting for connection... (%d)", WiFi.status());
        delay(1000);
    }
}

void wifiTask(void *const param) {
    ArduinoOTA.setTimeout(60000);
    ArduinoOTA
        .setHostname(HOSTNAME)
        .setPort(3232)
        .onStart([]() {
            log_i("Start updating %s", ArduinoOTA.getCommand() == U_FLASH ? "sketch" : "filesystem");
        })
        .onEnd([]() {
            log_i("\nEnd");
        })
        .onProgress([](const unsigned int progress, const unsigned int total) {
            log_i("Progress: %u%%\r", (progress / (total / 100)));
        })
        .onError([](const ota_error_t error) {
            log_i("Error[%u]: ", error);
            switch (error) {
                case OTA_AUTH_ERROR:
                    log_i("Auth Failed");
                    break;
                case OTA_BEGIN_ERROR:
                    log_i("Begin Failed");
                    break;
                case OTA_CONNECT_ERROR:
                    log_i("Connect Failed");
                    break;
                case OTA_RECEIVE_ERROR:
                    log_i("Receive Failed");
                    break;
                case OTA_END_ERROR:
                    log_i("End Failed");
                    break;
                default:
                    log_i("Unknown");
                    break;
            }
        });

    log_i("Start OTA Task");

    WiFi.setHostname(HOSTNAME);
    WiFi.onEvent([](const WiFiEvent_t event) {
        log_i("WiFi event: %d, WiFi status : %d", event, WiFi.status());
        switch (event) {
            case SYSTEM_EVENT_STA_DISCONNECTED:
                reconnectWiFi();
                break;
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
                break;
        }
    });
    reconnectWiFi();

    while (true) {
        ArduinoOTA.handle();
        delay(100);
    }
}
