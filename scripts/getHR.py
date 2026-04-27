#!/usr/bin/env python3
"""Compute aggregate relative hit-rate improvement from readeval CSV files.

The input CSVs contain byte miss ratios by policy. This script converts miss
ratio to hit ratio, compares each policy against LRU, and writes .dat files used
by the Figure 11 and Figure 12 plotting scripts.
"""
import os
import argparse
import numpy as np
import pandas as pd

# =============================
# parse arguments
# =============================
def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input_dir", required=True)
    parser.add_argument("--output_dir", required=True)

    parser.add_argument("--policy_list", default="")
    parser.add_argument("--dataset_list", default="")

    return parser.parse_args()


# =============================
# global variables
# =============================
BASELINE_POLICY = "LRU"
DEFAULT_POLICIES = [
    "merlin", "S3FIFO", "ARC", "Cacheus", 
    "LeCaR", "LIRS", "WTinyLFU", "GDSF"
]


DROP_COLUMNS = ["Belady", "size"]

DATASET = ["twitter", "metaKV", "metaCDN", "tencentPhoto", "wikimedia", "tencentBlock", "systor", "fiu", "msr", "alibabaBlock", "cloudphysics"]
DATASET_RENAME = ["Twitter", "MetaKV", "MetaCDN", "TencentPhoto", "Wikimedia", "Tencent", "Systor", "fiu", "MSR",  "Alibaba", "CloudPhysics"]


# =============================
# preprocess data
# =============================

def clean_df(df):
    """Drop columns that are not plotted or not comparable as policies."""
    for col in DROP_COLUMNS:
        if col in df.columns:
            df = df.drop(columns=[col])
    return df


def get_relative_hr(df):
    """Return per-trace hit-rate improvement relative to the LRU baseline."""
    # Filter out degenerate rows where every policy misses almost everything or
    # where the data is too close to 1.0 to produce stable relative ratios.
    df = df[df.max(axis=1) < 1.0]
    df = df[(df < 0.99).any(axis=1)]
    if BASELINE_POLICY not in df.columns:
        return pd.DataFrame()

    base = df[BASELINE_POLICY]
    result = pd.DataFrame()

    for col in df.columns:
        # Convert miss ratio to hit ratio, then divide by LRU hit ratio.
        result[col] = (1 - df[col]) / (1 - base)
    return result.fillna(1)


def geometric_mean(df):
    """Compute geometric mean in log space to avoid product overflow."""
    invalid_rows = df[(df <= 0).any(axis=1)]
    if len(invalid_rows) > 0:
        print(f"[WARN] Dropping {len(invalid_rows)} rows with non-positive values before geomean")
        print(invalid_rows.index.tolist())
        df = df[(df > 0).all(axis=1)]

    if len(df) == 0:
        return None

    return np.exp(np.log(df).mean(axis=0))


def read_file(filepath):
    """Read one cache-ratio CSV and return a geometric-mean score per policy."""
    df = pd.read_csv(filepath,header=0,index_col=0)
    df = clean_df(df)
    rel = get_relative_hr(df)
    if len(rel) == 0:
        return None
    # Geometric mean prevents large traces from dominating the aggregate.
    geomean = geometric_mean(rel)
    if geomean is None:
        return None
    score = geomean - 1

    return score


# =============================
# calculate relative HR and output
# =============================
def calculate(input_dir, output_dir, policies, datasets):
    result = {}

    for dataset in os.listdir(input_dir):
        if datasets and dataset not in datasets:
            continue

        dataset_path = os.path.join(input_dir, dataset)
        if not os.path.isdir(dataset_path):
            continue

        result[dataset] = {}

        for file in os.listdir(dataset_path):
            filepath = os.path.join(dataset_path, file)
            print(f"[INFO] Processing file: {filepath}")
            try:
                cachesize = float(os.path.splitext(file)[0])
            except:
                continue
            score = read_file(filepath)
            if score is None:
                continue

            result[dataset][cachesize] = score

    # =============================
    # output for dataset
    # =============================
    os.makedirs(output_dir, exist_ok=True)

    for dataset in result:
        df = pd.DataFrame(result[dataset])
        df = df.T  # cachesize as rows
        df = df.sort_index() # Sort by cache size ratio.
        df.index.name = "#cachesize"
        if policies:
            df = df[[c for c in policies if c in df.columns]]
        renamed_dataset = DATASET_RENAME[DATASET.index(dataset)] if dataset in DATASET else dataset
        df.to_csv(
            os.path.join(output_dir, f"{renamed_dataset}.dat"),
            sep=" ",
            float_format="%.3f"
        )

    # Also emit files grouped by cache size so each plotting panel can load one
    # file containing all datasets.
    if not result:
        return

    sample_dataset = next(iter(result))
    for size in result[sample_dataset]:
        tmp = pd.DataFrame()

        for dataset in result:
            if size in result[dataset]:
                tmp[dataset] = result[dataset][size]

        if policies:
            tmp = tmp.loc[[c for c in policies if c in tmp.index]]

        tmp = tmp[datasets]  # Reorder by the paper's dataset order.
        tmp = tmp.T
        # Rename dataset directories to plot-friendly labels.
        tmp.index = [DATASET_RENAME[DATASET.index(d)] if d in DATASET else d for d in tmp.index]
        tmp.index.name = "#dataset"
        tmp.to_csv(
            os.path.join(output_dir, f"size_{size}.dat"),
            sep=" ",
            float_format="%.3f"
        )


# =============================
# main
# =============================
def main():
    args = parse_args()

    policies = args.policy_list.split(",") if args.policy_list else DEFAULT_POLICIES
    dataset = args.dataset_list.split(",") if args.dataset_list else DATASET

    calculate(
        args.input_dir,
        args.output_dir,
        policies,
        dataset,
    )


if __name__ == "__main__":
    main()
