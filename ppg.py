import numpy as np
import matplotlib.pyplot as plt
import pandas as pd
import os

# ==============================================================================
# CẤU HÌNH HỆ THỐNG - ĐỒNG BỘ TUYỆT ĐỐI VỚI FIRMWARE CỦA ĐỨC TÀI
# ==============================================================================
FS = 50              # Tần số lấy mẫu 50Hz (Chu kỳ 20ms)
HPF_ALPHA = 0.9935   # Hệ số thông cao IIR
LPF_BETA = 0.8       # Hệ số thông thấp IIR

# File CSV lưu từ Serial Monitor của mạch thật
# Định dạng log từ code C: subjectID, timestamp, rawRed, rawIR, final_R, final_SpO2, smoothed_BPM
CSV_FILE_PATH = "somniguard_log.csv" 

# ==============================================================================
# 1. ĐỌC DỮ LIỆU THẬT HOẶC TỰ ĐỘNG TẠO ĐỂ KIỂM THỬ (FALLBACK)
# ==============================================================================
if os.path.exists(CSV_FILE_PATH):
    print(f"--> [DỮ LIỆU THẬT] Đang đọc file nhật ký từ mạch: {CSV_FILE_PATH}")
    # Đọc file và gán tên cột đúng theo thứ tự các lệnh Serial.print của Tài
    df = pd.read_csv(CSV_FILE_PATH, names=["subjectID", "timestamp", "rawRed", "rawIR", "final_R", "final_SpO2", "smoothed_BPM"])
    
    # Chuyển đổi timestamp (ms) sang Giây (s) tính từ mốc 0
    t = (df["timestamp"] - df["timestamp"].iloc[0]) / 1000.0
    raw_red = df["rawRed"].values
    raw_ir = df["rawIR"].values
else:
    print(f"--> [GIẢ LẬP] Không tìm thấy file {CSV_FILE_PATH}. Tự động tạo sóng sinh học mẫu để Học test script...")
    duration = 6.0  # Vẽ mẫu 6 giây
    t = np.arange(0, duration, 1/FS)
    
    # Sóng lý tưởng có khấc Dicrotic Notch y khoa
    f_heart = 1.2  # ~72 BPM
    ideal_ppg = -(0.5 * np.sin(2 * np.pi * f_heart * t - np.pi/2) + 
                  0.2 * np.sin(4 * np.pi * f_heart * t) + 
                  0.08 * np.sin(6 * np.pi * f_heart * t))
    
    # Ép dải biến thiên dạt nền (DC trôi) và nhiễu điện tử cao tần ngoài đời thực
    dc_base = 145000.0
    baseline_wander = 1500 * np.sin(2 * np.pi * 0.15 * t)
    noise = np.random.normal(0, 90, len(t))
    
    raw_ir = dc_base + baseline_wander + (ideal_ppg * 1200) + noise
    raw_red = (dc_base * 0.9) + baseline_wander + (ideal_ppg * 1000) + noise

# ==============================================================================
# 2. CHẠY MÔ PHỎNG DSP CHUẨN XÁC THEO TỪNG DÒNG CODE C CỦA TÀI
# ==============================================================================
# Khởi tạo nhanh đường nền giống khối logic: if (!isFingerAttached)
dc_track_red = float(raw_red[0])
dc_track_ir = float(raw_ir[0])
lpf_red_prev = 0.0
lpf_ir_prev = 0.0

# Các mảng chứa kết quả sau xử lý để vẽ đồ thị
py_dc_track_ir = []
py_acIR_filtered = []

# Duyệt qua từng mẫu để mô phỏng chính xác quá trình ngắt 20ms trên chip EFR32
for i in range(len(t)):
    curr_raw_ir = float(raw_ir[i])
    
    # --- ĐÚNG THEO CODE C: Tính toán acIR_raw trước bằng dc_track cũ ---
    acIR_raw = curr_raw_ir - dc_track_ir
    
    # --- ĐÚNG THEO CODE C: Cập nhật dc_track (Mặt nước tĩnh) cho chu kỳ sau ---
    dc_track_ir = (1.0 - HPF_ALPHA) * curr_raw_ir + HPF_ALPHA * dc_track_ir
    
    # --- ĐÚNG THEO CODE C: Lọc thông thấp LPF làm mịn gợn sóng ---
    acIR_filtered = (1.0 - LPF_BETA) * acIR_raw + LPF_BETA * lpf_ir_prev
    lpf_ir_prev = acIR_filtered
    
    # Lưu kết quả vào mảng lưu trữ
    py_dc_track_ir.append(dc_track_ir)
    py_acIR_filtered.append(acIR_filtered)

