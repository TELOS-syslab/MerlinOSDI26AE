"""Plot Figure 14 from CacheLib throughput summaries.

Input:
    data/throughput/wback/*.dat   - with backend latency
    data/throughput/woback/*.dat  - without backend latency
Output:
    throughput.pdf
"""
import os
import numpy as np
import matplotlib.pyplot as plt
from common import get_style

# Input directories for the two panels.
DIR_WITH = "data/throughput/wback"
DIR_WITHOUT = "data/throughput/woback"

# Algorithm order used in the paper.
ALGS = ["MERLIN", "S3FIFO", "ARC", "LRU"]

# Load one algorithm's thread-count/throughput file.
def load_file(filepath):
    threads = []
    throughput = []

    with open(filepath) as f:
        for line in f:
            parts = line.strip().split()
            # Format: <alg> <threads> <miss_ratio> <throughput_mqps>
            threads.append(int(parts[1]))
            throughput.append(float(parts[3]))

    return threads, throughput


# Plot one panel.
def plot_one(ax, directory, title):
    for alg in ALGS:
        alg_name = alg.lower()
        filepath = os.path.join(directory, f"{alg_name}.dat")

        if not os.path.exists(filepath):
            print(f"[WARN] Missing {filepath}")
            continue

        threads, thr = load_file(filepath)

        # Use evenly spaced x positions so all thread counts are readable.
        x_pos = np.arange(len(threads))

        style = get_style(alg_name, ps=1.5)

        ax.plot(
            x_pos,
            thr,
            **style,
        )

    # X-axis: thread counts.
    ax.set_xticks(x_pos)
    ax.set_xticklabels(threads)
    ax.set_xlabel("#Threads", fontsize=14)

    # Y-axis: throughput in million queries per second.
    ax.set_ylabel("Throughput (MQPS)", fontsize=14)

    # Panel title and paper-style grid.
    ax.set_title(title, fontsize=16)
    ax.grid(True, linestyle=":", alpha=0.6)

    # Remove top/right spines for a cleaner paper-style figure.
    ax.spines['top'].set_visible(False)
    ax.spines['right'].set_visible(False)


# Main plotting routine.
def main():
    fig, axes = plt.subplots(1, 2, figsize=(10, 5))

    plot_one(axes[0], DIR_WITH, "(a) With backend latency")
    plot_one(axes[1], DIR_WITHOUT, "(b) Without backend latency")

    # The first panel has all algorithms, so reuse its legend handles.
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
