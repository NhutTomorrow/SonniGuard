#include "read_sensorPPG.h"
#include <math.h>

static MAX30105 ppgSensor;

static const unsigned long SAMPLE_PERIOD_MS = 20; // Định kỳ đúng 20ms (50 Hz) chuẩn xác
static unsigned long lastSampleTime = 0;          //
static bool isFingerAttached = false;             //

// =================================================================
// KỸ THUẬT 1: BITWISE CIRCULAR BUFFER MAPPING (WINDOW_SIZE = 256)
// Tăng từ 200 lên 256 để thay thế phép chia % bằng toán tử & 0xFF cực nhanh
// =================================================================
static const int WINDOW_SIZE = 256;
static const int STRIDE_SIZE = 50; // Trượt mỗi 1 giây (50 mẫu ở 50Hz)

static int sampleCount = 0;                     //
static int strideCounter = 0;                   //
static float history_sq_red[WINDOW_SIZE] = {0}; //
static float history_sq_ir[WINDOW_SIZE] = {0};  //
static float history_dc_red[WINDOW_SIZE] = {0}; //
static float history_dc_ir[WINDOW_SIZE] = {0};  //
static int circular_index = 0;                  //

static bool isBufferFull = false; //

// --- DSP: BỘ LỌC IIR SỐ NGUYÊN/SỐ THỰC TỐI ƯU ---
static const float HPF_ALPHA = 0.9935f;         //
static const float LPF_BETA = 0.8f;             //
static float dc_track_red = 0, dc_track_ir = 0; //
static float lpf_red_prev = 0, lpf_ir_prev = 0; //

// --- HẰNG SỐ PHƯƠNG TRÌNH BẬC 2 CHUẨN ĐỊNH LƯỢNG LÂM SÀNG LÝ THUYẾT ---
static const float SPO2_A = 100.00f;
static const float SPO2_B = -2.50f; // Phép trừ đổi dấu thành cộng: 100 + 2.5*R - 18.75*R^2
static const float SPO2_C = 18.75f;

static float final_R = 0.0f;     //
static float final_SpO2 = 98.0f; //
static float buf_SPO2[5] = {98.0f, 98.0f, 98.0f, 98.0f, 98.0f};
// --- BPM: MÁY TRẠNG THÁI SĂN ĐÁY THÍCH ỨNG THEO NHỊP CHỐNG KHÓA CHẾT ---
typedef enum
{
    BPM_STATE_WAIT_FOR_DIP,
    BPM_STATE_HUNTING_VALLEY
} BpmFsmState; //

static BpmFsmState bpmState = BPM_STATE_WAIT_FOR_DIP; //
static float valley_min_val = 0.0f;                   //
static unsigned long valley_min_time = 0;             //

static long lastBeatTime = 0;                                  //
static const int BPM_FILTER_SIZE = 5;                          //
static const int BPM_DEFAULT = 75;                             //
static int bpmHistory[BPM_FILTER_SIZE] = {75, 75, 75, 75, 75}; //
static int bpmIndex = 0;                                       //
static int smoothed_BPM = 75;                                  //

static float local_acIR_min = 0.0f;
static float beat_threshold = -150.0f;         // Ngưỡng động âm ban đầu
static unsigned long samplesSinceLastBeat = 0; //

// --- KỸ THUẬT 2: MEDIAN R-FILTER BUFFER ---
// Mảng lưu trữ 5 giá trị R gần nhất để lấy phần tử trung vị, triệt tiêu gai nhiễu đột biến
static float r_history[5] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
static int r_history_index = 0;

// --- SOFTWARE AGC ENGINE TỐI ƯU TRÊN ĐƯỜNG NỀN DC ---
static uint8_t current_red_amp = 90;  //
static uint8_t current_ir_amp = 80;   //
static int agc_cooldown_counter = 20; //

