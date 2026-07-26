import argparse
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


def parse_args():
    parser = argparse.ArgumentParser(description="Plot raw RED/IR PPG signals")
    parser.add_argument("--time-window", type=float, default=None,
                        help="Giới hạn thời gian hiển thị theo giây, ví dụ 30")
    parser.add_argument("--time-step", type=float, default=None,
                        help="Khoảng chia trục thời gian, ví dụ 5")
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
