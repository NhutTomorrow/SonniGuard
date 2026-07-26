import argparse
from collections import deque
import csv
from datetime import datetime
import re
import sys
import threading
import time
import matplotlib.animation as animation
import matplotlib.pyplot as plt
import serial
import serial.tools.list_ports

# ==============================================================================
# CẤU HÌNH HỆ THỐNG THU THẬP DỮ LIỆU SOMNIGUARD
# ==============================================================================
DURATION_SECONDS = 120.0  # Giới hạn thời gian thu thập: Đúng 2 phút (120s)
MAX_POINTS = 500  # Hiển thị tối đa 500 điểm gần nhất trên đồ thị (10s ở 50Hz)

time_data = deque(maxlen=MAX_POINTS)
red_data = deque(maxlen=MAX_POINTS)
ir_data = deque(maxlen=MAX_POINTS)

# Lock đồng bộ giữa luồng đọc Serial và luồng giao diện Matplotlib
data_lock = threading.Lock()

is_running = True
start_time = None


def list_com_ports():
  ports = serial.tools.list_ports.comports()
  if not ports:
    print(
        "[!] Không tìm thấy cổng COM nào. Vui lòng kiểm tra lại cáp kết nối."
    )
    return []

  print("Danh sách cổng COM đang kết nối:")
  for port in sorted(ports):
    print(f"  - {port.device}: {port.description}")
  return [p.device for p in ports]


def read_from_chip(port_name, baudrate, filename):
  global is_running, start_time

  try:
    ser = serial.Serial(port_name, baudrate, timeout=1)
    print(f"\n[*] Đã kết nối tới {port_name} ở baudrate {baudrate}")
    print(f"[*] Dữ liệu sẽ thu thập trong {DURATION_SECONDS}s (2 phút)")
    print(f"[*] File lưu trữ CSV: {filename}\n")

    start_time = time.time()

    with open(filename, mode="w", newline="") as csv_file:
      csv_writer = csv.writer(csv_file)
      csv_writer.writerow(["Timestamp_s", "RED", "IR"])  # Header chuẩn

      while is_running:
        current_time = time.time() - start_time

        # KIỂM TRA ĐIỀU KIỆN CHẶN THỜI GIAN: ĐỦ 2 PHÚT THÌ DỪNG
        if current_time >= DURATION_SECONDS:
          print(
              f"\n\n[✓] Đã hoàn thành thu thập dữ liệu đủ"
              f" {DURATION_SECONDS:.0f}s (2 phút)!"
          )
          is_running = False
          break

        line = ser.readline().decode("utf-8", errors="ignore").strip()

        if line:
          match = re.search(
              r"ir:\s*(\d+)\s+red:\s*(\d+)", line, re.IGNORECASE
          )
          if match:
            ir_val = int(match.group(1))
            red_val = int(match.group(2))

            # Ghi vào file CSV
            csv_writer.writerow([round(current_time, 4), red_val, ir_val])

            # Cập nhật mảng vẽ đồ thị
            with data_lock:
              time_data.append(current_time)
              red_data.append(red_val)
              ir_data.append(ir_val)

            # In thanh tiến trình thời gian thực ra Terminal
            print(
                f"\r[TIẾN TRÌNH] {current_time:6.1f}s /"
                f" {DURATION_SECONDS:.0f}s | RED = {red_val:7d} | IR ="
                f" {ir_val:7d}",
                end="",
                flush=True,
            )
          else:
            if line:
              print(f"\n[LOG BOARD] {line}")

  except serial.SerialException as e:
    print(f"\n[!] Lỗi kết nối Serial với {port_name}: {e}")
    is_running = False
  except Exception as e:
    print(f"\n[!] Lỗi không xác định: {e}")
    is_running = False
  finally:
    if "ser" in locals() and ser.is_open:
      ser.close()
      print("\n[*] Đã đóng cổng Serial an toàn.")
    is_running = False


