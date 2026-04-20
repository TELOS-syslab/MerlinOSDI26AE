import os
import numpy as np
import matplotlib.pyplot as plt
from common import get_style

DATA_DIR = "data/RHR/withoutobjsize"

# ========= 固定布局 =========
TRACES = ["alibabaBlock", "tencentBlock", "cloudphysics"]
WSS = ["0.01", "0.03", "0.1"]
SKIP_ALGOS = {"CAR", "GLCACHE"}

# ========= 读取数据 =========
def load_dat(file_path):
    with open(file_path, "r") as f:
        lines = f.readlines()

    headers = lines[0].strip().split()
    data = {h: [] for h in headers}
    x_labels = []

    for line in lines[1:]:
        parts = line.strip().split()
        x_labels.append(parts[0])
        values = list(map(float, parts[1:]))

        for h, v in zip(headers, values):
            data[h].append(v)

    x = [int(p[1:]) for p in x_labels]
    return x, data, headers


# ========= 主函数 =========
def plot_all():
    fig, axes = plt.subplots(3, 3, figsize=(12, 10))
    axes = axes.reshape(3, 3)

    for i, wss in enumerate(WSS):
        for j, trace in enumerate(TRACES):
            ax = axes[i][j]

            # 构造文件名（适配你的格式）
            filename = f"{trace}_{wss}.dat"
            filepath = os.path.join(DATA_DIR, filename)

            if not os.path.exists(filepath):
                print(f"[WARNING] Missing file: {filepath}")
                continue

            x, data, headers = load_dat(filepath)
            real_x = x
            x_pos = np.arange(len(real_x))

            for alg in headers:
                alg_name = alg.upper()
                if alg_name in SKIP_ALGOS:
                    continue
                style = get_style(alg_name)
                z = 5 if alg_name == "MERLIN" else 3
                ax.plot(
                    x_pos,
                    data[alg],
                    zorder=z,
                    **style,
                )

            # ===== 标题（只在第一行显示）=====
            if i == 0:
                title_map = {
                    "alibabaBlock": "Alibaba",
                    "tencentBlock": "Tencent CBS",
                    "cloudphysics": "CloudPhysics"
                }
                ax.set_title(title_map.get(trace, trace), fontsize=12)

            # ===== y轴（只在第一列）=====
            if j == 0:
                wss_label = {
                    "0.01": "Cache size: 1% WSS",
                    "0.03": "Cache size: 3% WSS",
                    "0.1": "Cache size: 10% WSS"
                }
                ax.set_ylabel(wss_label[wss])

            # ===== x轴（只在最后一行）=====
            if i == 2:
                ax.set_xlabel("Cumulative distribution (%)")

            ax.set_xticks(x_pos)
            ax.set_xticklabels(real_x)

            ax.grid(True, linestyle=":", alpha=0.6)

    # ========= 全局 legend =========
    handles, labels = axes[0][0].get_legend_handles_labels()
    fig.legend(
        handles,
        labels,
        loc="lower center",
        ncol=5,
        fontsize=10,
        frameon=False
    )

    plt.tight_layout(rect=[0, 0.05, 1, 1])
    plt.savefig("relative_hit_ratio.pdf", dpi=300)
    plt.show()


if __name__ == "__main__":
    plot_all()