// #include <Wire.h>
// #include <Arduino.h>
// #include "global.h"
// #include "read_sensorPPG.h"
// #include "coreIOT.h"
// #include "read_sensorIMU.h"
// // ==========================================
// // BIẾN TOÀN CỤC
// // ==========================================
// QueueHandle_t iotQueue;
// int subjectID = 0;
// TaskHandle_t ppgTaskHandle = NULL;
// // ==========================================
// // CHỜ NHẬP SUBJECT ID QUA SERIAL
// // ==========================================
// static void waitForSubjectID()
// {
//   Serial.println("\n================================");
//   Serial.println("SONNI GUARD - Hệ thống đo SpO2 & BPM");
//   Serial.println("================================");
//   Serial.println("\n⏳ VUI LÒNG NHẬP SUBJECT ID (1-9999999):");
//   Serial.println("Ví dụ: gõ '5' rồi bấm ENTER để bắt đầu\n");

//   while (subjectID <= 0)
//   {
//     if (Serial.available() > 0)
//     {
//       int inputID = Serial.parseInt();
//       while (Serial.available() > 0)
//         Serial.read(); // flush

//       if (inputID > 0 && inputID <= 9999999)
//       {
//         subjectID = inputID;
//         Serial.print("✓ Subject ID được thiết lập: ");
//         Serial.println(subjectID);
//         Serial.println("\n✓ Bắt đầu đo... Đặt sensor trên ngón tay");
//         delay(2000);
//         break;
//       }
//       else
//       {
//         Serial.println("❌ ID không hợp lệ! Vui lòng nhập số từ 1-9999999");
//       }
//     }
//     delay(100);
//   }
// }

// // ==========================================
// // SETUP
// // ==========================================
// void setup()
// {
//   Serial.begin(115200);
//   Wire.begin(21, 22); // SDA=21, SCL=22

//   ppgSensor_init();

//   iotQueue = xQueueCreate(IOT_QUEUE_LENGTH, sizeof(DataPacket));

//   // waitForSubjectID();

//   // xTaskCreatePinnedToCore(
//   //     IoTCoreUploadTask,
//   //     "IoTCoreTask",
//   //     8192,
//   //     (void *)iotQueue, // truyền queue handle vào task
//   //     1,
//   //     NULL,
//   //     0 // CORE 0
//   // );
//   xTaskCreate(ppg_RawData, "TaskReadPPG", 2048, NULL, 1, &ppgTaskHandle);

//   Serial.println("Hệ thống sẵn sàng. Nhập số ID trên Serial để đổi Subject_ID.");
//   Serial.println("Format CSV: Subject_ID, Timestamp_ms, RawRed, RawIR, R_Value, SpO2, BPM");
//   // imuSensorInit();
// }

// // ==========================================
// // LOOP – CORE 1: ĐỌC SENSOR & DSP
// // ==========================================
// void loop()
// {
//   // Đổi Subject ID qua Serial
//   // if (Serial.available() > 0)
//   // {
//   //   int inputID = Serial.parseInt();
//   //   if (inputID > 0)
//   //   {
//   //     subjectID = inputID;
//   //     while (Serial.available() > 0)
//   //       Serial.read();
//   //     Serial.print("✓ Đã đổi Subject ID thành: ");
//   //     Serial.println(subjectID);
//   //   }
//   // }
// }
#include <Arduino.h>
#include <Wire.h>
#include "MAX30105.h"
#include "spo2_algorithm.h"

MAX30105 particleSensor;

#define WINDOW_SIZE 256 // N = 256
#define STEP_SIZE 50    // M = 50 (Thêm 50 mẫu mới mỗi lần)

uint32_t redBuffer[WINDOW_SIZE];
uint32_t irBuffer[WINDOW_SIZE];

int32_t spo2;
int8_t validSPO2;
int32_t heartRate;
int8_t validHeartRate;

void setup()
{
    Serial.begin(115200);
    Wire.begin();
    Wire.setClock(400000);

    if (!particleSensor.begin(Wire, I2C_SPEED_FAST))
    {
        Serial.println("MAX30102 Init Failed!");
        while (1)
            ;
    }

    // Cấu hình Sensor ở 50Hz
    uint8_t ledBrightness = 0x24; // ~7.0mA
    uint8_t sampleAverage = 1;
    uint8_t ledMode = 2;      // SpO2 Mode (Red + IR)
    uint8_t sampleRate = 50;  // 50 Hz
    uint8_t pulseWidth = 411; // 18-bit resolution
    uint8_t adcRange = 4096;

    particleSensor.setup(ledBrightness, sampleAverage, ledMode, sampleRate, pulseWidth, adcRange);
    particleSensor.clearFIFO();

    // 1. Nạp đầy 256 mẫu đầu tiên (Mất ~5.12s)
    Serial.println("Filling initial 256 samples buffer...");
    for (int i = 0; i < WINDOW_SIZE; i++)
    {
        while (!particleSensor.available())
        {
            particleSensor.check();
        }
        redBuffer[i] = particleSensor.getRed();
        irBuffer[i] = particleSensor.getIR();
        particleSensor.nextSample();
    }

    // Tính toán lần đầu tiên
    maxim_heart_rate_and_oxygen_saturation(irBuffer, WINDOW_SIZE, redBuffer, &spo2, &validSPO2, &heartRate, &validHeartRate);
}

void loop()
{
    // 2. Dịch chuyển mảng: Bỏ 50 mẫu cũ nhất, giữ lại 206 mẫu
    for (int i = STEP_SIZE; i < WINDOW_SIZE; i++)
    {
        redBuffer[i - STEP_SIZE] = redBuffer[i];
        irBuffer[i - STEP_SIZE] = irBuffer[i];
    }

    // 3. Đọc 50 mẫu mới nhất vào cuối mảng (Mất ~1.0s)
    for (int i = (WINDOW_SIZE - STEP_SIZE); i < WINDOW_SIZE; i++)
    {
        while (!particleSensor.available())
        {
            particleSensor.check();
        }
        redBuffer[i] = particleSensor.getRed();
        irBuffer[i] = particleSensor.getIR();
        particleSensor.nextSample();
    }

    // 4. Tính toán SpO2 / HR trên cửa sổ 256 mẫu
    maxim_heart_rate_and_oxygen_saturation(irBuffer, WINDOW_SIZE, redBuffer, &spo2, &validSPO2, &heartRate, &validHeartRate);

    // 5. Output
    Serial.printf("Red: %lu |IR: %lu  |HR: %d (Valid: %d) | SpO2: %d%% (Valid: %d)\n", redBuffer[BUFFER_SIZE - 1], irBuffer[BUFFER_SIZE - 1], heartRate, validHeartRate, spo2, validSPO2);
}