// #include <Wire.h>
// #include "MAX30105.h"
// #include "heartRate.h"
// #include <WiFi.h>
// #include <PubSubClient.h>
// #include <Arduino.h>
// #include <Adafruit_MPU6050.h>
// // ==========================================
// // THÔNG SỐ CẤU HÌNH KẾT NỐI MẠNG & IOT CORE
// // ==========================================
// const char *WIFI_SSID = "BKIT_L1";
// const char *WIFI_PASSWORD = "cselabc5c6";

// // ThingsBoard MQTT
// const char *IOT_CORE_SERVER = "app.coreiot.io";
// const int MQTT_PORT = 1883;
// const char *ACCESS_TOKEN = "xki9yjqjbwc9nekoyz59";

// // Topic chuẩn ThingsBoard telemetry qua MQTT
// const char *MQTT_TOPIC = "v1/devices/me/telemetry";

// // ==========================================
// // CẤU TRÚC GÓI DỮ LIỆU GIỮA 2 LÕI CPU
// // ==========================================
// struct DataPacket
// {
//   int subjectID;
//   unsigned long timestamp;
//   float rValue;
//   float spo2;
//   int bpm;
// };

// // Hàng đợi FreeRTOS
// QueueHandle_t iotQueue;
// // sensor
// MAX30105 ppgSensor;
// Adafruit_MPU6050 mpu;
// // ==========================================
// // CẤU HÌNH SENSOR MAX30105
// // ==========================================
// const unsigned long SAMPLE_PERIOD_MS = 20; // 50 Hz
// unsigned long lastSampleTime = 0;

// const int WINDOW_SIZE = 100;
// int sampleCount = 0;

// int subjectID = 0;

// // --- DSP: BỘ LỌC IIR ---
// const float HPF_ALPHA = 0.95;
// const float LPF_BETA = 0.25;
// float dc_track_red = 0, dc_track_ir = 0;
// float lpf_red_prev = 0, lpf_ir_prev = 0;
// float sum_sq_red = 0, sum_sq_ir = 0;

// // --- KẾT QUẢ ĐẦU RA ---
// float final_R = 0.0;
// float final_SpO2 = 98.0;

// // --- BPM ---
// long lastBeatTime = 0;
// const int BPM_FILTER_SIZE = 5;
// int bpmHistory[BPM_FILTER_SIZE] = {75, 75, 75, 75, 75};
// int bpmIndex = 0;
// int smoothed_BPM = 75;

// // ==========================================
// // PROTOTYPE
// // ==========================================
// void IoTCoreUploadTask(void *parameter);

// // ==========================================
// // CHỜ NHẬP SUBJECT ID
// // ==========================================
// void waitForSubjectID()
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

//   if (!ppgSensor.begin(Wire, I2C_SPEED_FAST))
//   {
//     Serial.println("❌ Không tìm thấy MAX30105!");
//     while (1)
//       ;
//   }
//   ppgSensor.setup(60, 1, 2, 100, 411, 4096);

//   iotQueue = xQueueCreate(15, sizeof(DataPacket));

//   waitForSubjectID();

//   // Task MQTT chạy trên Core 0
//   xTaskCreatePinnedToCore(
//       IoTCoreUploadTask,
//       "IoTCoreTask",
//       8192,
//       NULL,
//       1,
//       NULL,
//       0 // CORE 0
//   );

//   Serial.println("Hệ thống sẵn sàng. Nhập số ID trên Serial để đổi Subject_ID.");
//   Serial.println("Format CSV: Subject_ID, Timestamp_ms, RawRed, RawIR, R_Value, SpO2, BPM");
// }

// // ==========================================
// // LOOP - CORE 1: ĐỌC SENSOR & DSP
// // ==========================================
// void loop()
// {
//   // Đổi Subject ID qua Serial
//   while (Serial.available() > 0)
//   {
//     int inputID = Serial.parseInt();
//     if (inputID > 0)
//     {
//       subjectID = inputID;
//       while (Serial.available() > 0)
//         Serial.read();
//       Serial.print("✓ Đã đổi Subject ID thành: ");
//       Serial.println(subjectID);
//     }
//   }

