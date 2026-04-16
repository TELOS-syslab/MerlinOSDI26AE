#!/usr/bin/env python3
import os
import subprocess
import time
import argparse
import psutil
import threading
from concurrent.futures import ThreadPoolExecutor, wait, FIRST_COMPLETED
import re

# =============================
# global state
# =============================
file_lock = threading.Lock()
oom_happen = False

# pid -> start_time
running_tasks = {}
running_lock = threading.Lock()

MEM_SAFE_GB = 100

# -----------------------------
# config: policies to run
# -----------------------------
policy_list = [
    "merlin"
]
policy_name = [
    "merlin-0.10-0.05-1.00-32-1.0"
]

# =============================
# functions
# =============================
def get_available_memory_gb():
    return psutil.virtual_memory().available / (1024**3)

# =============================
# oom, kill the shortest job until memory is safe
# =============================
def kill_shortest_jobs_until_safe():
    print("[OOM] Trigger kill by runtime policy")

    while True:
        free_gb = get_available_memory_gb()
        if free_gb >= MEM_SAFE_GB:
            print(f"[OOM] Memory recovered: {free_gb:.1f} GB")
            return

        with running_lock:
            if len(running_tasks) <= 1:
                print("[OOM] limited running tasks")
                return

            # 按运行时间排序（最短优先）
            now = time.time()
            sorted_tasks = sorted(
                running_tasks.items(),
                key=lambda x: now - x[1]["start_time"]
            )

            pid, info = sorted_tasks[0]

        try:
            print(f"[KILL] PID={pid}, runtime={now - info['start_time']:.1f}s")
            psutil.Process(pid).kill()
        except Exception as e:
            print(f"[WARN] kill failed {pid}: {e}")

        time.sleep(1)

# =============================
# 
# =============================
def extract_important_lines(stdout_lines):
    """
    extract important lines from cachesim output, unify cache size format
    supports:
        590
        1968
        2GiB
        7GiB
    """
    important_lines = []
    # match lines like:
    # tracename policy cache size 2GiB, 12345 req, miss ratio 0.123, byte miss ratio 0.456
    pattern = re.compile(
        r"(.+?)\s+([A-Z0-9\(\)_\-]+)\s+cache size\s+(\d+(?:\.\d+)?(?:[KMG]iB)?),\s+(\d+)\s+req,\s+miss ratio\s+([\d.]+),\s+byte miss ratio\s+([\d.]+).*"
    )

    for line in stdout_lines:
        m = pattern.search(line)
        if m:
            trace_file, policy, cache_size, req_count, miss_ratio, byte_miss_ratio = m.groups()
            cache_size = cache_size.strip()  # strip extra spaces
            # convert cache size to a unified format (e.g., bytes) if needed
            cache_size_bytes = convert_to_bytes(cache_size)
            important_lines.append(
                f"{trace_file.strip()} {policy.strip()} cache size {cache_size_bytes}, {req_count} req, miss ratio {miss_ratio}, byte miss ratio {byte_miss_ratio}"
            )
    return important_lines


def convert_to_bytes(size_str):
    """
    convert cache size string to bytes integer
    2GiB -> 2*1024*1024*1024
    590  -> 590
    """
    size_str = size_str.strip()
    unit_multipliers = {"KiB": 1024, "MiB": 1024**2, "GiB": 1024**3}
    for unit, mult in unit_multipliers.items():
        if size_str.endswith(unit):
            return int(float(size_str[:-len(unit)]) * mult)
    # return original number if no unit
    return int(float(size_str))

