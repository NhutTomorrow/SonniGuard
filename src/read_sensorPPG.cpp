#include "read_sensorPPG.h"

static MAX30105 ppgSensor;

static const unsigned long SAMPLE_PERIOD_MS = 20; // Định kỳ đúng 20ms (50 Hz)
static unsigned long lastSampleTime = 0;
static bool isFingerAttached = false;

static const int WINDOW_SIZE = 200; // Cửa sổ 4 giây
static const int STRIDE_SIZE = 50;  // Trượt mỗi 1 giây

static int sampleCount = 0;
static int strideCounter = 0;
static float history_sq_red[WINDOW_SIZE] = {0};
static float history_sq_ir[WINDOW_SIZE] = {0};
static float history_dc_red[WINDOW_SIZE] = {0};
static float history_dc_ir[WINDOW_SIZE] = {0};
static int circular_index = 0;

static bool isBufferFull = false;

// --- DSP: BỘ LỌC IIR ---
static const float HPF_ALPHA = 0.9935f;
static const float LPF_BETA = 0.8f;
static float dc_track_red = 0, dc_track_ir = 0; // gia tri mat nuoc hay duong nen
static float lpf_red_prev = 0, lpf_ir_prev = 0;

// --- KẾT QUẢ ĐẦU RA ---
static float final_R = 0.0f;
static float final_SpO2 = 98.0f;

// --- BPM (MÁY TRẠNG THÁI CHUẨN Y KHOA) ---
typedef enum
{
    BPM_STATE_WAIT_FOR_DIP,
    BPM_STATE_HUNTING_VALLEY
} BpmFsmState;

static BpmFsmState bpmState = BPM_STATE_WAIT_FOR_DIP;
static float valley_min_val = 0.0f;       // Giá trị đáy thấp nhất trong chu kỳ
static unsigned long valley_min_time = 0; // Thời điểm đạt đáy chuẩn xác

static long lastBeatTime = 0;
static const int BPM_FILTER_SIZE = 5;
static const int BPM_DEFAULT = 75;
static int bpmHistory[BPM_FILTER_SIZE] = {75, 75, 75, 75, 75};
static int bpmIndex = 0;
static int smoothed_BPM = 75;

static float acIR_max = 0;
static float acIR_min = 0;
static float beat_threshold = 0;
static unsigned long lastBeatSampleCount = 0;

void ppgSensor_init()
{
    for (int i = 0; i < BPM_FILTER_SIZE; i++)
        bpmHistory[i] = BPM_DEFAULT;

    if (!ppgSensor.begin(Wire, I2C_SPEED_FAST))
    {
        Serial.println("❌ Không tìm thấy MAX30102!");
        while (1)
            ;
    }

    // ĐÃ ĐÚNG: Cấu hình phần cứng chạy chuẩn 50Hz đồng bộ phần mềm
    ppgSensor.setup(35, 1, 2, 50, 215, 4096);
    ppgSensor.setPulseAmplitudeRed(45);
    ppgSensor.setPulseAmplitudeIR(65);
}

