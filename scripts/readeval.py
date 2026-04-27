#!/usr/bin/env python3
"""Convert raw cachesim outputs into per-dataset CSV summaries.

Input:
    results/<dataset>/<trace-output-file>

Output:
    dataresult/<mode>/<dataset>/<cache-ratio>.csv

Each output CSV contains one row per trace and one column per policy. The stored 
value is the byte miss ratio, which later scripts convert into hit-rate style metrics.
"""
import os
import re
import argparse
from collections import defaultdict

XTICK = ["0.003", "0.01", "0.03", "0.1", "0.2", "0.4"]

# Parse lines emitted by cachesim. The cache size can be printed as a raw byte
# count or with KiB/MiB/GiB suffixes depending on the trace.
pattern = re.compile(
    r"(.+?)\s+([A-Za-z0-9\(\)_\.\-]+)\s+cache size\s+(\d+(?:\.\d+)?(?:[KMG]iB)?),\s+\d+\s+req,\s+miss ratio\s+[\d.]+,\s+byte miss ratio\s+([\d.]+)"
)

normalize_policy_map = {
    "S3FIFO": "S3FIFO",
    "S4LRU": "S4LRU",
    "QDLP": "QDLP",
    "WTinyLFU": "WTinyLFU",
    "FIFO_Merge": "FIFOMerge",
    "ARC": "ARC",
    "merlin": "merlin"
}
enable_normalize_policy = True
# =============================
# normalize policy name
# =============================
def normalize_policy(name):
    """Map verbose cachesim policy names to the names used in plots."""
    if not enable_normalize_policy:
        return name
    for key, value in normalize_policy_map.items():
        if key in name:
            return value
    return name

# =============================
# size to bytes
# =============================
def convert_size(size):
    """Normalize cache-size strings to bytes so files sort consistently."""
    if size.endswith("KiB"):
        return int(float(size[:-3]) * 1024)
    if size.endswith("MiB"):
        return int(float(size[:-3]) * 1024**2)
    if size.endswith("GiB"):
        return int(float(size[:-3]) * 1024**3)
    return int(float(size))

# =============================
# Parse a file and return a list of (size, {policy: bmr})
# =============================
def parse_file(filepath):
    """Parse one raw result file into sorted cache-size buckets."""
    size_dict = defaultdict(dict)
    policy_set = set()
    with open(filepath) as f:
        for line in f:
            m = pattern.search(line)
            if not m:
                continue
            fullpath, policy, size, bmr = m.groups()
            policy = normalize_policy(policy)
            size = convert_size(size)
            policy_set.add(policy)
            size_dict[size][policy] = float(bmr)

    if not size_dict:
        return None

    # Sort sizes so XTICK[i] maps to the intended cache/WSS ratio.
    sorted_sizes = sorted(size_dict.keys())

    # Discard files with fewer than six configured cache ratios. These usually
    # correspond to traces that were too small or incomplete.
    if len(sorted_sizes) < 6:
        print(f"[WARN] {filepath} has only {len(sorted_sizes)} sizes, expected 6. Skipping.")
        return None
    # Keep columns aligned even if one policy is missing for a size.
    for size in sorted_sizes:
        if len(size_dict[size]) < len(policy_set):
            for policy in policy_set:
                if policy not in size_dict[size]:
                    size_dict[size][policy] = None
                    print(f"[WARN] {filepath} size {size} policy {policy} has missing value, set to None")
    return [(size, size_dict[size]) for size in sorted_sizes]

# =============================
# main function
# =============================
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input_dir", required=True)
    parser.add_argument("--output_dir", required=True)
    parser.add_argument("--normalize_policy", action="store_true", help="normalize policy names for plotting")
    args = parser.parse_args()
    global enable_normalize_policy
    enable_normalize_policy = args.normalize_policy
    bucket = defaultdict(lambda: defaultdict(list))
    for dataset in os.listdir(args.input_dir):
        print(f"[INFO] Processing dataset: {dataset}")
        dataset_path = os.path.join(args.input_dir, dataset)
        if not os.path.isdir(dataset_path):
            continue
        for file in os.listdir(dataset_path):
            file_path = os.path.join(dataset_path, file)
            if not os.path.isfile(file_path):
                continue
            parsed = parse_file(file_path)
            if parsed is None:
                print(f"[WARN] Skipping file {file_path} due to parsing issues.")
                continue
            for i, (size, policy_map) in enumerate(parsed):
                ratio = XTICK[i]

                row = {"#file": file, "size": size}
                row.update(policy_map)

                bucket[dataset][ratio].append(row)

    # =============================
    # write output
    # =============================
    for dataset in bucket:
        print(f"[INFO] Writing output for dataset: {dataset}")
        outdir = os.path.join(args.output_dir, dataset)
        os.makedirs(outdir, exist_ok=True)
        for ratio in bucket[dataset]:
            outfile = os.path.join(outdir, f"{ratio}.csv")
            # Collect all policies for this dataset/ratio before writing the
            # header so rows can have consistent columns.
            all_policies = set()
            for row in bucket[dataset][ratio]:
                all_policies.update(row.keys())

            all_policies.discard("#file")
            all_policies.discard("size")

            columns = ["#file", "size"] + sorted(all_policies)

            with open(outfile, "w") as f:
                f.write(",".join(columns) + "\n")

                for row in bucket[dataset][ratio]:
                    values = [str(row.get(col, "")) for col in columns]
                    f.write(",".join(values) + "\n")

            print(f"[DONE] {dataset} - {ratio}")

# Example:
# python3 scripts/readeval.py --input_dir ./results/eval_ignore_obj \
#   --output_dir ./dataresult/withoutobjsize --normalize_policy
if __name__ == "__main__":
    main()