//   unsigned long currentMillis = millis();
//   if (currentMillis - lastSampleTime < SAMPLE_PERIOD_MS)
//     return;
//   lastSampleTime = currentMillis;

//   uint32_t rawRed = ppgSensor.getRed();
//   uint32_t rawIR = ppgSensor.getIR();

//   if (rawIR < 50000)
//   {
//     sampleCount = 0;
//     return;
//   }

//   // 1. BPM
//   if (checkForBeat(rawIR))
//   {
//     long delta = currentMillis - lastBeatTime;
//     lastBeatTime = currentMillis;
//     float instant_bpm = 60000.0f / delta;

//     if (instant_bpm >= 40 && instant_bpm <= 140)
//     {
//       if (abs(instant_bpm - smoothed_BPM) < 20)
//       {
//         bpmHistory[bpmIndex] = (int)instant_bpm;
//         bpmIndex = (bpmIndex + 1) % BPM_FILTER_SIZE;
//         long bpmSum = 0;
//         for (int i = 0; i < BPM_FILTER_SIZE; i++)
//           bpmSum += bpmHistory[i];
//         smoothed_BPM = bpmSum / BPM_FILTER_SIZE;
//       }
//     }
//   }

//   // 2. DSP 2 lớp (HPF + LPF)
//   float acRed_raw = rawRed - dc_track_red;
//   dc_track_red = (1.0f - HPF_ALPHA) * rawRed + HPF_ALPHA * dc_track_red;
//   float acIR_raw = rawIR - dc_track_ir;
//   dc_track_ir = (1.0f - HPF_ALPHA) * rawIR + HPF_ALPHA * dc_track_ir;

//   float acRed_filtered = (1.0f - LPF_BETA) * acRed_raw + LPF_BETA * lpf_red_prev;
//   lpf_red_prev = acRed_filtered;
//   float acIR_filtered = (1.0f - LPF_BETA) * acIR_raw + LPF_BETA * lpf_ir_prev;
//   lpf_ir_prev = acIR_filtered;

//   // 3. RMS Window
//   sum_sq_red += acRed_filtered * acRed_filtered;
//   sum_sq_ir += acIR_filtered * acIR_filtered;
//   sampleCount++;

//   if (sampleCount >= WINDOW_SIZE)
//   {
//     float rmsRed = sqrt(sum_sq_red / WINDOW_SIZE);
//     float rmsIR = sqrt(sum_sq_ir / WINDOW_SIZE);

//     if (rmsIR > 0 && dc_track_red > 0 && dc_track_ir > 0)
//     {
//       final_R = (rmsRed / dc_track_red) / (rmsIR / dc_track_ir);
//       float instant_SpO2 = 116.6f - 25.0f * final_R;
//       if (instant_SpO2 > 100.0f)
//         instant_SpO2 = 100.0f;
//       final_SpO2 = 0.2f * instant_SpO2 + 0.8f * final_SpO2;
//     }

//     // Gửi vào Queue
//     DataPacket dataP;
//     dataP.subjectID = subjectID;
//     dataP.timestamp = currentMillis;
//     dataP.rValue = final_R;
//     dataP.spo2 = final_SpO2;
//     dataP.bpm = smoothed_BPM;
//     xQueueSend(iotQueue, &dataP, 0);

//     // CSV ra Serial
//     Serial.print(subjectID);
//     Serial.print(",");
//     Serial.print(currentMillis);
//     Serial.print(",");
//     Serial.print(rawRed);
//     Serial.print(",");
//     Serial.print(rawIR);
//     Serial.print(",");
//     Serial.print(final_R, 4);
//     Serial.print(",");
//     Serial.print(final_SpO2, 1);
//     Serial.print(",");
//     Serial.println(smoothed_BPM);

//     sampleCount = 0;
//     sum_sq_red = 0;
//     sum_sq_ir = 0;
//   }
// }

