
#pragma once
#include "global.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// ThingsBoard MQTT
#define IOT_CORE_SERVER "app.coreiot.io"
#define MQTT_PORT 1883
#define ACCESS_TOKEN "xki9yjqjbwc9nekoyz59"

// Topic chuẩn ThingsBoard telemetry qua MQTT
#define MQTT_TOPIC "v1/devices/me/telemetry"

// ==========================================
// TASK MQTT UPLOAD – CHẠY TRÊN CORE 0
// ==========================================

/**
 * FreeRTOS task: kết nối WiFi + MQTT, nhận DataPacket từ queue
 * và publish lên ThingsBoard.
 *
 * Truyền handle của iotQueue qua tham số parameter.
 * Gọi xTaskCreatePinnedToCore() với hàm này trong setup().
 */
void IoTCoreUploadTask(void *parameter);