# =============================
# resource check
# =============================
def get_available_workers():
    cpu_total = psutil.cpu_count() * 0.8
    cpu_usage = psutil.cpu_percent(interval=0.5)

    cpu_free = cpu_total * (1 - cpu_usage/100) // 2

    mem_free = get_available_memory_gb()
    mem_based = int(mem_free // 20) - 5

    if oom_happen:
        mem_based = max(0, mem_based // 5)

    workers = int(min(cpu_free, mem_based))
    if workers <= 0 and len(running_tasks) == 0:
        workers = 1
    return max(0, workers)

# -----------------------------
# OOM detection
# -----------------------------
def is_oom(stderr):
    if not stderr:
        return False
    s = stderr.lower()
    return "killed" in s or "out of memory" in s or "oom" in s

# -----------------------------
# policy todo list
# recover from existing result file, only run the policies that are not done yet
# -----------------------------
def get_policy_todo(result_file):
    done = set()
    if not os.path.exists(result_file):
        pass
    else:
        with open(result_file) as f:
            for line in f:
                parts = line.strip().split()
                if len(parts) > 1:
                    done.add(parts[1])
    #--eviction-params filter-size-ratio=0.10,staging-size-ratio=0.05,ghost-size-ratio=1.00,epoch-update=32,sckech-scale=1.0
    filter_size_ratio = "0.10"
    staging_size_ratio = "0.05"
    ghost_size_ratio = "1.00"
    epoch_update = "32"
    sketch_scale = "1.0"
    epoch_tuple = (1,2,4,8,16,32,64,128)
    sketch_touple = ("0.50","1.00","2.00","4.00")
    todo = []
    for epoch in epoch_tuple:
        for sketch in sketch_touple:
            eparams = f"filter-size-ratio={filter_size_ratio},staging-size-ratio={staging_size_ratio},ghost-size-ratio={ghost_size_ratio},epoch-update={epoch},sketch-scale={sketch}"
            name = f"merlin-{filter_size_ratio}-{staging_size_ratio}-{ghost_size_ratio}-{epoch}-{sketch}"
            if name not in done:
                todo.append((f"merlin", eparams))
    return todo

# -----------------------------
# execute command with retry and OOM handling
# -----------------------------
def run_cmd(cmd, result_file):
    global oom_happen
    try:
        proc = subprocess.Popen(
            cmd,
            shell=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True
        )

        # register running task
        with running_lock:
            running_tasks[proc.pid] = {
                "start_time": time.time(),
                "cmd": cmd
            }

        stdout, stderr = proc.communicate()

        # register task finished
        with running_lock:
            running_tasks.pop(proc.pid, None)

        if is_oom(stderr):
            oom_happen = True
            kill_shortest_jobs_until_safe()
            return False

        lines = extract_important_lines(stdout.splitlines())

        with file_lock:
            with open(result_file, "a") as f:
                for line in lines:
                    # not used, since we re-define output to files
                    # f.write(line + "\n")
                    pass

        return proc.returncode == 0

    except Exception as e:
        print(f"[ERROR] {e}")
        time.sleep(1)

    return False

# -----------------------------
# construct task list based on input directory and existing results, supports resuming
# -----------------------------
def build_tasks(root_dir, input_dir, output_dir, ignoreobj):
    tasks = []
    os.makedirs(output_dir, exist_ok=True)
    for dir_name in os.listdir(input_dir):
        dir_path = os.path.join(input_dir, dir_name)
        out_dir = os.path.join(output_dir, dir_name)
        os.makedirs(out_dir, exist_ok=True)

        for file in os.listdir(dir_path):
            input_file = os.path.join(dir_path, file)
            result_file = os.path.join(out_dir, file)
            policies_params = get_policy_todo(result_file)
            if not policies_params:
                continue
            for po_eparams in policies_params:
                po, eparams = po_eparams
                for ratio in ["0.003,0.01","0.03,0.1","0.2,0.4"]:
                    cmd = f"{root_dir}/_build/bin/cachesim {input_file} oracleGeneral {po} {ratio} --num-thread 2 --eviction-params {eparams} --outputdir {out_dir} {ignoreobj}"
                    tasks.append((cmd, result_file))
    return tasks

# -----------------------------
# main loop
# -----------------------------
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root_dir", required=True)
    parser.add_argument("--input_dir", required=True)
    parser.add_argument("--output_dir", required=True)
    parser.add_argument("--ignore_obj", action="store_true")
    parser.add_argument("--check_interval", type=float, default=1.0)
    args = parser.parse_args()

    ignoreobj = "--ignore-obj-size=true" if args.ignore_obj else ""

    tasks = build_tasks(args.root_dir, args.input_dir, args.output_dir, ignoreobj)
    print(f"[INFO] Total tasks: {len(tasks)}")
    if not tasks:
        return

    executor = ThreadPoolExecutor(max_workers=psutil.cpu_count())
    futures = {}
    next_sleep = 0
    check_interval = args.check_interval
    while tasks or futures:
        free_workers = get_available_workers()
        n_submit = min(free_workers, len(tasks))

        for _ in range(n_submit):
            cmd, result_file = tasks.pop(0)
            fut = executor.submit(run_cmd, cmd, result_file)
            futures[fut] = (cmd, result_file)

        done, _ = wait(futures.keys(), timeout=0, return_when=FIRST_COMPLETED)

        finished = 0
        for fut in done:
            finished += 1
            ok = fut.result()
            cmd, result_file = futures.pop(fut)

            if not ok:
                tasks.append((cmd, result_file))

        # check OOM and kill if needed
        if finished < 3:
            free_gb = get_available_memory_gb()
            print(f"[CHECK] free memory {free_gb:.1f} GB")

            if free_gb < MEM_SAFE_GB:
                kill_shortest_jobs_until_safe()
            next_sleep = 300
        elif n_submit <= 3:
            next_sleep += check_interval
            next_sleep = min(300, next_sleep)
        else:
            next_sleep = check_interval
        
        print(f"[INFO] Submitted {n_submit} tasks, {len(futures)} running, next check in {next_sleep:.1f}s lefting {len(tasks)}")
        time.sleep(next_sleep)

    executor.shutdown(wait=True)
    print("[INFO] All tasks completed.")
    
#python sensitivity.py   --root_dir /pathto/libCacheSim   --input_dir /pathto/CacheTrace   --output_dir /pathtooutput   --ignore_obj
if __name__ == "__main__":
    main()