// // ==========================================
// // TASK MQTT - CORE 0
// // ==========================================
// void IoTCoreUploadTask(void *parameter)
// {
//   // --- Kết nối WiFi ---
//   WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
//   while (WiFi.status() != WL_CONNECTED)
//   {
//     vTaskDelay(500 / portTICK_PERIOD_MS);
//   }
//   vTaskDelay(1000 / portTICK_PERIOD_MS);
//   Serial.println("Core 0: ✓ Đã kết nối Wi-Fi!");

//   // --- Khởi tạo MQTT client ---
//   WiFiClient wifiClient;
//   PubSubClient mqttClient(wifiClient);
//   mqttClient.setServer(IOT_CORE_SERVER, MQTT_PORT);
//   // Tăng buffer nếu payload lớn (mặc định 256 bytes)
//   mqttClient.setBufferSize(512);

//   // Hàm reconnect MQTT nội bộ (lambda-style dùng local function)
//   auto reconnectMQTT = [&]()
//   {
//     while (!mqttClient.connected())
//     {
//       Serial.print("Core 0: Đang kết nối MQTT...");
//       // clientID bất kỳ, username = Access Token, password = ""
//       if (mqttClient.connect("ESP32_PPG", ACCESS_TOKEN, NULL))
//       {
//         Serial.println(" ✓ MQTT Connected!");
//       }
//       else
//       {
//         Serial.print(" ❌ Thất bại, rc=");
//         Serial.print(mqttClient.state());
//         Serial.println(" - thử lại sau 3s");
//         vTaskDelay(3000 / portTICK_PERIOD_MS);
//       }
//     }
//   };

//   reconnectMQTT(); // Kết nối lần đầu

//   DataPacket txPacket;

//   for (;;)
//   {
//     // Giữ kết nối MQTT sống
//     if (!mqttClient.connected())
//     {
//       reconnectMQTT();
//     }
//     mqttClient.loop();

//     // Nhận gói từ Queue (chờ tối đa 100ms để mqttClient.loop() vẫn chạy)
//     if (xQueueReceive(iotQueue, &txPacket, pdMS_TO_TICKS(100)) == pdPASS)
//     {
//       // Build JSON payload
//       char payload[256];
//       snprintf(payload, sizeof(payload),
//                "{\"Subject_ID\":%d,\"Timestamp_ms\":%lu,\"R_Value\":%.4f,\"SpO2_Percent\":%.1f,\"BPM\":%d}",
//                txPacket.subjectID,
//                txPacket.timestamp,
//                txPacket.rValue,
//                txPacket.spo2,
//                txPacket.bpm);

//       if (mqttClient.publish(MQTT_TOPIC, payload))
//       {
//         Serial.print(">> ✓ MQTT Publish OK | Subject ");
//         Serial.print(txPacket.subjectID);
//         Serial.print(" | SpO2=");
//         Serial.print(txPacket.spo2, 1);
//         Serial.print("% | BPM=");
//         Serial.println(txPacket.bpm);
//       }
//       else
//       {
//         Serial.println(">> ❌ MQTT Publish thất bại!");
//       }
//     }
//   }
// }

#include <Wire.h>
#include <Arduino.h>
#include "global.h"
#include "read_sensorPPG.h"
#include "coreIOT.h"
#include "read_sensorIMU.h"
// ==========================================
// BIẾN TOÀN CỤC
// ==========================================
QueueHandle_t iotQueue;
int subjectID = 0;

// ==========================================
// CHỜ NHẬP SUBJECT ID QUA SERIAL
// ==========================================
static void waitForSubjectID()
{
  Serial.println("\n================================");
  Serial.println("SONNI GUARD - Hệ thống đo SpO2 & BPM");
  Serial.println("================================");
  Serial.println("\n⏳ VUI LÒNG NHẬP SUBJECT ID (1-9999999):");
  Serial.println("Ví dụ: gõ '5' rồi bấm ENTER để bắt đầu\n");

  while (subjectID <= 0)
  {
    if (Serial.available() > 0)
    {
      int inputID = Serial.parseInt();
      while (Serial.available() > 0)
        Serial.read(); // flush

      if (inputID > 0 && inputID <= 9999999)
      {
        subjectID = inputID;
        Serial.print("✓ Subject ID được thiết lập: ");
        Serial.println(subjectID);
        Serial.println("\n✓ Bắt đầu đo... Đặt sensor trên ngón tay");
        delay(2000);
        break;
      }
      else
      {
        Serial.println("❌ ID không hợp lệ! Vui lòng nhập số từ 1-9999999");
      }
    }
    delay(100);
  }
}

