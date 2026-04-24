#!/usr/bin/env python3
"""Plot sensitivity results for filter, staging, and ghost size sweeps."""
import os
import re
import glob
import argparse

import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from matplotlib.transforms import blended_transform_factory


PERCENTILES = ["P10", "P30", "P50", "P70", "P90"]

# Keep the sweep dimensions in the order used by the artifact figure.
FILTER_ORDER = [5, 10, 15]
STAGING_ORDER = [1, 5, 10]

# Marker styles for each latency percentile.
STYLE = {
    "P10": {
        "marker": "^",
        "color": "#a61c13",
        "facecolor": "#a61c13",
        "edgecolor": "#a61c13",
        "size": 220,
        "linewidth": 1.8,
    },
    "P30": {
        "marker": "x",
        "color": "#3c9d2f",
        "facecolor": "none",
        "edgecolor": "#3c9d2f",
        "size": 220,
        "linewidth": 3.0,
    },
    "P50": {
        "marker": r"$\ast$",
        "color": "#5566cc",
        "facecolor": "#5566cc",
        "edgecolor": "#5566cc",
        "size": 320,
        "linewidth": 2.2,
    },
    "P70": {
        "marker": "s",
        "color": "#f0742a",
        "facecolor": "none",
        "edgecolor": "#f0742a",
        "size": 240,
        "linewidth": 3.0,
    },
    "P90": {
        "marker": "s",
        "color": "#4a90b8",
        "facecolor": "#4a90b8",
        "edgecolor": "#4a90b8",
        "size": 240,
        "linewidth": 1.5,
    },
}


def parse_filename(path):
    """Return the cache size and ghost size encoded in names like 0.1-200.dat."""
    base = os.path.basename(path)
    m = re.match(r"([0-9.]+)-([0-9]+)\.dat$", base)
    if not m:
        return None, None
    return m.group(1), int(m.group(2))


def load_dat(path):
    """Read one whitespace-delimited sensitivity data file."""
    df = pd.read_csv(
        path,
        comment="#",
        sep=r"\s+",
        header=None,
        names=["policy", "P10", "P30", "P50", "P70", "P90", "filtersize", "stagingsize"]
    )

    for p in PERCENTILES:
        df[p] = df[p].astype(float)

    df["filtersize"] = df["filtersize"].astype(int)
    df["stagingsize"] = df["stagingsize"].astype(int)
    return df


def collect_cache_data(data_dir, cache_size="0.1"):
    """Collect all data files for a single cache size, keyed by ghost size."""
    data = {}
    for path in sorted(glob.glob(os.path.join(data_dir, "*.dat"))):
        csize, ghost = parse_filename(path)
        if csize is None:
            continue
        if csize == str(cache_size):
            data[ghost] = load_dat(path)

    if not data:
        raise FileNotFoundError(f"no {cache_size}-*.dat files found under {data_dir}")

    return dict(sorted(data.items(), key=lambda x: x[0]))


def build_layout(ghost_sizes):
    """Build x positions for the Ghost size -> Staging size -> Filter size layout."""
    pos = {}                 # (ghost, staging, filter) -> x
    stage_centers = []       # (ghost, stage, center)
    ghost_spans = {}         # ghost -> (xmin, xmax, center)
    xticks = []
    xticklabels = []

    x = 0.0
    stage_gap = 0.9
    ghost_gap = 1.6

    for gi, ghost in enumerate(ghost_sizes):
        ghost_start = x

        for si, stage in enumerate(STAGING_ORDER):
            stage_start = x

            for filt in FILTER_ORDER:
                pos[(ghost, stage, filt)] = x
                xticks.append(x)
                xticklabels.append(str(filt))
                x += 1.0

            stage_end = x - 1.0
            stage_center = (stage_start + stage_end) / 2.0
            stage_centers.append((ghost, stage, stage_center))

            if si != len(STAGING_ORDER) - 1:
                x += stage_gap

        ghost_end = x - 1.0
        ghost_center = (ghost_start + ghost_end) / 2.0
        ghost_spans[ghost] = (ghost_start - 0.5, ghost_end + 0.5, ghost_center)

        if gi != len(ghost_sizes) - 1:
            x += ghost_gap

    return pos, stage_centers, ghost_spans, xticks, xticklabels


def get_value(df, filt, stage, metric):
    row = df[(df["filtersize"] == filt) & (df["stagingsize"] == stage)]
    if row.empty:
        return None
    return float(row.iloc[0][metric])


def make_legend_handles():
    handles = []
    for p in PERCENTILES:
        st = STYLE[p]
        if p == "P30":
            h = Line2D(
                [0], [0],
                marker=st["marker"],
                color=st["color"],
                linestyle="None",
                markersize=14,
                markeredgewidth=2.8,
                label=p
            )
        elif p == "P70":
            h = Line2D(
                [0], [0],
                marker=st["marker"],
                color=st["edgecolor"],
                markerfacecolor="none",
                linestyle="None",
                markersize=14,
                markeredgewidth=2.8,
                label=p
            )
        else:
            h = Line2D(
                [0], [0],
                marker=st["marker"],
                color=st["edgecolor"],
                markerfacecolor=st["facecolor"],
                linestyle="None",
                markersize=14,
                markeredgewidth=1.8,
                label=p
            )
        handles.append(h)
    return handles