// Hàm bổ trợ sắp xếp nổi bọt ngắn phục vụ lọc Trung vị
void sort_array_ascend(float *arr, int size)
{
    for (int i = 0; i < size - 1; i++)
    {
        for (int j = i + 1; j < size; j++)
        {
            if (arr[i] > arr[j])
            {
                float temp = arr[i];
                arr[i] = arr[j];
                arr[j] = temp;
            }
        }
    }
}
float apply_hampel_filter(float newspample)
{
    for (int i = 0; i < 4; ++i)
    {
        buf_SPO2[i] = buf_SPO2[i + 1];
    }
    buf_SPO2[4] = newspample;

    float sorted_x[5];
    for (int i = 0; i < 5; ++i)
        sorted_x[i] = buf_SPO2[i];
    sort_array_ascend(sorted_x, 5);

    float median_M = sorted_x[2];

    float dev[5];
    for (int i = 0; i < 5; ++i)
    {
        dev[i] = fabsf(buf_SPO2[i] - median_M);
    }
    sort_array_ascend(dev, 5);

    float mad_vaf = dev[2];
    float thresshold = 3.0f * mad_vaf;
    if (thresshold < 1.0f)
        thresshold = 1.0f;

    if (fabsf(newspample - median_M) > thresshold)
    {
        buf_SPO2[4] = median_M;
        return median_M;
    }

    return newspample;
}
void ppgSensor_init()
{
    for (int i = 0; i < BPM_FILTER_SIZE; i++)
        bpmHistory[i] = BPM_DEFAULT; //

    if (!ppgSensor.begin(Wire, I2C_SPEED_FAST)) //
    {
        Serial.println("❌ Không tìm thấy MAX30102!"); //
        while (1)
            ; //
    }

    // Thiết lập phần cứng chạy ở 50Hz sinh học đồng bộ
    ppgSensor.setup(35, 1, 2, 50, 215, 4096); //
    // THỰC CHẤT LÀ BẬT LED IR (Thư viện ghi vào 0x0C -> IR của MAX30102 sáng)
    ppgSensor.setPulseAmplitudeRed(current_red_amp);

    // THỰC CHẤT LÀ BẬT LED RED (Thư viện ghi vào 0x0D -> RED của MAX30102 sáng)
    ppgSensor.setPulseAmplitudeIR(current_red_amp);
}

