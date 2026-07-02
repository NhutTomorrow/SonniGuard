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

// Công thức: SpO2 = A - B*R - C*R^2
static const float SPO2_A = 100.0f;
static const float SPO2_B = -2.50f;
static const float SPO2_C = 18.75f;
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

static float local_acIR_min = 0.0f;
static float beat_threshold = -150.0f; // Khởi tạo an toàn (giá trị âm cho đáy sóng)
static unsigned long samplesSinceLastBeat = 0;

// biến sử dụng để ACG LED Red và IR
static uint8_t current_red_amp = 60; // Dòng khởi tạo ban đầu cho LED RED
static uint8_t current_ir_amp = 70;  // Dòng khởi tạo ban đầu cho LED IR
static int agc_cooldown_counter = 0; // Bộ trễ khóa tuyến tính chống quét liên tục
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
    ppgSensor.setPulseAmplitudeRed(current_red_amp);
    ppgSensor.setPulseAmplitudeIR(current_ir_amp);
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
    if (rawIR < 50000 || rawRed < 50000)
    {
        sampleCount = 0;
        strideCounter = 0;
        circular_index = 0;
        isBufferFull = false;
        isFingerAttached = false;
        bpmState = BPM_STATE_WAIT_FOR_DIP; // Reset luôn trạng thái FSM nhịp tim
        Serial.println("❌ Tín hiệu PPG bị bão hòa hoặc mất dấu! Vui lòng đặt lại ngón tay.");
        return;
    }

    if (!isFingerAttached)
    {
        dc_track_red = (float)rawRed;
        dc_track_ir = (float)rawIR;
        lpf_red_prev = 0.0f;
        lpf_ir_prev = 0.0f;
        isFingerAttached = true;
        Serial.println("✅ Đã phát hiện tín hiệu PPG! Vui lòng giữ nguyên vị trí.");
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
    samplesSinceLastBeat++;

    // Liên tục theo dõi điểm cực tiểu âm của cửa sổ hiện tại để tự cứu hộ nếu mất dấu tín hiệu
    if (acIR_filtered < local_acIR_min)
    {
        local_acIR_min = acIR_filtered;
    }

    switch (bpmState)
    {
    case BPM_STATE_WAIT_FOR_DIP:
        // Sóng lao xuống sâu hơn đường ngưỡng động (giá trị âm) VÀ qua thời gian chặn nhiễu sinh học (300ms = 15 mẫu)
        if (acIR_filtered < beat_threshold && samplesSinceLastBeat > 15)
        {
            bpmState = BPM_STATE_HUNTING_VALLEY;
            valley_min_val = acIR_filtered;
            valley_min_time = now;
        }
        break;

    case BPM_STATE_HUNTING_VALLEY:
        // Nếu sóng vẫn đang tiếp tục đi xuống âm sâu hơn, khóa mục tiêu đáy mới
        if (acIR_filtered < valley_min_val)
        {
            valley_min_val = acIR_filtered;
            valley_min_time = now;
        }

        // DẤU HIỆU QUAY ĐẦU CHẮC CHẮN Y KHOA: Sóng đi lên vượt qua đáy một khoảng trễ chống răng cưa (Hysteresis = 25.0f)
        if (acIR_filtered > (valley_min_val + 25.0f))
        {
            long delta_time = valley_min_time - lastBeatTime;

            // BỘ LỌC SINH HỌC LÂM SÀNG: Chỉ chấp nhận tính toán nếu nằm trong dải [40, 160] BPM
            if (delta_time > 375 && delta_time < 1500)
            {
                float instant_bpm = 60000.0f / (float)delta_time;

                // Bộ lọc trung bình trượt làm mịn BPM
                bpmHistory[bpmIndex] = (int)instant_bpm;
                bpmIndex = (bpmIndex + 1) % BPM_FILTER_SIZE;

                long bpmSum = 0;
                for (int i = 0; i < BPM_FILTER_SIZE; i++)
                    bpmSum += bpmHistory[i];
                smoothed_BPM = bpmSum / BPM_FILTER_SIZE;
            }

            // =================================================================
            // SỬA LỖI CHÍ MẠCH 1: ĐƯA ĐOẠN ĐỒNG BỘ THỜI GIAN VÀ NGƯỠNG RA NGOÀI LỌC SINH HỌC
            // Đảm bảo nhịp tiếp theo sẽ được tính toán dựa trên mốc thời gian của nhịp vừa tìm thấy này!
            // =================================================================
            lastBeatTime = valley_min_time;
            beat_threshold = valley_min_val * 0.60f; // Cập nhật ngưỡng động thích ứng beat-by-beat
            local_acIR_min = 0.0f;                   // Reset bộ cứu hộ

            samplesSinceLastBeat = 0;
            bpmState = BPM_STATE_WAIT_FOR_DIP;
        }
        break;
    }

    // Tự động cứu hộ: Nếu quá 2 giây không tìm thấy nhịp (Do người dùng di chuyển tay hoặc vừa khởi động)
    if (samplesSinceLastBeat > 100)
    {
        beat_threshold = local_acIR_min * 0.5f; // Ép hạ ngưỡng xuống để bắt lại sóng
        if (beat_threshold > -20.0f)
            beat_threshold = -50.0f; // Ngưỡng sàn bảo vệ

        local_acIR_min = 0.0f;
        samplesSinceLastBeat = 0;

        // =================================================================
        // SỬA LỖI CHÍ MẠCH 2: ĐỒNG BỘ LẠI MỐC THỜI GIAN KHI HỆ THỐNG TỰ CỨU HỘ
        // Ép trục thời gian đồng bộ về hiện tại để tránh cú nhảy Delta_time ở nhịp kế tiếp
        // =================================================================
        lastBeatTime = now;

        bpmState = BPM_STATE_WAIT_FOR_DIP;
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

        if (rmsIR > 0.0f && avg_dc_red > 0.0f && avg_dc_ir > 0.0f)
        {
            final_R = (rmsRed / avg_dc_red) / (rmsIR / avg_dc_ir);
            float instant_SpO2 = SPO2_A - (SPO2_B * final_R) - (SPO2_C * final_R * final_R);

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

        if (agc_cooldown_counter == 0)
        {
            bool is_hardware_adjusted = false;
            if (rawRed > 180000 && current_red_amp > 5)
            {
                current_red_amp -= 2;
                is_hardware_adjusted = true;
            }
            else if (rawRed < 145000 && current_red_amp < 250)
            {
                current_red_amp += 2;
                is_hardware_adjusted = true;
            }
            if (rawIR > 180000 && current_ir_amp > 5)
            {
                current_ir_amp -= 2;
                is_hardware_adjusted = true;
            }
            else if (rawIR < 145000 && current_ir_amp < 250)
            {
                current_ir_amp += 2;
                is_hardware_adjusted = true;
            }
            if (is_hardware_adjusted)
            {
                ppgSensor.setPulseAmplitudeRed(current_red_amp);
                ppgSensor.setPulseAmplitudeIR(current_ir_amp);

                agc_cooldown_counter = 3; // cho led 3 chu ki de on dinh
            }
        }
        else if (agc_cooldown_counter > 0)
        {
            agc_cooldown_counter--;
        }

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
        Serial.print(avg_dc_ir);
        Serial.print(",");
        Serial.print("Amp Led RED:");
        Serial.print(current_red_amp);
        Serial.print(", ");
        Serial.print("Amp Led IR");
        Serial.println(current_ir_amp);
    }
}