# common.py
# -*- coding: utf-8 -*-

"""
Shared plotting styles for the artifact figures.

The paper figures were originally styled with gnuplot. This module centralizes
the equivalent Matplotlib styles so all Python plotting scripts use consistent
colors, markers, and line widths.

Gnuplot usage examples:
    with linespoints ls 1 pt 9 ps 1
    with linespoints ls 7 ps 1
    with linespoints ls 1 pt 9 ps 1.5

Matplotlib counterparts:
    ls  -> predefined style dict below
    pt  -> marker
    ps  -> markersize
"""

# -----------------------------
# Global defaults
# -----------------------------
DEFAULT_LINEWIDTH = 2.0
DEFAULT_MARKERSIZE = 6.0   # gnuplot ps 1  ~ matplotlib markersize 6
DEFAULT_HOLLOW_MARKER_EDGEWIDTH = 1.8


def gnuplot_ps_to_markersize(ps: float) -> float:
    """
    Rough conversion:
        gnuplot ps 1   -> matplotlib markersize 6
        gnuplot ps 1.5 -> matplotlib markersize 9
        gnuplot ps 2   -> matplotlib markersize 12
    """
    return 6.0 * ps


# -----------------------------
# Gnuplot line-style mapping
# -----------------------------
# Based on your figure + plotting order:
# ls 1 -> Merlin
# ls 2 -> S3-FIFO
# ls 3 -> ARC
# ls 4 -> Cacheus
# ls 5 -> LeCaR
# ls 6 -> GDSF
# ls 7 -> WTinyLFU
# ls 8 -> LIRS
#
# Notes:
# - pt 9 in your gnuplot is used for Merlin; here we map it to '^'
# - hollow markers are represented with markerfacecolor='none'
# - filled markers use markerfacecolor=color

LS_STYLE = {
    1: {  # Merlin
        "label": "Merlin",
        "color": "#8B0000",
        "marker": "^",
        "linestyle": "-",
        "linewidth": DEFAULT_LINEWIDTH,
        "markerfacecolor": "#8B0000",
        "markeredgecolor": "#8B0000",
        "markeredgewidth": 1.4,
    },
    2: {  # S3-FIFO
        "label": "S3-FIFO",
        "color": "green",
        "marker": "x",
        "linestyle": "--",
        "linewidth": DEFAULT_LINEWIDTH,
        "markeredgecolor": "green",
        "markeredgewidth": 1.8,
    },
    3: {  # ARC
        "label": "ARC",
        "color": "royalblue",
        "marker": r"$\ast$",
        "linestyle": "--",
        "linewidth": DEFAULT_LINEWIDTH,
        "markerfacecolor": "royalblue",
        "markeredgecolor": "royalblue",
        "markeredgewidth": 1.2,
    },
    4: {  # Cacheus
        "label": "Cacheus",
        "color": "orange",
        "marker": "s",
        "linestyle": "--",
        "linewidth": DEFAULT_LINEWIDTH,
        "markerfacecolor": "none",
        "markeredgecolor": "orange",
        "markeredgewidth": DEFAULT_HOLLOW_MARKER_EDGEWIDTH,
    },
    5: {  # LeCaR
        "label": "LeCaR",
        "color": "#118AB2",
        "marker": "s",
        "linestyle": "-",
        "linewidth": DEFAULT_LINEWIDTH,
        "markerfacecolor": "#118AB2",
        "markeredgecolor": "#118AB2",
        "markeredgewidth": 1.4,
    },
    6: {  # GDSF
        "label": "GDSF",
        "color": "darkgreen",
        "marker": "o",
        "linestyle": "-.",
        "linewidth": DEFAULT_LINEWIDTH,
        "markerfacecolor": "none",
        "markeredgecolor": "darkgreen",
        "markeredgewidth": DEFAULT_HOLLOW_MARKER_EDGEWIDTH,
    },
    7: {  # WTinyLFU
        "label": "WTinyLFU",
        "color": "teal",
        "marker": "o",
        "linestyle": "-",
        "linewidth": DEFAULT_LINEWIDTH,
        "markerfacecolor": "teal",
        "markeredgecolor": "teal",
        "markeredgewidth": 1.4,
    },
    8: {  # LIRS
        "label": "LIRS",
        "color": "skyblue",
        "marker": "^",
        "linestyle": "-",
        "linewidth": 0.8,
        "markerfacecolor": "none",
        "markeredgecolor": "skyblue",
        "markeredgewidth": 0.8,
    },
}


# -----------------------------
# Algorithm-name mapping
# -----------------------------
ALGO_TO_LS = {
    "merlin": 1,
    "sys": 1,

    "s3fifo": 2,
    "sthreefifo": 2,

    "arc": 3,

    "cacheus": 4,
    "lru": 4,

    "lecar": 5,
    "gdsf": 6,
    "wtinylfu": 7,
    "lirs": 8,
}


LEGEND_ORDER = [
    "merlin",
    "arc",
    "s3fifo",
    "cacheus",
    "lecar",
    "lirs",
    "wtinylfu",
    "gdsf",
]


def get_style_by_ls(ls: int, ps: float = 1.0, pt=None):
    """
    Return a matplotlib-style kwargs dict from gnuplot-like inputs.

    Parameters
    ----------
    ls : int
        gnuplot line style number
    ps : float
        gnuplot point size
    pt : optional
        ignored by default unless you want to override marker manually
    """
    if ls not in LS_STYLE:
        raise KeyError(f"Unknown line style ls={ls}")

    style = dict(LS_STYLE[ls])
    style["markersize"] = gnuplot_ps_to_markersize(ps)

    if pt is not None:
        pass

    return style


def get_style(name: str, ps: float = 1.0):
    """
    Return a Matplotlib style dictionary by algorithm name.

    Args:
        name: Policy name as it appears in .dat files.
        ps: Marker-size multiplier matching gnuplot's point-size convention.

    Example:
        get_style("merlin", ps=1.5)
    """
    key = name.strip().lower()
    if key not in ALGO_TO_LS:
        raise KeyError(f"Unknown algorithm/style name: {name}")

    ls = ALGO_TO_LS[key]
    return get_style_by_ls(ls, ps=ps)


def plot_with_style(ax, x, y, name: str, ps: float = 1.0, label=None, **kwargs):
    """
    Helper wrapper for ax.plot(...)

    Example:
        plot_with_style(ax, x, y, "merlin", ps=1.5)
    """
    style = get_style(name, ps=ps)
    if label is not None:
        style["label"] = label

    style.update(kwargs)
    return ax.plot(x, y, **style)


def make_legend_handles():
    """
    Create legend handle configs in a unified order.
    Usually used together with manual plotting or custom legends.
    """
    handles = []
    labels = []
    for name in LEGEND_ORDER:
        style = get_style(name, ps=1.0)
        handles.append(style)
        labels.append(style["label"])
    return handles, labels
