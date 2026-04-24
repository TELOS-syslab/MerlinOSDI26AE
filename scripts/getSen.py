#!/usr/bin/env python3
"""Build Merlin sensitivity summaries from sweep results.

This script merges Merlin sensitivity-sweep CSVs with baseline LRU CSVs,
computes relative hit ratio against LRU, and writes compact percentile
summaries for each cache size and ghost-size slice.
"""
import os
import argparse
import numpy as np
import pandas as pd

# =============================
# constants
# =============================
DROP_COLUMNS = ["Belady", "QDLP", "ThreeLCache-BMR"]

BASELINE_POLICY = "LRU"
MERLIN_PREFIX = "merlin-"
DEFAULT_INPUT_DIR = os.path.join("dataresult", "sensitivity")
DEFAULT_BASELINE_DIR = os.path.join("dataresult", "withoutobjsize")
DEFAULT_OUTPUT_DIR = os.path.join("data", "sensitivity")

DEFAULT_DATASETS = [
    "metaKV", "metaCDN", "tencentPhoto", "wikimedia", "tencentBlock",
    "systor", "fiu", "msr", "alibabaBlock", "cloudphysics"
]

CDF_TARGET = [0.1, 0.3, 0.5, 0.7, 0.9]

# Old plotting flow dropped these parameter points.
EXCLUDED_GHOST_SIZE = 0.25
EXCLUDED_FILTER_SIZE = 0.01


# =============================
# argument parsing
# =============================
def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input_dir", default=DEFAULT_INPUT_DIR)
    parser.add_argument("--baseline_dir", default=DEFAULT_BASELINE_DIR)
    parser.add_argument("--output_dir", default=DEFAULT_OUTPUT_DIR)
    parser.add_argument("--dataset_list", default="")
    return parser.parse_args()


# =============================
# utils
# =============================
def clean_df(df):
    """Drop columns that should not participate in policy comparison."""
    for col in DROP_COLUMNS:
        if col in df.columns:
            df = df.drop(columns=[col])
    return df


def get_merlin_columns(df):
    """Keep only Merlin sweep columns from the sensitivity results."""
    merlin_cols = [col for col in df.columns if col.startswith(MERLIN_PREFIX)]
    return df[merlin_cols]


def get_baseline_columns(df):
    """Keep only the chosen baseline policy from the baseline results."""
    if BASELINE_POLICY not in df.columns:
        return pd.DataFrame(index=df.index)
    return df[[BASELINE_POLICY]]


def get_relative_hr(df):
    """Return per-trace hit ratio relative to the LRU baseline."""
    df = df[df.max(axis=1) < 1.0]
    df = df[(df < 0.99).any(axis=1)]
    if BASELINE_POLICY not in df.columns:
        return pd.DataFrame()

    base = df[BASELINE_POLICY]
    result = pd.DataFrame(index=df.index)

    for col in df.columns:
        result[col] = (1 - df[col]) / (1 - base)

    result = result.fillna(1)
    result = result.replace(0, 1)
    return result


def read_csv(filepath):
    """Read one cache-size CSV and drop unrelated columns."""
    df = pd.read_csv(filepath, header=0, index_col=0)
    df = clean_df(df)
    return df


def read_merged_file(sensitivity_path, baseline_path):
    """Merge Merlin sensitivity results with the LRU baseline."""
    sensitivity_df = get_merlin_columns(read_csv(sensitivity_path))
    baseline_df = get_baseline_columns(read_csv(baseline_path))

    if sensitivity_df.empty or baseline_df.empty:
        return None

    merged_df = sensitivity_df.join(baseline_df, how="left")

    null_rows = merged_df[merged_df.isnull().any(axis=1)]
    if len(null_rows) > 0:
        #print(
        #    f"[WARN] Dropping {len(null_rows)} rows with null values from "
        #    f"{os.path.basename(sensitivity_path)}"
        #)
        #print(null_rows.index.tolist())
        merged_df = merged_df.dropna(axis=0)

    rel = get_relative_hr(merged_df)
    if len(rel) == 0:
        return None
    return rel


