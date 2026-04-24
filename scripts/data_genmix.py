#!/usr/bin/env python3
"""Generate the synthetic mixed trace used by throughput experiments.

This script is the canonical trace-generator entrypoint under scripts/.

Example:
    python3 scripts/data_genmix.py -m 1000000 -n 100000000 --bin-output mix.oracleGeneral.bin
"""

import numpy as np
import struct


def gen_zipf(m: int, alpha: float, n: int, start: int = 0) -> np.ndarray:
    """Generate zipf-distributed object IDs."""
    np_tmp = np.power(np.arange(1, m + 1), -alpha)
    np_zeta = np.cumsum(np_tmp)
    dist_map = np_zeta / np_zeta[-1]
    random_values = np.random.uniform(0, 1, n)
    return np.searchsorted(dist_map, random_values) + start


def gen_repeated_range(num_objects: int, repeats: int, start: int = 0) -> np.ndarray:
    """Generate a repeated contiguous range [start, start + num_objects)."""
    object_ids = np.arange(start, start + num_objects)
    return np.tile(object_ids, repeats)


def build_workload_segments(params):
    """Build warmup and steady-state segments for the mixed workload."""
    zipf_requests = gen_zipf(params.m, params.alpha, int(params.m * params.zipf_ratio))
    small_churn_size = int(params.m * params.churn_ratio * 0.2)
    small_churn_base = params.m
    small_churn = gen_repeated_range(small_churn_size, 1, small_churn_base)

    mid_churn_size = int(params.m * params.churn_ratio * 0.8)
    mid_churn_base = small_churn_base + small_churn_size
    mid_churn = gen_repeated_range(mid_churn_size, 1, mid_churn_base)

    uniform_size = int(params.m * params.uniform_ratio)
    uniform_base = mid_churn_base + mid_churn_size
    uniform_requests = gen_repeated_range(uniform_size, params.uniform_multiplier, uniform_base)

    frequent_size = int(params.m * params.frequent_ratio)
    frequent_base = uniform_base + uniform_size
    frequent_requests = gen_repeated_range(frequent_size, params.frequent_repeats, frequent_base)

    mixed_requests = np.concatenate((uniform_requests, zipf_requests, frequent_requests))
    np.random.shuffle(mixed_requests)

    scan_size = int(params.m * params.scan_ratio)
    scan_base = frequent_base + frequent_size

    first_half = mixed_requests[:len(mixed_requests) // 2]
    second_half = mixed_requests[len(mixed_requests) // 2:]
    warmup_requests = np.concatenate((small_churn, mid_churn, small_churn, mixed_requests))
    pattern_requests = np.concatenate((small_churn, first_half, small_churn, mid_churn, second_half, mid_churn))

    return warmup_requests, pattern_requests, scan_size, scan_base


def write_requests(requests: np.ndarray, start_index: int, output_file, record_struct,
                   obj_size: int, time_span: int, total_requests: int) -> int:
    """Write requests and return the updated global request index."""
    request_index = start_index
    for obj_id in requests:
        request_index += 1
        timestamp = request_index * time_span // total_requests
        if output_file:
            output_file.write(record_struct.pack(timestamp, int(obj_id), obj_size, -2))
        else:
            print(int(obj_id))
    return request_index


if __name__ == "__main__":
    from argparse import ArgumentParser

    parser = ArgumentParser()
    parser.add_argument("-m", type=int, default=1000000, help="Number of objects")
    parser.add_argument("-n", type=int, default=100000000, help="Number of requests")
    parser.add_argument("--alpha", type=float, default=1.0, help="Zipf parameter")
    parser.add_argument("--bin-output", type=str, default="", help="Output path in oracleGeneral format")
    parser.add_argument("--obj-size", type=int, default=4000, help="Object size for binary output")
    parser.add_argument("--time-span", type=int, default=86400 * 7, help="Time span in seconds")
    parser.add_argument("--zipf-ratio", type=float, default=0.5)
    parser.add_argument("--churn-ratio", type=float, default=0.5)
    parser.add_argument("--uniform-ratio", type=float, default=0.5)
    parser.add_argument("--uniform-multiplier", type=int, default=20)
    parser.add_argument("--frequent-ratio", type=float, default=0.1)
    parser.add_argument("--frequent-repeats", type=int, default=50)
    parser.add_argument("--scan-ratio", type=float, default=0.6)
    parser.add_argument("--scan-repeats", type=int, default=1)

    arguments = parser.parse_args()

    output_file = open(arguments.bin_output, "wb") if arguments.bin_output else None
    request_record = struct.Struct("<IQIq")

    request_index = 0
    warmup_requests, pattern_requests, scan_size, scan_base = build_workload_segments(arguments)
    current_scan = gen_repeated_range(scan_size, 1, scan_base)

    request_index = write_requests(
        warmup_requests,
        request_index,
        output_file,
        request_record,
        arguments.obj_size,
        arguments.time_span,
        arguments.n,
    )
    request_index = write_requests(
        current_scan,
        request_index,
        output_file,
        request_record,
        arguments.obj_size,
        arguments.time_span,
        arguments.n,
    )

    while request_index < arguments.n:
        request_index = write_requests(
            pattern_requests,
            request_index,
            output_file,
            request_record,
            arguments.obj_size,
            arguments.time_span,
            arguments.n,
        )
        request_index = write_requests(
            current_scan,
            request_index,
            output_file,
            request_record,
            arguments.obj_size,
            arguments.time_span,
            arguments.n,
        )
        scan_base += scan_size
        current_scan = gen_repeated_range(scan_size, 1, scan_base)

    if output_file:
        output_file.close()