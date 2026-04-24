#!/usr/bin/env python3
"""Plot Figure 17 precision-vs-hit scatter points from .dat files.

Each input file should contain columns named algo, hit, and precision. The
script writes one PDF per input unless a single output path is provided.
"""
import os
import math
import argparse
import pandas as pd
import matplotlib.pyplot as plt

plt.rcParams.update({
    "font.family": "serif",
    "font.serif": ["Times New Roman", "DejaVu Serif"],
    "font.size": 18,
    "axes.linewidth": 2.6,
    "xtick.major.width": 2.2,
    "ytick.major.width": 2.2,
    "xtick.major.size": 8,
    "ytick.major.size": 8,
    "legend.frameon": False,
})

ALGO_STYLE = {
    "merlin": {
        "label": "MERLIN",
        "color": "#9e1b12",
        "marker": "^",
        "filled": True,
        "size": 220,
    },
    "cacheus-r": {
        "label": "Cacheus-R",
        "color": "#e07028",
        "marker": "s",
        "filled": False,
        "size": 260,
    },
    "cacheus-f": {
        "label": "Cacheus-F",
        "color": "#e07028",
        "marker": "s",
        "filled": True,
        "size": 220,
    },
    "arc-r": {
        "label": "ARC-R",
        "color": "#5060c8",
        "marker": r"$\ast$",
        "filled": True,
        "size": 500,
    },
    "arc-f": {
        "label": "ARC-F",
        "color": "#5060c8",
        "marker": "o",
        "filled": False,
        "size": 280,
    },
}


ORDER = ["merlin", "cacheus-r", "cacheus-f", "arc-r", "arc-f"]


def normalize_algo(name: str) -> str:
    s = name.strip().lower()
    s = s.replace("_", "-")
    return s


def nice_upper_bound(val, step):
    return max(step, math.ceil(val / step) * step)


def load_dat(path):
    df = pd.read_csv(path, skipinitialspace=True)
    df.columns = [c.strip().lower() for c in df.columns]

    required = {"algo", "hit", "precision"}
    if not required.issubset(df.columns):
        raise ValueError(f"{path} miss, required: {required}")

    df["algo"] = df["algo"].astype(str).map(normalize_algo)
    df["hit"] = df["hit"].astype(float)
    df["precision"] = df["precision"].astype(float) * 100.0

    return df


def plot_one(dat_path, out_path=None):
    df = load_dat(dat_path)

    fig, ax = plt.subplots(figsize=(7.6, 4.2))

    # Plot one point per algorithm.
    for algo in ORDER:
        sub = df[df["algo"] == algo]
        if sub.empty:
            continue

        style = ALGO_STYLE[algo]
        x = sub["precision"].iloc[0]
        y = sub["hit"].iloc[0]

        if algo == "arc-f":
            # ARC-F is drawn as a hollow marker with a small center dot.
            ax.scatter(
                x, y,
                s=style["size"],
                marker="o",
                facecolors="none",
                edgecolors=style["color"],
                linewidths=3.6,
                label=style["label"],
                zorder=3,
            )
            ax.scatter(
                x, y,
                s=35,
                marker="o",
                color=style["color"],
                zorder=4,
            )
        elif style["filled"]:
            ax.scatter(
                x, y,
                s=style["size"],
                marker=style["marker"],
                color=style["color"],
                linewidths=2.0,
                label=style["label"],
                zorder=3,
            )
        else:
            ax.scatter(
                x, y,
                s=style["size"],
                marker=style["marker"],
                facecolors="none",
                edgecolors=style["color"],
                linewidths=3.0,
                label=style["label"],
                zorder=3,
            )

    x_max = df["precision"].max()
    y_max = df["hit"].max()

    x_upper = nice_upper_bound(x_max * 1.08, 20)
    y_upper = nice_upper_bound(y_max * 1.08, 3)

    # x_upper = max(x_upper, 40)
    x_upper = 108
    y_upper = max(y_upper, 6)

    ax.set_xlim(0, x_upper)
    ax.set_ylim(0, y_upper)

    ax.set_xticks(range(0, int(x_upper) + 1, 20))
    ax.set_yticks(range(0, int(y_upper) + 1, 3))

    ax.grid(
        True,
        linestyle=(0, (1.2, 2.4)),
        linewidth=3.0,
        color="#b0b0b0",
        zorder=0,
    )

    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)

    ax.tick_params(axis="both", direction="in", pad=12)

    ax.set_xlabel("Precision(%)", labelpad=10)
    ax.set_ylabel("Average Hit", labelpad=10)

    ax.legend(
        loc="center left",
        bbox_to_anchor=(1.02, 0.5),
        handletextpad=0.8,
        borderaxespad=0.4,
        markerscale=1.0,
    )

    plt.tight_layout()

    if out_path is None:
        base = os.path.splitext(dat_path)[0]
        out_path = base + ".pdf"

    fig.savefig(out_path, bbox_inches="tight")
    plt.close(fig)
    print(f"saved to {out_path}")


def main():
    parser = argparse.ArgumentParser(
        description="Plot precision-hit scatter from .dat files"
    )
    parser.add_argument(
        "inputs",
        nargs="+",
        help="input .dat file(s), e.g. data/precision/fiu.dat"
    )
    parser.add_argument(
        "-o", "--output",
        default=None,
        help="output file path (only valid when a single input is given)"
    )
    args = parser.parse_args()

    if args.output is not None and len(args.inputs) > 1:
        raise ValueError("Input invalid.")

    for dat_path in args.inputs:
        if len(args.inputs) == 1:
            plot_one(dat_path, args.output)
        else:
            plot_one(dat_path, None)


if __name__ == "__main__":
    main()