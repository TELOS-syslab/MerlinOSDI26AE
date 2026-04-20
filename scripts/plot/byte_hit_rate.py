import os
import re
import glob
import numpy as np
import matplotlib.pyplot as plt
from common import get_style

DATA_DIR = "data/HR/withobjsize"

# ========= 读取数据 =========
def load_dat(file_path):
    with open(file_path) as f:
        lines = f.readlines()

    headers = lines[0].strip().split()[1:]
    datasets = []
    data = []

    for line in lines[1:]:
        parts = line.strip().split()
        datasets.append(parts[0])
        values = list(map(float, parts[1:]))
        data.append(values)

    data = np.array(data)

    return datasets, headers, data


# ========= 排序（按论文） =========
def sort_data(datasets, headers, data):
    # 论文固定顺序
    target_order = [
        "CloudPhysics",
        "Alibaba",
        "MSR",
        "fiu",
        "Systor",
        "Tencent",
        "Wikimedia",
        "TencentPhoto",
        "MetaCDN",
        "MetaKV",
        "Twitter",
    ]
    

    # 建立映射：dataset -> index
    index_map = {name: i for i, name in enumerate(datasets)}

    # 按论文顺序重排
    ordered_indices = [index_map[name] for name in target_order if name in index_map]

    datasets = [datasets[i] for i in ordered_indices]
    data = data[ordered_indices]

    return datasets, data

def extract_size(f):
    # 从文件名提取 0.01 / 0.03 / 0.1
    return float(re.search(r"size_(\d+\.?\d*)", f).group(1))

# ========= 主绘图 =========
def plot():
    files = sorted(glob.glob(os.path.join(DATA_DIR, "size_*.dat")))

    # 只保留需要的三个
    wanted = ["0.01", "0.03", "0.1"]
    files = [f for f in files if any(w in f for w in wanted)]

    fig, axes = plt.subplots(1, 3, figsize=(15, 5))

    for i, file in enumerate(files):
        ax = axes[i]

        datasets, headers, data = load_dat(file)
        datasets, data = sort_data(datasets, headers, data)

        y_pos = np.arange(len(datasets))

        # 灰色条背景（论文关键）
        for y in y_pos:
            ax.axhline(y, color="gray", linewidth=1, alpha=0.6, zorder=0)

        # 画点
        for j, alg in enumerate(headers):
            alg_name = alg.lower()
            style = get_style(alg_name, ps=1.5)
            scatter_style = {
                "marker": style["marker"],
                "label": style["label"],
                "color": style["color"],
                "facecolors": style.get("markerfacecolor", style["color"]),
                "edgecolors": style.get("markeredgecolor", style["color"]),
                "linewidth": style.get("markeredgewidth", 1.0),
            }

            ax.scatter(
                data[:, j],
                y_pos,
                s=80,
                zorder=3,
                **scatter_style
            )

        # y轴
        ax.set_yticks(y_pos)
        if i == 0:
            ax.set_yticklabels(datasets)
        else:
            ax.set_yticklabels([])
        ax.invert_yaxis()

        # x轴
        ax.set_xlim(0, None)
        ax.grid(axis="x", linestyle=":", alpha=0.5)

        # 标题
        title = ""
        if "0.01" in file:
            title = "(a) Cache size: 1% WSS"
        elif "0.03" in file:
            title = "(b) Cache size: 3% WSS"
        elif "0.1" in file:
            title = "(c) Cache size: 10% WSS"
        ax.set_title(title, fontsize=13)

        # 灰底
        ax.set_facecolor("#f5f5f5")

    # legend（顶部）
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(
        handles,
        labels,
        loc="upper center",
        ncol=8,
        frameon=False,
        fontsize=11
    )

    plt.subplots_adjust(top=0.78, wspace=0.05)
    plt.savefig("byte_hit_rate.pdf", dpi=300)
    plt.show()


if __name__ == "__main__":
    plot()