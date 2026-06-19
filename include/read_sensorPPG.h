
#pragma once
#include "global.h"
#include "heartRate.h"
#include "MAX30105.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// ==========================================
// KHỞI TẠO & ĐIỀU KHIỂN SENSOR PPG
// ==========================================

/**
 * Khởi tạo MAX30105 qua I2C.
 * Gọi trong setup(). Treo chương trình nếu không tìm thấy sensor.
 */
void ppgSensor_init();

/**
 * Xử lý một chu kỳ mẫu (gọi liên tục trong loop()).
 * Đọc raw data, chạy DSP, tính SpO2/BPM.
 * Khi đủ WINDOW_SIZE mẫu sẽ đẩy DataPacket vào queue và in CSV ra Serial.
 *
 * @param subjectID  ID đối tượng đo hiện tại
 * @param iotQueue   Handle của FreeRTOS queue để gửi DataPacket
 */
void ppgSensor_process(int subjectID, QueueHandle_t iotQueue);