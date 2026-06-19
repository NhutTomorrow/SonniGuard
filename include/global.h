#pragma once

#include <Wire.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Arduino.h>

// ==========================================
// THÔNG SỐ CẤU HÌNH KẾT NỐI MẠNG & IOT CORE
// ==========================================
#define WIFI_SSID "BKIT_L1"
#define WIFI_PASSWORD "cselabc5c6"
#define IOT_QUEUE_LENGTH 15
// ==========================================
// CẤU TRÚC GÓI DỮ LIỆU GIỮA 2 LÕI CPU
// ==========================================
struct DataPacket
{
    int subjectID;
    unsigned long timestamp;
    float rValue;
    float spo2;
    int bpm;
};

// Hàng đợi FreeRTOS
extern QueueHandle_t iotQueue;
extern int subjectID;