// ==========================================
// SETUP
// ==========================================
void setup()
{
  Serial.begin(115200);
  Wire.begin(21, 22); // SDA=21, SCL=22

  ppgSensor_init();

  iotQueue = xQueueCreate(IOT_QUEUE_LENGTH, sizeof(DataPacket));

  // waitForSubjectID();

  // xTaskCreatePinnedToCore(
  //     IoTCoreUploadTask,
  //     "IoTCoreTask",
  //     8192,
  //     (void *)iotQueue, // truyền queue handle vào task
  //     1,
  //     NULL,
  //     0 // CORE 0
  // );

  Serial.println("Hệ thống sẵn sàng. Nhập số ID trên Serial để đổi Subject_ID.");
  Serial.println("Format CSV: Subject_ID, Timestamp_ms, RawRed, RawIR, R_Value, SpO2, BPM");
  // imuSensorInit();
}

// ==========================================
// LOOP – CORE 1: ĐỌC SENSOR & DSP
// ==========================================
void loop()
{
  // Đổi Subject ID qua Serial
  // if (Serial.available() > 0)
  // {
  //   int inputID = Serial.parseInt();
  //   if (inputID > 0)
  //   {
  //     subjectID = inputID;
  //     while (Serial.available() > 0)
  //       Serial.read();
  //     Serial.print("✓ Đã đổi Subject ID thành: ");
  //     Serial.println(subjectID);
  //   }
  // }

  ppgSensor_process(subjectID, iotQueue);
}

// /*
//   Optical SP02 Detection (SPK Algorithm) using the MAX30105 Breakout
//   By: Nathan Seidle @ SparkFun Electronics
//   Date: October 19th, 2016
//   https://github.com/sparkfun/MAX30105_Breakout

//   This demo shows heart rate and SPO2 levels.

//   It is best to attach the sensor to your finger using a rubber band or other tightening
//   device. Humans are generally bad at applying constant pressure to a thing. When you
//   press your finger against the sensor it varies enough to cause the blood in your
//   finger to flow differently which causes the sensor readings to go wonky.

//   This example is based on MAXREFDES117 and RD117_LILYPAD.ino from Maxim. Their example
//   was modified to work with the SparkFun MAX30105 library and to compile under Arduino 1.6.11
//   Please see license file for more info.

//   Hardware Connections (Breakoutboard to Arduino):
//   -5V = 5V (3.3V is allowed)
//   -GND = GND
//   -SDA = A4 (or SDA)
//   -SCL = A5 (or SCL)
//   -INT = Not connected

//   The MAX30105 Breakout can handle 5V or 3.3V I2C logic. We recommend powering the board with 5V
//   but it will also run at 3.3V.
// */

// #include <Wire.h>
// #include "MAX30105.h"
// #include "spo2_algorithm.h"

// MAX30105 particleSensor;

// #define MAX_BRIGHTNESS 255

// #if defined(__AVR_ATmega328P__) || defined(__AVR_ATmega168__)
// // Arduino Uno doesn't have enough SRAM to store 100 samples of IR led data and red led data in 32-bit format
// // To solve this problem, 16-bit MSB of the sampled data will be truncated. Samples become 16-bit data.
// uint16_t irBuffer[100];  // infrared LED sensor data
// uint16_t redBuffer[100]; // red LED sensor data
// #else
// uint32_t irBuffer[100];  // infrared LED sensor data
// uint32_t redBuffer[100]; // red LED sensor data
// #endif

// int32_t bufferLength;  // data length
// int32_t spo2;          // SPO2 value
// int8_t validSPO2;      // indicator to show if the SPO2 calculation is valid
// int32_t heartRate;     // heart rate value
// int8_t validHeartRate; // indicator to show if the heart rate calculation is valid

// byte pulseLED = 11; // Must be on PWM pin
// byte readLED = 13;  // Blinks with each data read

