#!/usr/bin/env python3
"""Build relative-hit-ratio CDF summaries for Figure 13.

For each trace and cache size, policies are compared against the row-wise best
policy. The script writes selected percentile points as .dat files consumed by
scripts/plot/relative_hit_ratio.py.
"""
import os
import argparse
import numpy as np
import pandas as pd

# =============================
# constants
# =============================
DROP_COLUMNS = ["Belady", "size"]

DEFAULT_POLICIES = [
    "merlin", "S3FIFO", "ARC", "Cacheus",
    "LeCaR", "LIRS", "WTinyLFU", "GDSF",
    "CAR"
]

DATASETS = [
    "twitter", "metaKV", "metaCDN", "tencentPhoto",
    "wikimedia", "tencentBlock", "systor", "fiu",
    "msr", "alibabaBlock", "cloudphysics"
]

DATASET_RENAME = [
    "Twitter", "MetaKV", "MetaCDN", "TencentPhoto",
    "Wikimedia", "Tencent", "Systor", "fiu",
    "MSR", "Alibaba", "CloudPhysics"
]

CDF_TARGET = [0.01, 0.03, 0.05, 0.1, 0.3, 0.5]


# =============================
# argument parsing
# =============================
def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input_dir", required=True)
    parser.add_argument("--output_dir", required=True)

    parser.add_argument("--policy_list", default="")
    parser.add_argument("--dataset_list", default="")

    return parser.parse_args()


# =============================
# utils
# =============================
def clean_df(df):
    """Remove non-policy columns before computing relative ratios."""
    for col in DROP_COLUMNS:
        if col in df.columns:
            df = df.drop(columns=[col])
    return df


def get_relative(df):
    """Compute hit ratio relative to the row-wise minimum miss ratio."""
    mindf = df.min(axis=1)
    result = pd.DataFrame()

    for col in df.columns:
        result[col] = (1 - df[col]) / (1 - mindf)

    return result.fillna(1)


def read_file(filepath):
    """Read one per-dataset cache-ratio CSV and return relative ratios."""
    df = pd.read_csv(filepath, header=0, index_col=0)
    df = clean_df(df)

    rel = get_relative(df)
    if len(rel) == 0:
        return None

    return rel


# =============================
# analysis functions
# =============================
def compute_cdf_tail(df, policies):
    """Compute selected lower-tail percentile stats for each policy."""
    df_sorted = df.apply(np.sort)

    if policies:
        df_sorted = df_sorted[[c for c in policies if c in df_sorted.columns]]

    df_sorted = df_sorted.T
    df_sorted.columns = range(1, len(df_sorted.columns) + 1)

    result = pd.DataFrame()

    for t in CDF_TARGET:
        # Keep only a few CDF points so the plotting script can remain simple.
        idx = int(len(df_sorted.columns) * t)
        result[f"P{int(t * 100)}"] = df_sorted[idx]

    result.index.name = "#policy"
    return result.T


def performance(df, policies):
    """Optional debug metric: fraction of traces above fixed relative thresholds."""
    df_sorted = df.apply(np.sort)

    if policies:
        df_sorted = df_sorted[[c for c in policies if c in df_sorted.columns]]

    targets = [1, 0.99, 0.95, 0.90]
    result = pd.DataFrame(columns=df_sorted.columns)

    for col in df_sorted.columns:
        for t in targets:
            result.at[f"{int(t*100)}%", col] = (df_sorted[col] >= t).sum() / len(df_sorted[col])

    return result


# =============================
# main calculation
# =============================
def calculate(input_dir, output_dir, policies, datasets):
    os.makedirs(output_dir, exist_ok=True)

    total_by_size = {}

    for dataset in os.listdir(input_dir):
        if datasets and dataset not in datasets:
            continue

        dataset_path = os.path.join(input_dir, dataset)
        if not os.path.isdir(dataset_path):
            continue

        print(f"[INFO] Processing dataset: {dataset}")

        for file in os.listdir(dataset_path):
            filepath = os.path.join(dataset_path, file)

            try:
                cachesize = float(os.path.splitext(file)[0])
            except:
                continue

            rel = read_file(filepath)
            if rel is None or len(rel) == 0:
                continue

            # Aggregate across datasets for optional total_*.dat summaries.
            total_by_size.setdefault(cachesize, []).append(rel)

            # Skip tiny datasets where percentile estimates are unstable.
            if len(rel) > 100:
                dfout = compute_cdf_tail(rel, policies)

                outfile = os.path.join(
                    output_dir,
                    f"{dataset}_{cachesize}.dat"
                )

                dfout.to_csv(outfile, sep=" ", float_format="%.3f")

    # =============================
    # Global aggregation across all selected datasets.
    # =============================
    for size, dfs in total_by_size.items():
        merged = pd.concat(dfs)

        dfout = compute_cdf_tail(merged, policies)

        outfile = os.path.join(output_dir, f"total_{size}.dat")
        dfout.to_csv(outfile, sep=" ", float_format="%.3f")

        print(f"[INFO] Total size={size}")
        _ = performance(merged, policies)


# =============================
# main
# =============================
def main():
    args = parse_args()

    policies = args.policy_list.split(",") if args.policy_list else DEFAULT_POLICIES
    datasets = args.dataset_list.split(",") if args.dataset_list else DATASETS

    calculate(
        args.input_dir,
        args.output_dir,
        policies,
        datasets,
    )


if __name__ == "__main__":
    main()
