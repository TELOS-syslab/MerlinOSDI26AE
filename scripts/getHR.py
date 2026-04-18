#!/usr/bin/env python3
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
    # drop useless columns
    for col in DROP_COLUMNS:
        if col in df.columns:
            df = df.drop(columns=[col])
    return df


def get_relative_hr(df):
    df = df[df.max(axis=1) < 1.0]
    df = df[(df < 0.99).any(axis=1)]
    if BASELINE_POLICY not in df.columns:
        return pd.DataFrame()

    base = df[BASELINE_POLICY]
    result = pd.DataFrame()

    for col in df.columns:
        result[col] = (1 - df[col]) / (1 - base)
    return result.fillna(1)


def read_file(filepath):
    df = pd.read_csv(filepath,header=0,index_col=0)
    df = clean_df(df)
    rel = get_relative_hr(df)
    if len(rel) == 0:
        return None
    score = rel.prod(axis=0) ** (1 / len(rel)) - 1

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
        df = df.sort_index() #sort by cachesize
        df.index.name = "#cachesize"
        if policies:
            df = df[[c for c in policies if c in df.columns]]
        renamed_dataset = DATASET_RENAME[DATASET.index(dataset)] if dataset in DATASET else dataset
        df.to_csv(
            os.path.join(output_dir, f"{renamed_dataset}.dat"),
            sep=" ",
            float_format="%.3f"
        )

    # =============================
    # output for cachesize
    # =============================
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

        tmp = tmp[datasets]  # reorder by dataset
        tmp = tmp.T
        #rename dataset to pretty name
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