py_dc_track_ir = np.array(py_dc_track_ir)
py_acIR_filtered = np.array(py_acIR_filtered)

# ==============================================================================
# 3. TRỰC QUAN HÓA ĐỒ THỊ - CHỨNG MINH ĐỘ TRUNG THỰC HÌNH THÁI HỌC SÓNG
# ==============================================================================
fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(12, 8), sharex=True)
fig.suptitle("SOMNIGUARD - PHÂN TÍCH HÌNH THÁI HỌC TÍN HIỆU PPG (BẢO CHỨNG Y TẾ)", fontsize=13, fontweight='bold', color='#1a365d')

# --- ĐỒ THỊ 1: TÍN HIỆU THÔ VÀ ĐƯỜNG NỀN DC ---
ax1.plot(t, raw_ir, label="Tín hiệu Thô (rawIR từ cảm biến MAX30102)", color='#e53e3e', alpha=0.75, linewidth=1.2)
ax1.plot(t, py_dc_track_ir, label="Mặt nước tĩnh bám đuôi (dc_track_ir từ HPF)", color='#2b6cb0', linestyle='--', linewidth=2)
ax1.set_title("1. Tín hiệu PPG thô chưa xử lý (Bị trôi dạt đường nền do nhịp thở & nhiễu cơ học)", fontsize=11, fontweight='bold', loc='left')
ax1.set_ylabel("Biên độ ADC (Counts)", fontsize=10)
ax1.grid(True, linestyle=':', alpha=0.5)
ax1.legend(loc="upper right", frameon=True, facecolor='white')

# --- ĐỒ THỊ 2: TÍN HIỆU SẠCH AC SAU DSP CỦA NHÓM ---
ax2.plot(t, py_acIR_filtered, label="Gợn sóng AC sạch sau DSP (acIR_filtered)", color='#319795', linewidth=2)
ax2.axhline(0, color='black', linestyle='-', alpha=0.3) # Trục 0 đối xứng

# Tự động tìm vị trí đỉnh, đáy trên đồ thị sạch để chú thích cấu trúc giải phẫu học
# Giới hạn tìm kiếm trong một khoảng thời gian tĩnh ở giữa đồ thị để tránh nhiễu biên
search_start, search_end = int(len(t)*0.3), int(len(t)*0.6)
sub_ac = py_acIR_filtered[search_start:search_end]
max_local_idx = search_start + np.argmax(sub_ac)
min_local_idx = search_start + np.argmin(sub_ac)

# Vẽ mũi tên chú thích chuẩn y tế vào đồ thị
ax2.annotate('Đỉnh Tim Co\n(Systolic Peak)', xy=(t[max_local_idx], py_acIR_filtered[max_local_idx]), 
             xytext=(t[max_local_idx]-0.5, py_acIR_filtered[max_local_idx]*0.6),
             arrowprops=dict(facecolor='#2c5282', shrink=0.08, width=1, headwidth=6))

ax2.annotate('Đáy Mạch Đập\n(Systolic Valley)', xy=(t[min_local_idx], py_acIR_filtered[min_local_idx]), 
             xytext=(t[min_local_idx]+0.2, py_acIR_filtered[min_local_idx]*0.8),
             arrowprops=dict(facecolor='#742a2a', shrink=0.08, width=1, headwidth=6))

# Ước lượng vị trí khấc dicrotic notch dựa trên sườn dốc đi xuống sau đỉnh 120ms
notch_idx = max_local_idx + int(0.12 * FS)
if notch_idx < len(t):
    ax2.annotate('Đỉnh khấc phụ\n(Dicrotic Notch)', xy=(t[notch_idx], py_acIR_filtered[notch_idx]), 
                 xytext=(t[notch_idx]+0.4, py_acIR_filtered[notch_idx]*1.3),
                 arrowprops=dict(facecolor='#319795', shrink=0.08, width=1, headwidth=6))

ax2.set_title("2. Sợi sóng AC tinh khiết sau bộ lọc (Cào phẳng hoàn toàn DC, gọt sạch răng cưa)", fontsize=11, fontweight='bold', loc='left')
ax2.set_xlabel("Thời gian (Giây)", fontsize=10)
ax2.set_ylabel("Biên độ AC (Đã cào phẳng về trục 0)", fontsize=10)
ax2.grid(True, linestyle=':', alpha=0.5)
ax2.legend(loc="upper right", frameon=True, facecolor='white')

plt.tight_layout()
plt.show()