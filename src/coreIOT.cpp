#include "coreIOT.h"

// ==========================================
// TASK MQTT – CORE 0
// ==========================================
void IoTCoreUploadTask(void *parameter)
{
    QueueHandle_t iotQueue = (QueueHandle_t)parameter;

    // ------------------------------------------
    // Kết nối WiFi
    // ------------------------------------------
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED)
        vTaskDelay(pdMS_TO_TICKS(500));

    vTaskDelay(pdMS_TO_TICKS(1000));
    Serial.println("Core 0: ✓ Đã kết nối Wi-Fi!");

    // ------------------------------------------
    // Khởi tạo MQTT client
    // ------------------------------------------
    WiFiClient wifiClient;
    PubSubClient mqttClient(wifiClient);
    mqttClient.setServer(IOT_CORE_SERVER, MQTT_PORT);
    mqttClient.setBufferSize(512);

    // Tạo client ID duy nhất từ MAC address
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char clientID[32];
    snprintf(clientID, sizeof(clientID),
             "ESP32_%02X%02X%02X", mac[3], mac[4], mac[5]);

    // ------------------------------------------
    // Hàm reconnect nội bộ
    // ------------------------------------------
    auto reconnect = [&]()
    {
        while (!mqttClient.connected())
        {
            Serial.print("Core 0: Đang kết nối MQTT...");
            if (mqttClient.connect(clientID, ACCESS_TOKEN, NULL))
            {
                Serial.println(" ✓ MQTT Connected!");
            }
            else
            {
                Serial.print(" ❌ Thất bại, rc=");
                Serial.print(mqttClient.state());
                Serial.println(" - thử lại sau 3s");
                vTaskDelay(pdMS_TO_TICKS(3000));
            }
        }
    };

    reconnect(); // Kết nối lần đầu

    // ------------------------------------------
    // Vòng lặp chính: giữ kết nối & publish
    // ------------------------------------------
    DataPacket pkt;
    for (;;)
    {
        if (!mqttClient.connected())
            reconnect();

        mqttClient.loop();

        // Chờ tối đa 100ms để mqttClient.loop() vẫn chạy đều
        if (xQueueReceive(iotQueue, &pkt, pdMS_TO_TICKS(100)) == pdPASS)
        {
            char payload[256];
            snprintf(payload, sizeof(payload),
                     "{\"Subject_ID\":%d,"
                     "\"Timestamp_ms\":%lu,"
                     "\"R_Value\":%.4f,"
                     "\"SpO2_Percent\":%.1f,"
                     "\"BPM\":%d}",
                     pkt.subjectID,
                     pkt.timestamp,
                     pkt.rValue,
                     pkt.spo2,
                     pkt.bpm);

            if (mqttClient.publish(MQTT_TOPIC, payload))
            {
                Serial.print(">> ✓ MQTT Publish OK | Subject ");
                Serial.print(pkt.subjectID);
                Serial.print(" | SpO2=");
                Serial.print(pkt.spo2, 1);
                Serial.print("% | BPM=");
                Serial.println(pkt.bpm);
            }
            else
            {
                Serial.println(">> ❌ MQTT Publish thất bại!");
            }
        }
    }
}