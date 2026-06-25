import serial
import time
import os

# ==============================================================================
# CẤU HÌNH CỔNG KẾT NỐI (NHÓM TỰ ĐIỀU CHỈNH)
# ==============================================================================
# Windows: Thường là 'COM3', 'COM4',... Kiểm tra trong Device Manager hoặc Arduino IDE
# Linux/Mac: Thường là '/dev/ttyUSB0' hoặc '/dev/tty.usbserial-...'
PORT_NAME = 'COM10'  
BAUD_RATE = 115200  # Đảm bảo trùng với Baudrate trong code C của Đức Tài
OUTPUT_FILE = 'somniguard_log.csv'

print("==================================================================")
print("     SOMNIGUARD - TIẾN TRÌNH GHI NHẬT KÝ DỮ LIỆU THỜI GIAN THỰC   ")
print("==================================================================")

try:
    # Mở cổng kết nối Serial với kit EFR32
    ser = serial.Serial(PORT_NAME, BAUD_RATE, timeout=1)
    # Xóa bộ đệm cũ để tránh dữ liệu rác lúc vừa cắm mạch
    ser.flushInput() 
    print(f"--> [OK] Đã kết nối thành công với {PORT_NAME} ở tốc độ {BAUD_RATE}!")
    print(f"--> Dữ liệu đo được sẽ được ghi thẳng vào file: {os.path.abspath(OUTPUT_FILE)}")
    print("--> Bấm tổ hợp phím [Ctrl + C] để DỪNG GHI và đóng file an toàn.")
    print("------------------------------------------------------------------")
    
    # Mở file CSV ở chế độ "a" (append - ghi nối tiếp vào cuối file)
    with open(OUTPUT_FILE, 'a', encoding='utf-8') as f:
        # Nếu là file mới hoàn toàn, ghi thêm dòng tiêu đề (Header) cho chuẩn y tế
        if os.stat(OUTPUT_FILE).st_size == 0:
            f.write("subjectID,timestamp,rawRed,rawIR,final_R,final_SpO2,smoothed_BPM\n")
            
        line_count = 0
        while True:
            if ser.in_waiting > 0:
                # Đọc một dòng dữ liệu thô từ cổng Serial gửi về
                raw_line = ser.readline()
                try:
                    # Giải mã byte sang chuỗi text chữ chuẩn UTF-8 và bỏ ký tự xuống dòng
                    clean_line = raw_line.decode('utf-8').strip()
                    
                    # Kiểm tra nếu dòng dữ liệu hợp lệ (chứa dấu phẩy phân tách và không phải thông báo lỗi)
                    if clean_line and ',' in clean_line and not "Không tìm thấy" in clean_line:
                        f.write(clean_line + '\n')
                        f.flush() # Ép hệ thống đẩy dữ liệu từ RAM xuống ổ cứng ngay lập tức
                        
                        line_count += 1
                        # Cứ mỗi 10 dòng thì in trạng thái ra màn hình cho Nhựt theo dõi
                        if line_count % 10 == 0:
                            print(f"[LOGGING] Đã ghi thành công {line_count} mẫu vào ổ cứng. Data hiện tại: {clean_line}")
                except Exception as decode_error:
                    # Bỏ qua nếu dính phải 1 dòng lỗi ký tự do vừa cắm dây
                    continue
                    
except KeyboardInterrupt:
    print("\n--> [STOP] Đã chủ động dừng tiến trình ghi log theo lệnh của người dùng.")
except Exception as e:
    print(f"\n❌ LỖI KẾT NỐI: Không thể mở cổng {PORT_NAME}. Hãy kiểm tra xem em có đang bật Serial Monitor của Arduino IDE không (nếu có phải TẮT ĐI vì 1 cổng COM không thể mở bởi 2 phần mềm cùng lúc).")