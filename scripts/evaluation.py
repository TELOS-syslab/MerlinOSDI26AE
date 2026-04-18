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
oom_time = 0

# pid -> start_time
running_tasks = {}
running_lock = threading.Lock()

MEM_SAFE_GB = 100

# -----------------------------
# config: policies to run
# -----------------------------
policy_list = [
    "fifo","arc","cacheus","car","lhd","gdsf","twoq","slru","hyperbolic",
    "lecar","tinyLFU","belady","clock","lirs","fifomerge",
    "s3fifo","qdlp","lru","lfu","GLCache","sieve","merlin"
]

policy_name = [
    "FIFO","ARC","Cacheus","CAR","LHD","GDSF","TwoQ","S4LRU(25:25:25:25)",
    "Hyperbolic","LeCaR","WTinyLFU-w0.01-SLRU","Belady","Clock",
    "LIRS","FIFO_Merge_FREQUENCY","S3FIFO-0.1000-2",
    "QDLP-0.1000-0.9000-Clock2-1","LRU","LFU","GLCache","Sieve","merlin-0.10-0.05-1.00-32-1.0"
]

# =============================
# utils
# =============================
def get_available_memory_gb():
    return psutil.virtual_memory().available / (1024**3)

def is_oom(stderr):
    if not stderr:
        return False
    s = stderr.lower()
    return "killed" in s or "out of memory" in s or "oom" in s

# =============================
# kill shortest job
# =============================
def kill_shortest_jobs_until_safe():
    killed = 0
    while True:
        cpu_usage = psutil.cpu_percent(interval=0.5)
        free_gb = get_available_memory_gb()

        print(f"[CHECK] free memory {free_gb:.1f} GB cpu {cpu_usage:.1f}%")

        if free_gb >= MEM_SAFE_GB and cpu_usage < 80:
            return

        now = time.time()

        alive = []
        with running_lock:
            for pid, info in list(running_tasks.items()):
                if not psutil.pid_exists(pid):
                    running_tasks.pop(pid, None)
                    continue
                if info.get("killed"):
                    continue
                alive.append((pid, info))

        if len(alive) <= 1:
            print("[OOM] limited running tasks")
            return

        pid, info = sorted(
            alive,
            key=lambda x: now - x[1]["start_time"]
        )[0]

        try:
            runtime = now - info["start_time"]
            print(f"[KILL] PID={pid}, runtime={runtime:.1f}s")

            with running_lock:
                running_tasks[pid]["killed"] = True

            p = psutil.Process(pid)
            p.kill()
            killed += 1

            try:
                p.wait(timeout=3)
            except psutil.TimeoutExpired:
                pass

        except Exception as e:
            print(f"[WARN] kill failed {pid}: {e}")

        time.sleep(5*killed)
        if killed >= 10:
            print("killed too many tasks, wait for a while")
            time.sleep(60)
            return