def parse_merlin_params(policy_name):
    """Extract filter/staging/ghost parameters from a Merlin policy name."""
    if not policy_name.startswith(MERLIN_PREFIX):
        return None

    parts = policy_name.split("-")
    if len(parts) < 4:
        return None

    try:
        return {
            "filtersize": int(float(parts[1]) * 100),
            "stagingsize": int(float(parts[2]) * 100),
            "ghostsize": int(float(parts[3]) * 100),
        }
    except ValueError:
        return None


def compute_cdf_tail(df):
    """Compute selected percentile points for Merlin sensitivity variants."""
    merlin_cols = [c for c in df.columns if c.startswith(MERLIN_PREFIX)]
    if not merlin_cols:
        return pd.DataFrame()

    df_sorted = df[merlin_cols].apply(np.sort).T
    if df_sorted.shape[1] == 0:
        return pd.DataFrame()

    df_sorted.columns = range(1, len(df_sorted.columns) + 1)
    result = pd.DataFrame(index=df_sorted.index)

    for target in CDF_TARGET:
        idx = max(1, int(len(df_sorted.columns) * target))
        result[f"P{int(target * 100)}"] = df_sorted[idx] - 1

    params = result.index.to_series().apply(parse_merlin_params)
    result["filtersize"] = params.apply(lambda x: x["filtersize"] if x else np.nan)
    result["stagingsize"] = params.apply(lambda x: x["stagingsize"] if x else np.nan)
    result["ghostsize"] = params.apply(lambda x: x["ghostsize"] if x else np.nan)

    result = result.dropna(subset=["filtersize", "stagingsize", "ghostsize"])
    result["filtersize"] = result["filtersize"].astype(int)
    result["stagingsize"] = result["stagingsize"].astype(int)
    result["ghostsize"] = result["ghostsize"].astype(int)

    result = result[result["ghostsize"] != int(EXCLUDED_GHOST_SIZE * 100)]
    result = result[result["filtersize"] != int(EXCLUDED_FILTER_SIZE * 100)]
    result = result.sort_values(by=["ghostsize", "stagingsize", "filtersize"])
    result.index.name = "#policy"
    return result


def write_outputs(cachesize, df, output_dir):
    """Write one .dat file per ghost-size slice for a given cache size."""
    summary = compute_cdf_tail(df)
    if len(summary) == 0:
        return

    for ghostsize in summary["ghostsize"].unique():
        subset = summary[summary["ghostsize"] == ghostsize].drop(columns=["ghostsize"])
        outfile = os.path.join(output_dir, f"{cachesize}-{ghostsize}.dat")
        subset.to_csv(outfile, sep=" ", float_format="%.3f")


# =============================
# main calculation
# =============================
def calculate(input_dir, baseline_dir, output_dir, datasets):
    os.makedirs(output_dir, exist_ok=True)

    total_by_size = {}

    for dataset in os.listdir(input_dir):
        if datasets and dataset not in datasets:
            continue

        sweep_dir = os.path.join(input_dir, dataset)
        base_dir = os.path.join(baseline_dir, dataset)
        if not os.path.isdir(sweep_dir) or not os.path.isdir(base_dir):
            continue

        print(f"[INFO] Processing dataset: {dataset}")

        for file in os.listdir(sweep_dir):
            sweep_path = os.path.join(sweep_dir, file)
            baseline_path = os.path.join(base_dir, file)

            if not os.path.isfile(sweep_path) or not os.path.isfile(baseline_path):
                continue

            try:
                cachesize = float(os.path.splitext(file)[0])
            except ValueError:
                continue

            rel = read_merged_file(sweep_path, baseline_path)
            if rel is None or len(rel) == 0:
                continue

            total_by_size.setdefault(cachesize, []).append(rel)

    for size, dfs in total_by_size.items():
        merged = pd.concat(dfs, axis=0)
        write_outputs(size, merged, output_dir)


# =============================
# main
# =============================
def main():
    args = parse_args()
    datasets = args.dataset_list.split(",") if args.dataset_list else DEFAULT_DATASETS

    calculate(
        args.input_dir,
        args.baseline_dir,
        args.output_dir,
        datasets,
    )


if __name__ == "__main__":
    main()
