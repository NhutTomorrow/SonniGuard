#include "read_sensorPPG.h"

// ==========================================
// BIẾN NỘI BỘ MODULE — dùng static để tránh multiple definition
// ==========================================
static MAX30105 ppgSensor;

static const unsigned long SAMPLE_PERIOD_MS = 20; // 50 Hz
static unsigned long lastSampleTime = 0;

static const int WINDOW_SIZE = 100;
static int sampleCount = 0;

// --- DSP: BỘ LỌC IIR ---
static const float HPF_ALPHA = 0.9935f; // 0.992 - 0.995
static const float LPF_BETA = 0.8f;     // 0.75 - 0.85
static float dc_track_red = 0, dc_track_ir = 0;
static float lpf_red_prev = 0, lpf_ir_prev = 0;
static float sum_sq_red = 0, sum_sq_ir = 0;

// --- KẾT QUẢ ĐẦU RA ---
static float final_R = 0.0f;
static float final_SpO2 = 98.0f;

// --- BPM ---
static long lastBeatTime = 0;
static const int BPM_FILTER_SIZE = 5;
static const int BPM_DEFAULT = 75;
static int bpmHistory[BPM_FILTER_SIZE] = {75, 75, 75, 75, 75};
static int bpmIndex = 0;
static int smoothed_BPM = 75;

// ==========================================
// KHỞI TẠO
// ==========================================
void ppgSensor_init()
{
    for (int i = 0; i < BPM_FILTER_SIZE; i++)
        bpmHistory[i] = BPM_DEFAULT;

    if (!ppgSensor.begin(Wire, I2C_SPEED_FAST)) // sửa: _sensor → ppgSensor
    {
        Serial.println("❌ Không tìm thấy MAX30105!");
        while (1)
            ;
    }
    ppgSensor.setup(60, 1, 2, 100, 411, 4096); // sửa: _sensor → ppgSensor
}

// ==========================================
// XỬ LÝ MỘT CHU KỲ MẪU
// ==========================================
void ppgSensor_process(int subjectID, QueueHandle_t iotQueue)
{
    unsigned long now = millis();
    if (now - lastSampleTime < SAMPLE_PERIOD_MS) // sửa: _lastSampleTime → lastSampleTime
        return;
    lastSampleTime = now;

    uint32_t rawRed = ppgSensor.getRed(); // sửa: _sensor → ppgSensor
    uint32_t rawIR = ppgSensor.getIR();

    if (rawIR < 50000)
    {
        sampleCount = 0; // sửa: _sampleCount → sampleCount
        return;
    }

    // ------------------------------------------
    // 1. BPM
    // ------------------------------------------
    if (checkForBeat(rawIR))
    {
        long delta = now - lastBeatTime; // sửa: _lastBeatTime
        lastBeatTime = now;
        float instant_bpm = 60000.0f / delta;

        if (instant_bpm >= 40 && instant_bpm <= 140)
        {
            if (abs(instant_bpm - smoothed_BPM) < 20) // sửa: _smoothed_BPM
            {
                bpmHistory[bpmIndex] = (int)instant_bpm; // sửa: _bpmIndex
                bpmIndex = (bpmIndex + 1) % BPM_FILTER_SIZE;

                long bpmSum = 0;
                for (int i = 0; i < BPM_FILTER_SIZE; i++)
                    bpmSum += bpmHistory[i];
                smoothed_BPM = bpmSum / BPM_FILTER_SIZE; // sửa: _smoothed_BPM
            }
        }
    }

    // ------------------------------------------
    // 2. DSP: HPF + LPF
    // ------------------------------------------
    float acRed_raw = rawRed - dc_track_red; // sửa: _dc_red → dc_track_red
    dc_track_red = (1.0f - HPF_ALPHA) * rawRed + HPF_ALPHA * dc_track_red;

    float acIR_raw = rawIR - dc_track_ir; // sửa: _dc_ir → dc_track_ir
    dc_track_ir = (1.0f - HPF_ALPHA) * rawIR + HPF_ALPHA * dc_track_ir;

    float acRed_filtered = (1.0f - LPF_BETA) * acRed_raw + LPF_BETA * lpf_red_prev; // sửa: _lpf_red_prev
    lpf_red_prev = acRed_filtered;

    float acIR_filtered = (1.0f - LPF_BETA) * acIR_raw + LPF_BETA * lpf_ir_prev; // sửa: _lpf_ir_prev
    lpf_ir_prev = acIR_filtered;

    // ------------------------------------------
    // 3. RMS Window → SpO2
    // ------------------------------------------
    sum_sq_red += acRed_filtered * acRed_filtered; // sửa: _sum_sq_red
    sum_sq_ir += acIR_filtered * acIR_filtered;
    sampleCount++;

    if (sampleCount >= WINDOW_SIZE)
    {
        float rmsRed = sqrtf(sum_sq_red / WINDOW_SIZE);
        float rmsIR = sqrtf(sum_sq_ir / WINDOW_SIZE);

        if (rmsIR > 0 && dc_track_red > 0 && dc_track_ir > 0)
        {
            final_R = (rmsRed / dc_track_red) / (rmsIR / dc_track_ir); // sửa: _final_R, _dc_red, _dc_ir
            float instant_SpO2 = 100.22f - 0.79f * final_R;            //$SpO_2 = C_1 \cdot R^2 + C_2 \cdot R + C_3$.
            if (instant_SpO2 > 100.0f)
                instant_SpO2 = 100.0f;
            final_SpO2 = 0.2f * instant_SpO2 + 0.8f * final_SpO2; // sửa: _final_SpO2
        }

        // ------------------------------------------
        // 4. Đẩy vào Queue
        // ------------------------------------------
        DataPacket pkt;
        pkt.subjectID = subjectID;
        pkt.timestamp = now;
        pkt.rValue = final_R;
        pkt.spo2 = final_SpO2;
        pkt.bpm = smoothed_BPM;
        xQueueSend(iotQueue, &pkt, 0);

        // ------------------------------------------
        // 5. In CSV ra Serial
        // ------------------------------------------
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

        // Reset cửa sổ
        sampleCount = 0;
        sum_sq_red = 0;
        sum_sq_ir = 0;
    }
}