// void setup()
// {
//   Serial.begin(115200); // initialize serial communication at 115200 bits per second:

//   pinMode(pulseLED, OUTPUT);
//   pinMode(readLED, OUTPUT);

//   // Initialize sensor
//   if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) // Use default I2C port, 400kHz speed
//   {
//     Serial.println(F("MAX30105 was not found. Please check wiring/power."));
//     while (1)
//       ;
//   }

//   Serial.println(F("Attach sensor to finger with rubber band. Press any key to start conversion"));
//   while (Serial.available() == 0)
//     ; // wait until user presses a key
//   Serial.read();

//   byte ledBrightness = 60; // Options: 0=Off to 255=50mA
//   byte sampleAverage = 4;  // Options: 1, 2, 4, 8, 16, 32
//   byte ledMode = 2;        // Options: 1 = Red only, 2 = Red + IR, 3 = Red + IR + Green
//   byte sampleRate = 100;   // Options: 50, 100, 200, 400, 800, 1000, 1600, 3200
//   int pulseWidth = 411;    // Options: 69, 118, 215, 411
//   int adcRange = 4096;     // Options: 2048, 4096, 8192, 16384

//   particleSensor.setup(ledBrightness, sampleAverage, ledMode, sampleRate, pulseWidth, adcRange); // Configure sensor with these settings
// }

// void loop()
// {
//   bufferLength = 100; // buffer length of 100 stores 4 seconds of samples running at 25sps

//   // read the first 100 samples, and determine the signal range
//   for (byte i = 0; i < bufferLength; i++)
//   {
//     while (particleSensor.available() == false) // do we have new data?
//       particleSensor.check();                   // Check the sensor for new data

//     redBuffer[i] = particleSensor.getRed();
//     irBuffer[i] = particleSensor.getIR();
//     particleSensor.nextSample(); // We're finished with this sample so move to next sample

//     Serial.print(F("red="));
//     Serial.print(redBuffer[i], DEC);
//     Serial.print(F(", ir="));
//     Serial.println(irBuffer[i], DEC);
//   }

//   // calculate heart rate and SpO2 after first 100 samples (first 4 seconds of samples)
//   maxim_heart_rate_and_oxygen_saturation(irBuffer, bufferLength, redBuffer, &spo2, &validSPO2, &heartRate, &validHeartRate);

//   // Continuously taking samples from MAX30102.  Heart rate and SpO2 are calculated every 1 second
//   while (1)
//   {
//     // dumping the first 25 sets of samples in the memory and shift the last 75 sets of samples to the top
//     for (byte i = 25; i < 100; i++)
//     {
//       redBuffer[i - 25] = redBuffer[i];
//       irBuffer[i - 25] = irBuffer[i];
//     }

//     // take 25 sets of samples before calculating the heart rate.
//     for (byte i = 75; i < 100; i++)
//     {
//       while (particleSensor.available() == false) // do we have new data?
//         particleSensor.check();                   // Check the sensor for new data

//       digitalWrite(readLED, !digitalRead(readLED)); // Blink onboard LED with every data read

//       redBuffer[i] = particleSensor.getRed();
//       irBuffer[i] = particleSensor.getIR();
//       particleSensor.nextSample(); // We're finished with this sample so move to next sample

//       // send samples and calculation result to terminal program through UART
//       Serial.print(F("red="));
//       Serial.print(redBuffer[i], DEC);
//       Serial.print(F(", ir="));
//       Serial.print(irBuffer[i], DEC);

//       Serial.print(F(", HR="));
//       Serial.print(heartRate, DEC);

//       Serial.print(F(", HRvalid="));
//       Serial.print(validHeartRate, DEC);

//       Serial.print(F(", SPO2="));
//       Serial.print(spo2, DEC);

//       Serial.print(F(", SPO2Valid="));
//       Serial.println(validSPO2, DEC);
//     }

//     // After gathering 25 new samples recalculate HR and SP02
//     maxim_heart_rate_and_oxygen_saturation(irBuffer, bufferLength, redBuffer, &spo2, &validSPO2, &heartRate, &validHeartRate);
//   }
// }