void ppgSensor_process(int subjectID, QueueHandle_t iotQueue)
{
    unsigned long now = millis();

    // ĐÃ ĐÚNG: Chống trôi pha lưới thời gian bằng cộng tiến
    if (now - lastSampleTime < SAMPLE_PERIOD_MS)
        return;
    lastSampleTime += SAMPLE_PERIOD_MS;

    uint32_t rawRed = ppgSensor.getRed();
    uint32_t rawIR = ppgSensor.getIR();

    // KIỂM TRA TAY RA KHỎI CẢM BIẾN
    if (rawIR < 50000)
    {
        sampleCount = 0;
        strideCounter = 0;
        circular_index = 0;
        isBufferFull = false;
        isFingerAttached = false;
        bpmState = BPM_STATE_WAIT_FOR_DIP; // Reset luôn trạng thái FSM nhịp tim
        return;
    }
    // Serial.print(subjectID);
    // Serial.print(",");
    // Serial.print(now);
    // Serial.print(",");
    // Serial.print(rawRed);
    // Serial.print(",");
    // Serial.print(rawIR);
    // Serial.print(",");
    // Serial.print(final_R, 4);
    // Serial.print(",");
    // Serial.print(final_SpO2, 1);
    // Serial.print(",");
    // Serial.println(smoothed_BPM);
    // KHỞI TẠO NHANH ĐƯỜNG NỀN DC
    if (!isFingerAttached)
    {
        dc_track_red = (float)rawRed;
        dc_track_ir = (float)rawIR;
        lpf_red_prev = 0.0f;
        lpf_ir_prev = 0.0f;
        isFingerAttached = true;
        return;
    }

    // ------------------------------------------
    // 1. DSP: HPF + LPF
    // ------------------------------------------
    float acRed_raw = (float)rawRed - dc_track_red;
    dc_track_red = (1.0f - HPF_ALPHA) * (float)rawRed + HPF_ALPHA * dc_track_red;

    float acIR_raw = (float)rawIR - dc_track_ir;
    dc_track_ir = (1.0f - HPF_ALPHA) * (float)rawIR + HPF_ALPHA * dc_track_ir;

    float acRed_filtered = (1.0f - LPF_BETA) * acRed_raw + LPF_BETA * lpf_red_prev;
    lpf_red_prev = acRed_filtered;

    float acIR_filtered = (1.0f - LPF_BETA) * acIR_raw + LPF_BETA * lpf_ir_prev;
    lpf_ir_prev = acIR_filtered;

    // ------------------------------------------
    // 2. VÁ LỖI: BPM (MÁY TRẠNG THÁI SĂN ĐÁY SÓNG ĐỒNG BỘ 50HZ)
    // ------------------------------------------
    lastBeatSampleCount++;

    if (acIR_filtered > acIR_max)
        acIR_max = acIR_filtered;
    if (acIR_filtered < acIR_min)
        acIR_min = acIR_filtered;

    // Cứ sau mỗi 1 giây (50 mẫu), cập nhật đường ngưỡng động dựa trên đáy thực tế
    if (strideCounter == 0)
    {
        beat_threshold = acIR_min * 0.55f; // Đặt ngưỡng an toàn bằng 55% biên độ âm đáy
        acIR_max = 0;
        acIR_min = 0;
    }

    switch (bpmState)
    {
    case BPM_STATE_WAIT_FOR_DIP:
        // Nếu sóng lao xuống sâu hơn đường ngưỡng động VÀ đã qua thời gian chặn nhiễu (300ms = 15 mẫu)
        if (acIR_filtered < beat_threshold && lastBeatSampleCount > 15)
        {
            bpmState = BPM_STATE_HUNTING_VALLEY;
            valley_min_val = acIR_filtered; // Tạm khóa mẫu này làm đáy
            valley_min_time = now;
        }
        break;

    case BPM_STATE_HUNTING_VALLEY:
        // Nếu sóng vẫn tiếp tục lao xuống sâu hơn, cập nhật mốc đáy thực tế mới
        if (acIR_filtered < valley_min_val)
        {
            valley_min_val = acIR_filtered;
            valley_min_time = now; // Khóa chặt thời gian thực của ĐÁY THẬT y khoa
        }

        // DẤU HIỆU QUAY ĐẦU: Sóng đi ngược lên vượt qua đáy một biên độ trễ (Hysteresis = 20.0f) để chặn nhiễu răng cưa
        float hysteresis_margin = 20.0f;
        if (acIR_filtered > (valley_min_val + hysteresis_margin))
        {
            // CHỐT MỤC TIÊU: Điểm valley_min_time chính là đáy thật!
            long delta = valley_min_time - lastBeatTime;

            // Bộ lọc cửa sổ sinh học (Chấp nhận nhịp tim từ 40 đến 160 BPM)
            if (delta > 375 && delta < 1500)
            {
                lastBeatTime = valley_min_time;
                float instant_bpm = 60000.0f / (float)delta;

                bpmHistory[bpmIndex] = (int)instant_bpm;
                bpmIndex = (bpmIndex + 1) % BPM_FILTER_SIZE;

                long bpmSum = 0;
                for (int i = 0; i < BPM_FILTER_SIZE; i++)
                    bpmSum += bpmHistory[i];
                smoothed_BPM = bpmSum / BPM_FILTER_SIZE;
            }

            // Quay xe về trạng thái chờ xung máu tiếp theo
            lastBeatSampleCount = 0;
            bpmState = BPM_STATE_WAIT_FOR_DIP;
        }
        break;
    }

    // ------------------------------------------
    // 3. Cập Nhật Mảng Vòng Tròn (Sliding Window)
    // ------------------------------------------
    history_sq_red[circular_index] = acRed_filtered * acRed_filtered;
    history_sq_ir[circular_index] = acIR_filtered * acIR_filtered;
    history_dc_red[circular_index] = dc_track_red;
    history_dc_ir[circular_index] = dc_track_ir;

    circular_index = (circular_index + 1) % WINDOW_SIZE;

    sampleCount++;
    strideCounter++;

    if (!isBufferFull && sampleCount >= WINDOW_SIZE)
    {
        isBufferFull = true;
    }

    // ĐỦ 1 GIÂY -> CHỐT SỔ TÍNH TOÁN (ĐÃ ĐÚNG CHUẨN Y TẾ)
    if (isBufferFull && (strideCounter >= STRIDE_SIZE))
    {
        strideCounter = 0;

        float sum_sq_red = 0, sum_sq_ir = 0;
        float sum_dc_red = 0, sum_dc_ir = 0;

        for (int i = 0; i < WINDOW_SIZE; i++)
        {
            sum_sq_red += history_sq_red[i];
            sum_sq_ir += history_sq_ir[i];
            sum_dc_red += history_dc_red[i];
            sum_dc_ir += history_dc_ir[i];
        }

        float rmsRed = sqrtf(sum_sq_red / (float)WINDOW_SIZE);
        float rmsIR = sqrtf(sum_sq_ir / (float)WINDOW_SIZE);

        float avg_dc_red = sum_dc_red / (float)WINDOW_SIZE;
        float avg_dc_ir = sum_dc_ir / (float)WINDOW_SIZE;

        Serial.print("rmsRed:");
        Serial.print(rmsRed);
        Serial.print(",");
        Serial.print("dcRed:");
        Serial.print(avg_dc_red);
        Serial.print(",");
        Serial.print("rmsIR:");
        Serial.print(rmsIR);
        Serial.print(",");
        Serial.print("dcIR:");
        Serial.println(avg_dc_ir);

        if (rmsIR > 0.0f && avg_dc_red > 0.0f && avg_dc_ir > 0.0f)
        {
            final_R = (rmsRed / avg_dc_red) / (rmsIR / avg_dc_ir);
            float instant_SpO2 = 108.0f - 19.23f * final_R;

            if (instant_SpO2 > 100.0f)
                instant_SpO2 = 100.0f;
            if (instant_SpO2 < 50.0f)
                instant_SpO2 = 50.0f;

            final_SpO2 = 0.2f * instant_SpO2 + 0.8f * final_SpO2;
        }

        // 4. Đẩy vào Queue truyền thông FreeRTOS
        DataPacket pkt;
        pkt.subjectID = subjectID;
        pkt.timestamp = now;
        pkt.rValue = final_R;
        pkt.spo2 = final_SpO2;
        pkt.bpm = smoothed_BPM;
        xQueueSend(iotQueue, &pkt, 0);

        // 5. In CSV ra Serial monitor
        Serial.print(subjectID);
        Serial.print(",");
        Serial.print(now);
        Serial.print(",");
        Serial.print(rawRed);
        Serial.print(",");
        Serial.print(rawIR);
        Serial.print(",");
        Serial.print(final_R, 4);
        Serial.print(",");
        Serial.print(final_SpO2, 1);
        Serial.print(",");
        Serial.println(smoothed_BPM);
    }
}