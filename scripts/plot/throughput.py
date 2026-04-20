import os
import numpy as np
import matplotlib.pyplot as plt
from common import get_style

# ========= 路径 =========
DIR_WITH = "data/throughput/wback"
DIR_WITHOUT = "data/throughput/woback"

# ========= 算法顺序（论文顺序） =========
ALGS = ["MERLIN", "S3FIFO", "ARC", "LRU"]

# ========= 读取单个算法 =========
def load_file(filepath):
    threads = []
    throughput = []

    with open(filepath) as f:
        for line in f:
            parts = line.strip().split()
            # 格式: merlin 1 0.0970 0.72
            threads.append(int(parts[1]))
            throughput.append(float(parts[3]))

    return threads, throughput


# ========= 画一个子图 =========
def plot_one(ax, directory, title):
    for alg in ALGS:
        alg_name = alg.lower()
        filepath = os.path.join(directory, f"{alg_name}.dat")

        if not os.path.exists(filepath):
            print(f"[WARN] Missing {filepath}")
            continue

        threads, thr = load_file(filepath)

        # 👉 等间距 x（关键，和论文一致）
        x_pos = np.arange(len(threads))

        style = get_style(alg_name, ps=1.5)

        ax.plot(
            x_pos,
            thr,
            **style,
        )

    # ===== x轴 =====
    ax.set_xticks(x_pos)
    ax.set_xticklabels(threads)
    ax.set_xlabel("#Threads", fontsize=14)

    # ===== y轴 =====
    ax.set_ylabel("Throughput (MQPS)", fontsize=14)

    # ===== 标题 =====
    ax.set_title(title, fontsize=16)
    ax.grid(True, linestyle=":", alpha=0.6)

    # ===== 去掉右上边框（论文风）=====
    ax.spines['top'].set_visible(False)
    ax.spines['right'].set_visible(False)


# ========= 主函数 =========
def main():
    fig, axes = plt.subplots(1, 2, figsize=(10, 5))

    plot_one(axes[0], DIR_WITH, "(a) With backend latency")
    plot_one(axes[1], DIR_WITHOUT, "(b) Without backend latency")

    # ===== legend（只取一个）=====
    handles, labels = axes[0].get_legend_handles_labels()
    axes[0].legend(
        loc="upper left",
        bbox_to_anchor=(0.05, 0.95),
        frameon=False,
        fontsize=14,
        handlelength=2.5
    )

    plt.subplots_adjust(wspace=0.25, top=0.82)
    plt.savefig("throughput.pdf", dpi=300)
    plt.show()


if __name__ == "__main__":
    main()