def plot_sensitivity(cache_size, data, output_dir, ylim=(-0.02, 0.12)):
    ghost_sizes = sorted(data.keys())
    pos, stage_centers, ghost_spans, xticks, xticklabels = build_layout(ghost_sizes)

    fig, ax = plt.subplots(figsize=(14, 6.4))

    # Set the y-axis range before placing labels with an axes-fraction transform.
    ax.set_ylim(*ylim)

    # Highlight the staging-size-5 band for each ghost-size group.
    for ghost in ghost_sizes:
        xs = [pos[(ghost, 5, f)] for f in FILTER_ORDER if (ghost, 5, f) in pos]
        if xs:
            ax.axvspan(min(xs) - 0.5, max(xs) + 0.5, color="0.90", zorder=0)

    # Draw one scatter series per percentile.
    for p in PERCENTILES:
        xs, ys = [], []
        for ghost in ghost_sizes:
            df = data[ghost]
            for stage in STAGING_ORDER:
                for filt in FILTER_ORDER:
                    y = get_value(df, filt, stage, p)
                    if y is not None:
                        xs.append(pos[(ghost, stage, filt)])
                        ys.append(y)

        st = STYLE[p]
        if p == "P30":
            ax.scatter(
                xs, ys,
                marker=st["marker"],
                s=st["size"],
                c=st["color"],
                linewidths=st["linewidth"],
                zorder=3
            )
        elif p == "P70":
            ax.scatter(
                xs, ys,
                marker=st["marker"],
                s=st["size"],
                facecolors="none",
                edgecolors=st["edgecolor"],
                linewidths=st["linewidth"],
                zorder=3
            )
        else:
            ax.scatter(
                xs, ys,
                marker=st["marker"],
                s=st["size"],
                facecolors=st["facecolor"],
                edgecolors=st["edgecolor"],
                linewidths=st["linewidth"],
                zorder=3
            )

    # Axis ticks and labels.
    ax.set_xticks(xticks)
    ax.set_xticklabels(xticklabels, rotation=90, fontsize=22)
    ax.set_ylabel("Hit rate improvement", fontsize=24)
    # ax.set_xlabel("Bottom: Filter size, Upper: Staging size", fontsize=28, fontweight="bold")

    # Tick styling.
    ax.tick_params(axis="y", labelsize=16)
    ax.tick_params(axis="x", pad=2)

    # Grid lines help align points across the grouped x-axis.
    ax.grid(axis="y", linestyle=(0, (2, 4)), color="0.65", linewidth=1.1)
    ax.grid(axis="x", linestyle=(0, (2, 4)), color="0.72", linewidth=0.9)
    ax.set_axisbelow(True)

    # Draw vertical separators around each ghost-size group.
    for ghost in ghost_sizes:
        xmin, xmax, _ = ghost_spans[ghost]
        ax.axvline(x=xmin, color="black", linewidth=1.0, zorder=1)
        ax.axvline(x=xmax, color="black", linewidth=1.0, zorder=1)

    # Place the top staging-size labels manually to avoid twiny tick overlap.
    trans = blended_transform_factory(ax.transData, ax.transAxes)
    for ghost, stage, center in stage_centers:
        ax.text(
            center, 1.02, str(stage),
            transform=trans,
            ha="center", va="bottom",
            fontsize=20
        )

    # Label each ghost-size group below the filter-size tick labels.
    for ghost in ghost_sizes:
        _, _, center = ghost_spans[ghost]
        ax.text(
            center, -0.14, f"Ghost size {ghost}%",
            transform=trans,
            ha="center", va="top",
            fontsize=22
        )

    # Keep the legend above the staging labels with extra vertical separation.
    handles = make_legend_handles()
    fig.legend(
        handles=handles,
        labels=PERCENTILES,
        loc="upper center",
        bbox_to_anchor=(0.5, 0.99),
        ncol=5,
        frameon=False,
        fontsize=22,
        columnspacing=2.0,
        handletextpad=0.6
    )

    # Match the open-frame style used by the other plot scripts.
    ax.spines["right"].set_visible(False)

    # Leave a larger top margin so the legend does not crowd the 1/5/10 labels.
    fig.subplots_adjust(left=0.10, right=0.99, top=0.76, bottom=0.30)

    os.makedirs(output_dir, exist_ok=True)
    pdf_path = os.path.join(output_dir, f"sensitivity-{cache_size}.pdf")

    fig.savefig(pdf_path, bbox_inches="tight")
    plt.close(fig)

    print(f"[OK] saved: {pdf_path}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--data-dir", default="data/sensitivity")
    parser.add_argument("--output-dir", default="./")
    parser.add_argument(
        "--cache-size",
        default="0.1",
    )
    parser.add_argument(
        "--ylim",
        nargs=2,
        type=float,
        default=[-0.02, 0.12],
        metavar=("YMIN", "YMAX")
    )
    args = parser.parse_args()

    data = collect_cache_data(args.data_dir, args.cache_size)
    plot_sensitivity(args.cache_size, data, args.output_dir, tuple(args.ylim))


if __name__ == "__main__":
    main()
