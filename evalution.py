import argparse
import math
import matplotlib

try:
    import tkinter  # noqa: F401
    matplotlib.use("TkAgg")
except Exception:
    matplotlib.use("Agg")

import matplotlib.pyplot as plt
from matplotlib.ticker import MultipleLocator
import pandas as pd
from pathlib import Path

BASE_DIR = Path(__file__).resolve().parent
CSV_FILES = sorted(BASE_DIR.glob("ppg_2min*.csv"), key=lambda p: p.stat().st_mtime, reverse=True)

if not CSV_FILES:
    raise FileNotFoundError("Không tìm thấy file CSV nào có tên bắt đầu bằng ppg_2min")

CSV_FILE = CSV_FILES[0]
OUTPUT_FILE = BASE_DIR / f"{CSV_FILE.stem}_raw_red_ir.png"

WINDOW_SIZE = 100
ADC_18BIT_FS = 131071
DC_MIN_BOUND = 0.25 * ADC_18BIT_FS
DC_MAX_BOUND = 0.75 * ADC_18BIT_FS


def evaluate_ppg_stability(red_buf, ir_buf, window_size=100, start=0):
    if len(red_buf) != len(ir_buf):
        raise ValueError("red_buf và ir_buf phải có cùng số lượng mẫu")

    if start < 0:
        raise ValueError("start phải >= 0")

    if len(red_buf) < start + window_size:
        raise ValueError(f"Cửa sổ bắt đầu từ {start} vượt quá độ dài dữ liệu ({len(red_buf)} mẫu)")

    red_window = red_buf[start:start + window_size]
    ir_window = ir_buf[start:start + window_size]

    result = {
        "is_stable": False,
        "pi_red": 0.0,
        "pi_ir": 0.0,
        "correlation": 0.0,
        "dc_red": 0.0,
        "dc_ir": 0.0,
    }

    sum_red = 0.0
    sum_ir = 0.0
    min_red = float("inf")
    max_red = float("-inf")
    min_ir = float("inf")
    max_ir = float("-inf")

    for i in range(window_size):
        red_val = float(red_window[i])
        ir_val = float(ir_window[i])

        sum_red += red_val
        sum_ir += ir_val

        if red_val < min_red:
            min_red = red_val
        if red_val > max_red:
            max_red = red_val
        if ir_val < min_ir:
            min_ir = ir_val
        if ir_val > max_ir:
            max_ir = ir_val

    result["dc_red"] = sum_red / window_size
    result["dc_ir"] = sum_ir / window_size

    if (result["dc_red"] < DC_MIN_BOUND or result["dc_red"] > DC_MAX_BOUND or
            result["dc_ir"] < DC_MIN_BOUND or result["dc_ir"] > DC_MAX_BOUND):
        return result

    ac_red = max_red - min_red
    ac_ir = max_ir - min_ir

    result["pi_red"] = (ac_red / result["dc_red"]) * 100.0
    result["pi_ir"] = (ac_ir / result["dc_ir"]) * 100.0

    if (result["pi_red"] < 0.1 or result["pi_red"] > 5.0 or
            result["pi_ir"] < 0.1 or result["pi_ir"] > 5.0):
        return result

    num = 0.0
    den_r = 0.0
    den_i = 0.0

    dc_red = result["dc_red"]
    dc_ir = result["dc_ir"]

    for i in range(window_size):
        diff_r = float(red_window[i]) - dc_red
        diff_i = float(ir_window[i]) - dc_ir
        num += diff_r * diff_i
        den_r += diff_r * diff_r
        den_i += diff_i * diff_i

    if den_r > 0 and den_i > 0:
        result["correlation"] = num / math.sqrt(den_r * den_i)

    if result["correlation"] >= 0.85:
        result["is_stable"] = True

    return result


def parse_args():
    parser = argparse.ArgumentParser(description="Plot raw RED/IR PPG signals")
    parser.add_argument("--time-window", type=float, default=None,
                        help="Giới hạn thời gian hiển thị theo giây, ví dụ 30")
    parser.add_argument("--time-step", type=float, default=None,
                        help="Khoảng chia trục thời gian, ví dụ 5")
    parser.add_argument("--start", type=int, default=0,
                        help="Chỉ số bắt đầu của cửa sổ 100 mẫu để đánh giá, ví dụ 200")
    parser.add_argument("--window-size", type=int, default=WINDOW_SIZE,
                        help="Số mẫu trong cửa sổ đánh giá, ví dụ 100")
    return parser.parse_args()


def main():
    args = parse_args()
    df = pd.read_csv(CSV_FILE)

    required_cols = {"Timestamp_s", "RED", "IR"}
    if not required_cols.issubset(df.columns):
        raise ValueError("CSV không đúng định dạng. Cần có các cột: Timestamp_s, RED, IR")

    time_s = df["Timestamp_s"].astype(float).to_numpy()
    red = df["RED"].astype(float).to_numpy()
    ir = df["IR"].astype(float).to_numpy()

    window_size = args.window_size
    start_idx = args.start

    if window_size <= 0:
        raise ValueError("--window-size phải > 0")

    print(f"Đang đánh giá tín hiệu từ {CSV_FILE.name}")
    eval_result = evaluate_ppg_stability(red, ir, window_size=window_size, start=start_idx)
    print(f"Cửa sổ bắt đầu từ mẫu {start_idx} ({window_size} mẫu):")
    print(f"  is_stable = {eval_result['is_stable']}")
    print(f"  dc_red    = {eval_result['dc_red']:.2f}")
    print(f"  dc_ir     = {eval_result['dc_ir']:.2f}")
    print(f"  pi_red    = {eval_result['pi_red']:.3f}")
    print(f"  pi_ir     = {eval_result['pi_ir']:.3f}")
    print(f"  correlation = {eval_result['correlation']:.4f}")

    fig, ax = plt.subplots(figsize=(14, 6))
    ax.plot(time_s, red, label="Raw RED", color="red", linewidth=1.2)
    ax.plot(time_s, ir, label="Raw IR", color="royalblue", linewidth=1.2)
    ax.set_title(f"Raw PPG signals from {CSV_FILE.name}")
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("ADC")
    ax.grid(True, linestyle=":", alpha=0.5)
    ax.legend(loc="upper right")

    if args.time_step is not None:
        ax.xaxis.set_major_locator(MultipleLocator(args.time_step))

    if args.time_window is not None:
        if args.time_window <= 0:
            raise ValueError("--time-window phải > 0")
        start = max(time_s.min(), 0)
        end = min(time_s.max(), start + args.time_window)
        ax.set_xlim(start, end)

    fig.tight_layout()

    fig.savefig(OUTPUT_FILE, dpi=200, bbox_inches="tight")

    backend = matplotlib.get_backend().lower()
    if backend == "agg":
        print(f"Đã vẽ xong và lưu file: {OUTPUT_FILE}")
    else:
        print(f"Đã lưu file: {OUTPUT_FILE}")
        print("Bạn có thể dùng chuột kéo chọn vùng, zoom, pan và xem chi tiết các đỉnh/nấc phụ.")
        plt.show(block=True)

    plt.close(fig)


if __name__ == "__main__":
    main()
