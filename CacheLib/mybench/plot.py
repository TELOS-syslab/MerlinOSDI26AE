#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os
import re
import numpy as np
import matplotlib.pyplot as plt

# 输入目录（放 txt 文件的目录）
INPUT_DIR = "./results/fiu_expand"   # 你可以改成你自己的目录，比如 "."

# 固定的线程数刻度
THREAD_TICKS = [1, 2, 4, 8, 16, 24, 32, 48, 64, 128]


# 解析单个 txt 文件，返回 {algo: { 'threads':[], 'cache_mb':[], 'throughput':[], 'miss_ratio':[] }}
def parse_txt_file(path):
    data = {
        "flex":   {"threads": [], "cache_mb": [], "throughput": [], "miss_ratio": []},
        "s3fifo": {"threads": [], "cache_mb": [], "throughput": [], "miss_ratio": []},
    }

    # ############## flex 1 threads, cache size 4000 MB, hashpower 21
    header_re = re.compile(
        r"^#+\s*(\w+)\s+(\d+)\s+threads,\s+cache size\s+(\d+)\s+MB",
        re.IGNORECASE
    )

    # cachelib ... runtime 1.43 sec, 7795814 requests, throughput 5.80 MQPS, miss ratio 0.0627
    data_re = re.compile(
        r"runtime\s+([\d.]+)\s+sec,\s+(\d+)\s+requests,\s+throughput\s+([\d.]+)\s+MQPS,\s+miss ratio\s+([\d.]+)",
        re.IGNORECASE
    )

    current_algo = None
    current_threads = None
    current_cache_mb = None

    with open(path, "r") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue

            # 头行
            m_header = header_re.match(line)
            if m_header:
                algo = m_header.group(1).lower()   # flex / s3fifo
                threads = int(m_header.group(2))
                cache_mb = int(m_header.group(3))
                current_algo = algo
                current_threads = threads
                current_cache_mb = cache_mb
                continue

            # 数据行
            m_data = data_re.search(line)
            if m_data and current_algo in data:
                runtime = float(m_data.group(1))      # 目前没用，但保留
                requests = int(m_data.group(2))       # 目前没用，但保留
                throughput = float(m_data.group(3))
                miss_ratio = float(m_data.group(4))

                data[current_algo]["threads"].append(current_threads)
                data[current_algo]["cache_mb"].append(current_cache_mb)
                data[current_algo]["throughput"].append(throughput)
                data[current_algo]["miss_ratio"].append(miss_ratio)

                current_algo = None
                current_threads = None
                current_cache_mb = None

    # 按线程数排序
    for algo in data:
        threads = data[algo]["threads"]
        if not threads:
            continue
        cache_mb = data[algo]["cache_mb"]
        thr = data[algo]["throughput"]
        miss = data[algo]["miss_ratio"]

        zipped = list(zip(threads, cache_mb, thr, miss))
        zipped.sort(key=lambda x: x[0])

        data[algo]["threads"]     = [z[0] for z in zipped]
        data[algo]["cache_mb"]    = [z[1] for z in zipped]
        data[algo]["throughput"]  = [z[2] for z in zipped]
        data[algo]["miss_ratio"]  = [z[3] for z in zipped]

    return data


def plot_flex_vs_s3fifo(basename, data, out_dir):
    """
    在同一张图中对比 flex 和 s3fifo：
    - x: 固定为 THREAD_TICKS（类别轴，等距）
    - 左 y: throughput (MQPS)，柱状图
    - 右 y: miss ratio，折线图
    """
    flex_threads = data["flex"]["threads"]
    s3_threads   = data["s3fifo"]["threads"]

    if not flex_threads and not s3_threads:
        return

    # 把原始数据变成 dict: 线程数 -> 数值，方便按 THREAD_TICKS 对齐
    flex_thr_map  = {t: v for t, v in zip(data["flex"]["threads"],
                                          data["flex"]["throughput"])}
    flex_miss_map = {t: v for t, v in zip(data["flex"]["threads"],
                                          data["flex"]["miss_ratio"])}
    s3_thr_map    = {t: v for t, v in zip(data["s3fifo"]["threads"],
                                          data["s3fifo"]["throughput"])}
    s3_miss_map   = {t: v for t, v in zip(data["s3fifo"]["threads"],
                                          data["s3fifo"]["miss_ratio"])}

    # 用统一的 x 位置：0,1,2,...，并按 THREAD_TICKS 顺序取值
    x_idx = np.arange(len(THREAD_TICKS))

    import math
    flex_thr  = [flex_thr_map.get(t, math.nan)  for t in THREAD_TICKS]
    flex_miss = [flex_miss_map.get(t, math.nan) for t in THREAD_TICKS]
    s3_thr    = [s3_thr_map.get(t, math.nan)    for t in THREAD_TICKS]
    s3_miss   = [s3_miss_map.get(t, math.nan)   for t in THREAD_TICKS]

    fig, ax1 = plt.subplots()

    ax1.set_title(f"{basename} - flex vs s3fifo")
    ax1.set_xlabel("Threads")
    ax1.set_ylabel("Throughput (MQPS)")

    # bar 宽度（类别间距是 1，这里取 0.35 比较合适）
    width = 0.35

    # throughput: 柱状图（左轴）
    # flex 在左偏一点，s3 在右偏一点
    ax1.bar(
        x_idx - width / 2.0,
        flex_thr,
        width=width,
        label="flex throughput",
        color="tab:blue",
        alpha=0.8,
    )
    ax1.bar(
        x_idx + width / 2.0,
        s3_thr,
        width=width,
        label="s3fifo throughput",
        color="tab:orange",
        alpha=0.8,
    )

    # 右轴 miss ratio：折线图
    ax2 = ax1.twinx()
    ax2.set_ylabel("Miss ratio")

    ax2.plot(
        x_idx,
        flex_miss,
        marker="o",
        linestyle="--",
        label="flex miss ratio",
        color="tab:green",
    )
    ax2.plot(
        x_idx,
        s3_miss,
        marker="s",
        linestyle="-.",
        label="s3fifo miss ratio",
        color="tab:red",
    )

    # 设置 x 轴刻度为类别：1 2 4 8 16 24 32 48 64 128
    ax1.set_xticks(x_idx)
    ax1.set_xticklabels([str(t) for t in THREAD_TICKS])

    # 合并两个坐标轴的图例
    handles1, labels1 = ax1.get_legend_handles_labels()
    handles2, labels2 = ax2.get_legend_handles_labels()
    ax1.legend(handles1 + handles2, labels1 + labels2, loc="best")

    fig.tight_layout()

    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, f"{basename}_flex_vs_s3fifo.png")
    plt.savefig(out_path, dpi=200)
    plt.close(fig)
    print(f"Saved: {out_path}")


def main():
    # 输出图片放到 figs 子目录
    output_dir = os.path.join(INPUT_DIR, "figs")
    os.makedirs(output_dir, exist_ok=True)

    for fname in os.listdir(INPUT_DIR):
        if not fname.endswith(".txt"):
            continue

        path = os.path.join(INPUT_DIR, fname)
        if not os.path.isfile(path):
            continue

        print(f"Processing {path} ...")
        data = parse_txt_file(path)
        basename, _ = os.path.splitext(fname)

        plot_flex_vs_s3fifo(basename, data, output_dir)


if __name__ == "__main__":
    main()