def update_plot(frame, line_red, line_ir, ax1, ax2, fig):
  global is_running

  # Nếu luồng đọc đã báo kết thúc 2 phút, đóng cửa sổ GUI tự động
  if not is_running:
    plt.close(fig)
    return line_red, line_ir

  with data_lock:
    if len(time_data) == 0:
      return line_red, line_ir

    t = list(time_data)
    r = list(red_data)
    i = list(ir_data)

  # Cập nhật đường tín hiệu RED
  line_red.set_data(t, r)
  ax1.set_xlim(max(0, t[-1] - 5), t[-1] + 0.5)  # Khung nhìn trượt 5s
  if len(r) > 0:
    min_r, max_r = min(r), max(r)
    padding_r = (max_r - min_r) * 0.1 if max_r != min_r else 100
    ax1.set_ylim(min_r - padding_r, max_r + padding_r)

  # Cập nhật đường tín hiệu IR
  line_ir.set_data(t, i)
  ax2.set_xlim(max(0, t[-1] - 5), t[-1] + 0.5)
  if len(i) > 0:
    min_i, max_i = min(i), max(i)
    padding_i = (max_i - min_i) * 0.1 if max_i != min_i else 100
    ax2.set_ylim(min_i - padding_i, max_i + padding_i)

  return line_red, line_ir


def on_close(event):
  global is_running
  is_running = False
  print("\n[*] Người dùng đóng cửa sổ. Dừng thu thập...")


if __name__ == "__main__":
  parser = argparse.ArgumentParser(
      description="Thu thập dữ liệu PPG MAX30102 tự động trong 2 phút"
  )
  parser.add_argument(
      "-p", "--port", type=str, help="Cổng COM (ví dụ: COM3)", default=None
  )
  parser.add_argument("-b", "--baud", type=int, help="Baudrate", default=115200)
  parser.add_argument(
      "-f",
      "--file",
      type=str,
      help="Tên file CSV",
      default=None,
  )
  parser.add_argument(
      "--no-plot",
      action="store_true",
      help="Chỉ ghi CSV, không mở đồ thị",
  )

  args = parser.parse_args()

  filename = args.file
  if not filename:
    current_time_str = datetime.now().strftime("%Y%m%d_%H%M%S")
    filename = f"ppg_2min.csv"

  port = args.port
  if not port:
    available_ports = list_com_ports()
    if not available_ports:
      sys.exit(1)
    port = (
        input("\nNhập tên cổng COM của vi điều khiển (ví dụ: COM3): ")
        .strip()
        .upper()
    )
    if not port:
      sys.exit(1)

  # Luồng đọc Serial chạy song song
  serial_thread = threading.Thread(
      target=read_from_chip, args=(port, args.baud, filename)
  )
  serial_thread.daemon = True
  serial_thread.start()

  if args.no_plot:
    try:
      while is_running:
        time.sleep(0.1)
    except KeyboardInterrupt:
      is_running = False
      print("\n[*] Đã ngắt bởi bàn phím.")
  else:
    plt.style.use("fast")
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(10, 6))
    fig.canvas.mpl_connect("close_event", on_close)

    fig.suptitle("SomniGuard - PPG Real-time Collection (2-Minute Session)")

    ax1.set_title("RED Channel Signal")
    ax1.set_ylabel("ADC Value")
    (line_red,) = ax1.plot([], [], "r-", lw=1.2)
    ax1.grid(True)

    ax2.set_title("IR Channel Signal")
    ax2.set_xlabel("Time (s)")
    ax2.set_ylabel("ADC Value")
    (line_ir,) = ax2.plot([], [], "b-", lw=1.2)
    ax2.grid(True)

    plt.tight_layout()

    # Cập nhật đồ thị mỗi 50ms (20 FPS)
    ani = animation.FuncAnimation(
        fig,
        update_plot,
        fargs=(line_red, line_ir, ax1, ax2, fig),
        interval=50,
        blit=False,
        cache_frame_data=False,
    )

    try:
      plt.show()
    except KeyboardInterrupt:
      is_running = False

  is_running = False
  serial_thread.join(timeout=1.5)
  print(f"[*] Hoàn tất! File dataset 2 phút đã được lưu tại: {filename}")