void ppgSensor_process(int subjectID, QueueHandle_t iotQueue)
{
    unsigned long now = millis(); //

    if (now - lastSampleTime < SAMPLE_PERIOD_MS) //
        return;                                  //
    lastSampleTime += SAMPLE_PERIOD_MS;          //
    // thư viện SparkFun viết cho max30105 nên thứ tự led Red và IR bị ngược so với max30102
    uint32_t rawRed = ppgSensor.getRed(); //
    uint32_t rawIR = ppgSensor.getIR();   //

    // BẪY LỖI PHẦN CỨNG: PHÁT HIỆN HỞ SÁNG HOẶC NHẤC NGÓN TAY RA KHỎI CẢM BIẾN
    if (rawIR < 40000 || rawRed < 40000 || rawIR > 255000 || rawRed > 255000) //
    {
        sampleCount = 0;   //
        strideCounter = 0; //
        circular_index = 0;
        final_SpO2 = 0.0f;                 //
        isBufferFull = false;              //
        isFingerAttached = false;          //
        bpmState = BPM_STATE_WAIT_FOR_DIP; //'
        if (rawIR > 255000)
        {
            current_ir_amp -= 5;
            ppgSensor.setPulseAmplitudeIR(current_ir_amp);
        }
        if (rawRed > 255000)
        {
            current_red_amp -= 5;
            ppgSensor.setPulseAmplitudeRed(current_red_amp);
        }
        Serial.println("Hở sáng hoặc nhấn ngón tay ra khỏi cảm biến!!!!!!!!!!!!!");
        return; //
    }

    // Khởi tạo nhanh đường nền khi vừa chạm tay vào thiết bị
    if (!isFingerAttached) //
    {
        dc_track_red = (float)rawRed; //
        dc_track_ir = (float)rawIR;   //
        lpf_red_prev = 0.0f;          //
        lpf_ir_prev = 0.0f;           //
        isFingerAttached = true;      //
        lastBeatTime = now;
        samplesSinceLastBeat = 0;
        return; //
    }

    // ----------------------------------------------------------------
    // TẦNG DSP SƠ CẤP: BỘ LỌC ĐƯỜNG NỀN VÀ KHỬ NHIỄU CAO TẦN (IIR)
    // ----------------------------------------------------------------
    float acRed_raw = (float)rawRed - dc_track_red;                               //
    dc_track_red = (1.0f - HPF_ALPHA) * (float)rawRed + HPF_ALPHA * dc_track_red; //

    float acIR_raw = (float)rawIR - dc_track_ir;                               //
    dc_track_ir = (1.0f - HPF_ALPHA) * (float)rawIR + HPF_ALPHA * dc_track_ir; //

    float acRed_filtered = (1.0f - LPF_BETA) * acRed_raw + LPF_BETA * lpf_red_prev; //
    lpf_red_prev = acRed_filtered;                                                  //

    float acIR_filtered = (1.0f - LPF_BETA) * acIR_raw + LPF_BETA * lpf_ir_prev; //
    lpf_ir_prev = acIR_filtered;                                                 //

    // ----------------------------------------------------------------
    // SOFTWARE AGC ENGINE: ĐIỀU CHỈNH DÒNG LED DỰA TRÊN ĐƯỜNG NỀN DC THỰC
    // ----------------------------------------------------------------
    if (agc_cooldown_counter == 0) //
    {
        bool is_hardware_adjusted = false; //

        // Chỉnh dựa trên dc_track thay vì raw để không bị méo biên độ bởi sóng AC
        if (dc_track_red > 180000.0f && current_red_amp > 10) //
        {
            if (dc_track_red > 240000.0f)
                current_red_amp -= 5;
            else
            {
                current_red_amp--;
            }

            is_hardware_adjusted = true; //
        }
        else if (dc_track_red < 130000.0f && current_red_amp < 245) //
        {
            current_red_amp++;
            is_hardware_adjusted = true; //
        }

        if (dc_track_ir > 180000.0f && current_ir_amp > 10) //
        {
            if (dc_track_ir > 240000.0f)
                current_ir_amp -= 5;
            else
            {
                current_ir_amp--;
            }

            is_hardware_adjusted = true; //
        }
        else if (dc_track_ir < 130000.0f && current_ir_amp < 245) //
        {
            current_ir_amp++;
            is_hardware_adjusted = true; //
        }

        if (is_hardware_adjusted) //
        {
            ppgSensor.setPulseAmplitudeRed(current_red_amp); //
            ppgSensor.setPulseAmplitudeIR(current_ir_amp);   //
            agc_cooldown_counter = 20;                       // Khóa 2 giây (100 mẫu * 20ms) bảo vệ hình thái sóng AC
        }
    }
    else if (agc_cooldown_counter > 0) //
    {
        agc_cooldown_counter--; //
    }

    // ----------------------------------------------------------------
    // BPM FSM: MÁY TRẠNG THÁI SĂN ĐÁY SÓNG THÍCH ỨNG CHỐNG KẸT LOGIC
    // ----------------------------------------------------------------
    samplesSinceLastBeat++; //

    if (acIR_filtered < local_acIR_min)
        local_acIR_min = acIR_filtered; //

    switch (bpmState) //
    {
    case BPM_STATE_WAIT_FOR_DIP:                                         //
        if (acIR_filtered < beat_threshold && samplesSinceLastBeat > 15) //
        {
            bpmState = BPM_STATE_HUNTING_VALLEY; //
            valley_min_val = acIR_filtered;      //
            valley_min_time = now;               //
        }
        break; //

    case BPM_STATE_HUNTING_VALLEY:          //
        if (acIR_filtered < valley_min_val) //
        {
            valley_min_val = acIR_filtered; //
            valley_min_time = now;          //
        }

        if (acIR_filtered > (valley_min_val + 25.0f)) // Xác nhận quay đầu với biên trễ Hysteresis = 25.0f
        {
            long delta_time = valley_min_time - lastBeatTime; //

            if (delta_time > 375 && delta_time < 1500) // Bộ lọc sinh học [40, 160] BPM
            {
                float instant_bpm = 60000.0f / (float)delta_time; //

                bpmHistory[bpmIndex] = (int)instant_bpm;     //
                bpmIndex = (bpmIndex + 1) % BPM_FILTER_SIZE; //

                long bpmSum = 0;                          //
                for (int i = 0; i < BPM_FILTER_SIZE; i++) //
                    bpmSum += bpmHistory[i];              //
                smoothed_BPM = bpmSum / BPM_FILTER_SIZE;  //
            }

            // GIẢI QUYẾT DEADLOCK: Luôn chốt mốc thời gian và cập nhật ngưỡng động ngay khi có sóng quay đầu!
            lastBeatTime = valley_min_time;
            beat_threshold = valley_min_val * 0.60f; // Cập nhật đường thích ứng thích nghi nhịp mới
            local_acIR_min = 0.0f;                   // Reset bộ cứu hộ

            samplesSinceLastBeat = 0;          //
            bpmState = BPM_STATE_WAIT_FOR_DIP; //
        }
        break; //
    }

    // Tự động cứu hộ nếu mất dấu tín hiệu quá 2 giây (100 mẫu)
    if (samplesSinceLastBeat > 100) //
    {
        beat_threshold = local_acIR_min * 0.5f; //
        if (beat_threshold > -20.0f)            //
            beat_threshold = -50.0f;            // Ngưỡng sàn bảo vệ

        local_acIR_min = 0.0f;             //
        samplesSinceLastBeat = 0;          //
        lastBeatTime = now;                // Ép mốc đồng bộ trục thời gian
        bpmState = BPM_STATE_WAIT_FOR_DIP; //
    }

    // ----------------------------------------------------------------
    // CẬP NHẬT MẢNG VÒNG TRÒN VÀ TÍNH TOÁN NĂNG LƯỢNG LÂM SÀNG
    // ----------------------------------------------------------------
    history_sq_red[circular_index] = acRed_filtered * acRed_filtered; //
    history_sq_ir[circular_index] = acIR_filtered * acIR_filtered;    //
    history_dc_red[circular_index] = dc_track_red;                    //
    history_dc_ir[circular_index] = dc_track_ir;                      //

    // TOÁN TỬ BITWISE ĐỈNH CAO: Tự động wrap mảng kích thước 256 mà không tốn lệnh rẽ nhánh if hay phép toán chia (%)
    circular_index = (circular_index + 1) & 0xFF;

    sampleCount++;   //
    strideCounter++; //

    if (!isBufferFull && sampleCount >= WINDOW_SIZE) //
    {
        isBufferFull = true; //
    }

    // ĐỦ CHU KỲ 1 GIÂY -> CHỐT SỔ ĐẨY DATA LÊN TẦNG TRÊN
    if (isBufferFull && (strideCounter >= STRIDE_SIZE)) //
    {
        strideCounter = 0; //

        float sum_sq_red = 0, sum_sq_ir = 0; //
        float sum_dc_red = 0, sum_dc_ir = 0; //

        for (int i = 0; i < WINDOW_SIZE; i++) //
        {
            sum_sq_red += history_sq_red[i]; //
            sum_sq_ir += history_sq_ir[i];   //
            sum_dc_red += history_dc_red[i]; //
            sum_dc_ir += history_dc_ir[i];   //
        }

        float rmsRed = sqrtf(sum_sq_red / (float)WINDOW_SIZE); //
        float rmsIR = sqrtf(sum_sq_ir / (float)WINDOW_SIZE);   //

        float avg_dc_red = sum_dc_red / (float)WINDOW_SIZE; //
        float avg_dc_ir = sum_dc_ir / (float)WINDOW_SIZE;   //

        if (rmsIR > 0.0f && avg_dc_red > 0.0f && avg_dc_ir > 0.0f) //
        {
            // Tỷ số R tức thời (Ratio-of-Ratios)
            float instant_R = (rmsRed / avg_dc_red) / (rmsIR / avg_dc_ir); //

            // =================================================================
            // KỸ THUẬT 3: MEDIAN R-FILTER (BỘ LỌC TRUNG VỊ XẢ NHIỄU BIẾN ĐỘNG)
            // Lấy 5 mẫu gần nhất, sắp xếp để bốc phần tử ở giữa, loại bỏ nhiễu động cựa mình
            // =================================================================
            // r_history[r_history_index] = instant_R;
            // r_history_index = (r_history_index + 1) % 5;

            // final_R = r_sorted[2]; // Chốt giá trị trung vị y tế chuẩn xác làm R cuối cùng

            // =================================================================
            // KỸ THUẬT 4: PHƯƠNG TRÌNH KHỚP MẪU PARABOL BẬC 2 PHI TUYẾN CHUẨN ĐỊNH LƯỢNG
            // =================================================================
            final_R = instant_R;
            float instant_SpO2 = SPO2_A - (SPO2_B * final_R) - (SPO2_C * final_R * final_R);

            if (instant_SpO2 > 100.0f)
                instant_SpO2 = 100.0f; //
            if (instant_SpO2 < 50.0f)
                instant_SpO2 = 50.0f; //

            apply_hampel_filter(instant_SpO2);
            // Bộ lọc mượt mà quán tính sinh học, tích hợp xả nhanh (Fast convergence) khi bắt đầu gắn tay
            static bool is_first_calc = true;
            if (!isFingerAttached)
            {
                is_first_calc = true;
            }

            if (is_first_calc)
            {
                final_SpO2 = instant_SpO2; // Khóa ngay mục tiêu thực tế ở giây T0
                is_first_calc = false;
            }
            else
            {
                final_SpO2 = 0.2f * instant_SpO2 + 0.8f * final_SpO2; // Lọc thông thấp IIR
            }
        }

        // Đẩy gói dữ liệu sạch vào FreeRTOS Queue để xử lý Edge AI và BLE
        DataPacket pkt;                //
        pkt.subjectID = subjectID;     //
        pkt.timestamp = now;           //
        pkt.rValue = final_R;          //
        pkt.spo2 = final_SpO2;         //
        pkt.bpm = smoothed_BPM;        //
        xQueueSend(iotQueue, &pkt, 0); //

        // Xuất log terminal định dạng CSV sạch chuẩn để vẽ đồ thị
        Serial.print(subjectID);
        Serial.print(","); //
        Serial.print(now);
        Serial.print(","); //
        Serial.print(rawRed);
        Serial.print(","); //
        Serial.print(rawIR);
        Serial.print(","); //
        Serial.print(final_R, 4);
        Serial.print(","); //
        Serial.print(final_SpO2, 1);
        Serial.print(",");            //
        Serial.println(smoothed_BPM); //
    }
}