# =============================
# extract important lines
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
    cpu_total = psutil.cpu_count()
    cpu_usage = psutil.cpu_percent(interval=0.5)
    cpu_usage += 15  # add some buffer
    cpu_free = cpu_total * (1 - cpu_usage/100) // 2

    mem_free = get_available_memory_gb()
    mem_based = int(mem_free // 20) - 5
    global oom_happen
    if oom_happen:
        mem_based = max(0, mem_based // 5)
    print(f"[RESOURCE] CPU usage {cpu_usage:.1f}% CPU free {cpu_free:.1f}, Mem free {mem_free:.1f} GB, mem_based {mem_based}")
    workers = int(min(cpu_free, mem_based))
    if workers <= 0 and len(running_tasks) == 0:
        workers = 1
    return max(0, workers)

# -----------------------------
# policy todo list
# recover from existing result file, only run the policies that are not done yet
# -----------------------------
def get_policy_todo(result_file):
    if not os.path.exists(result_file):
        return policy_list[:]
    done = set()
    with open(result_file) as f:
        for line in f:
            parts = line.strip().split()
            if len(parts) > 1:
                done.add(parts[1])
    todo = []
    for i, name in enumerate(policy_name):
        if name not in done:
            todo.append(policy_list[i])
    return todo

# -----------------------------
# execute command with retry and OOM handling
# -----------------------------
def run_cmd(cmd, result_file):
    global oom_happen, oom_time
    try:
        proc = subprocess.Popen(
            cmd,
            shell=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True
        )

        # register
        with running_lock:
            running_tasks[proc.pid] = {
                "start_time": time.time(),
                "cmd": cmd,
                "killed": False
            }

        stdout, stderr = proc.communicate()

        # check killed
        killed = False
        with running_lock:
            info = running_tasks.get(proc.pid)
            if info:
                killed = info.get("killed", False)
            running_tasks.pop(proc.pid, None)

        if killed:
            print(f"[KILLED] {cmd}")
            return "KILLED"

        if is_oom(stderr):
            print(f"[OOM] {cmd}")
            oom_happen = True
            oom_time = time.time()
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
    return False

# -----------------------------
# construct task list based on input directory and existing results, supports resuming
# -----------------------------
def build_tasks(root_dir, input_dir, output_dir, ignoreobj):
    tasks = []
    os.makedirs(output_dir, exist_ok=True)
    for dataset in os.listdir(input_dir):
        dataset_path = os.path.join(input_dir, dataset)
        if not os.path.isdir(dataset_path):
            continue

        out_dir = os.path.join(output_dir, dataset)
        os.makedirs(out_dir, exist_ok=True)

        for file in os.listdir(dataset_path):
            input_file = os.path.join(dataset_path, file)
            result_file = os.path.join(out_dir, file)
            policies_params = get_policy_todo(result_file)
            if not policies_params:
                continue
            for po_eparams in policies_params:
                po, eparams = po_eparams
                for ratio in ["0.003,0.01","0.03,0.1","0.2,0.4"]:
                    cmd = f"{root_dir}/bin/cachesim {input_file} oracleGeneral {po} {ratio} --num-thread 2 --outputdir {out_dir} {ignoreobj}"
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

    executor = ThreadPoolExecutor(max_workers=psutil.cpu_count())
    futures = {}
    next_sleep = 0
    check_interval = args.check_interval
    while tasks or futures:
        free_workers = get_available_workers()
        n_submit = min(free_workers, len(tasks))

        for _ in range(n_submit):
            cmd, result_file = tasks.pop(0)
            print(f"[SUBMIT] {cmd}")
            fut = executor.submit(run_cmd, cmd, result_file)
            futures[fut] = (cmd, result_file)

        done, _ = wait(futures.keys(), timeout=0, return_when=FIRST_COMPLETED)

        finished = 0
        for fut in done:
            finished += 1
            result = fut.result()
            cmd, result_file = futures.pop(fut)

            # if failed, re-append to task list for retry, but only for OOM or killed cases
            if result == "KILLED":
                tasks.append((cmd, result_file))
            elif result is False:
                tasks.append((cmd, result_file))

        # congestion control
        if finished <= 1:
            kill_shortest_jobs_until_safe()
            next_sleep += check_interval * 5
            next_sleep = min(300, next_sleep)
        if n_submit >= 10:
            next_sleep = check_interval
        elif n_submit <= 3:
            next_sleep += check_interval
            next_sleep = min(300, next_sleep)
        else:
            next_sleep -= check_interval
            next_sleep = max(check_interval, next_sleep)
        
        global oom_happen, oom_time
        if oom_happen and time.time() - oom_time > 600:
            oom_happen = False
            oom_time = 0
        print(f"[INFO] Finished {finished} tasks, Submitted {n_submit} tasks, {len(futures)} running, next check in {next_sleep:.1f}s lefting {len(tasks)}")
        time.sleep(next_sleep)

    executor.shutdown(wait=True)
    print("[INFO] All tasks completed.")

#python evaluation.py   --root_dir /pathto/libCacheSim/_build   --input_dir /pathto/CacheTrace   --output_dir /pathtooutput   --ignore_obj
if __name__ == "__main__":
    main()