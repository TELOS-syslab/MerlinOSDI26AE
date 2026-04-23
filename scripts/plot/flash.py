"""Plot Figure 15 from flash-cache summary files.

Input:  data/flash/*.txt
Output: flash.pdf
"""
import os
import pandas as pd
import matplotlib.pyplot as plt
import matplotlib.lines as mlines
import numpy as np

# Plot configuration.

DATA_DIR = "data/flash"

ALGORITHMS = {
    "merlin":    ("Merlin",    "^",  14),   # filled triangle-up
    "s3fifo":   ("S3FIFO",   "x",  10),   # cross
    "arc":       ("ARC",      "*",  14),   # asterisk
    "cacheus":   ("Cacheus",  "s",  12),   # open square
    "flashield": ("Flashield","v",  14),   # filled triangle-down
}

# Algorithms that use open (unfilled) markers
OPEN_MARKERS = {"cacheus"}

DRAM_COLORS = {
    0.001: ("#3776c1", "DRAM size 0.1%"),   # blue
    0.01:  ("#2ba02b", "DRAM size 1%"),     # green
    0.1:   ("#d62728", "DRAM size 10%"),    # red
}

CACHE_SIZES  = [0.01, 0.1]
CACHE_LABELS = ["Cache size: 1% WSS", "Cache size: 10% WSS"]

# Load all algorithm summary files.

def load_data(data_dir: str) -> dict[str, pd.DataFrame]:
    frames = {}
    for fname in os.listdir(data_dir):
        if not fname.endswith(".txt"):
            continue
        alg = fname[:-4].lower()
        if alg not in ALGORITHMS:
            continue
        path = os.path.join(data_dir, fname)
        df = pd.read_csv(path, skipinitialspace=True)
        df.columns = df.columns.str.strip()
        frames[alg] = df
    return frames


# Build the two-panel hit-rate/write-amplification figure.

def make_figure(frames: dict[str, pd.DataFrame]) -> plt.Figure:
    fig, axes = plt.subplots(
        1, 2,
        figsize=(10, 3.6),   # wider to accommodate external legends
        sharey=False,
    )

    for ax, cache_size, title in zip(axes, CACHE_SIZES, CACHE_LABELS):
        for alg, (label, marker, ms) in ALGORITHMS.items():
            if alg not in frames:
                print(f"  [warn] missing data for '{alg}', skipping")
                continue

            sub = frames[alg][np.isclose(frames[alg]["cache_size"], cache_size)]
            if sub.empty:
                continue

            for dram_val, (color, _) in DRAM_COLORS.items():
                row = sub[np.isclose(sub["dram_ratio"], dram_val)]
                if row.empty:
                    continue

                hit_rate  = 1.0 - float(row["miss_ratio"].iloc[0])
                write_amp = float(row["write_amp"].iloc[0])

                # 'x' / '*' / '+' are line-style markers: only 'color' applies.
                # Passing edgecolors to them triggers a UserWarning.
                LINE_MARKERS = {"x", "*", "+", "1", "2", "3", "4"}
                is_open = alg in OPEN_MARKERS
                is_line_marker = marker in LINE_MARKERS

                if is_line_marker:
                    ax.scatter(
                        hit_rate, write_amp,
                        marker=marker,
                        s=ms ** 2,
                        color=color,
                        linewidths=1.5,
                        zorder=3,
                    )
                elif is_open:
                    ax.scatter(
                        hit_rate, write_amp,
                        marker=marker,
                        s=ms ** 2,
                        facecolors="none",
                        edgecolors=color,
                        linewidths=1.5,
                        zorder=3,
                    )
                else:
                    ax.scatter(
                        hit_rate, write_amp,
                        marker=marker,
                        s=ms ** 2,
                        color=color,
                        edgecolors=color,
                        linewidths=1.5,
                        zorder=3,
                    )

        # Axes cosmetics
        ax.set_title(title, fontsize=10)
        ax.set_xlabel("Hit rate", fontsize=9)
        ax.invert_yaxis()           # 0 at top, larger values below
        ax.tick_params(labelsize=8)
        ax.grid(True, linestyle="--", linewidth=0.4, alpha=0.5)
        ax.spines[["top", "right"]].set_visible(False)

    axes[0].set_ylabel("Write bytes (normalized)", fontsize=9)

    # Legends are placed outside the right subplot to avoid covering data.

    # Algorithm legend: marker shape encodes the policy.
    algo_handles = []
    for alg, (label, marker, ms) in ALGORITHMS.items():
        is_open = alg in OPEN_MARKERS
        h = mlines.Line2D(
            [], [],
            marker=marker,
            linestyle="None",
            markersize=ms * 0.7,
            color="black" if not is_open else "none",
            markeredgecolor="black",
            markerfacecolor="black" if not is_open else "none",
            label=label,
        )
        algo_handles.append(h)

    algo_leg = axes[1].legend(
        handles=algo_handles,
        loc="upper left",
        bbox_to_anchor=(1.02, 1.0),   # just outside right edge, top-aligned
        borderaxespad=0,
        fontsize=7.5,
        frameon=True,
        framealpha=0.9,
        edgecolor="#cccccc",
        handlelength=1.2,
        handletextpad=0.5,
        borderpad=0.6,
    )
    axes[1].add_artist(algo_leg)

    # DRAM legend: color encodes the DRAM/filter-size ratio.
    dram_handles = []
    for dram_val, (color, label) in DRAM_COLORS.items():
        h = mlines.Line2D(
            [], [],
            marker="o",
            linestyle="None",
            markersize=7,
            color=color,
            label=label,
        )
        dram_handles.append(h)

    axes[1].legend(
        handles=dram_handles,
        loc="lower left",
        bbox_to_anchor=(1.02, 0.0),   # just outside right edge, bottom-aligned
        borderaxespad=0,
        fontsize=7.5,
        frameon=True,
        framealpha=0.9,
        edgecolor="#cccccc",
        handlelength=1.0,
        handletextpad=0.5,
        borderpad=0.6,
    )

    plt.tight_layout(pad=1.2, w_pad=2.0)
    # Make room on the right for the external legends
    plt.subplots_adjust(right=0.78)
    return fig


# Entry point.

if __name__ == "__main__":
    if not os.path.isdir(DATA_DIR):
        raise FileNotFoundError(
            f"Data directory '{DATA_DIR}' not found. "
            "Please run this script from the project root."
        )

    frames = load_data(DATA_DIR)
    fig = make_figure(frames)
    out_path = "flash.pdf"
    fig.savefig(out_path, dpi=200, bbox_inches="tight")

    plt.show()
