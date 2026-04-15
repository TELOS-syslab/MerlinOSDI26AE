#!/usr/bin/env python3
import os
import subprocess
import time
import argparse
import psutil
import threading
from concurrent.futures import ThreadPoolExecutor, wait, FIRST_COMPLETED
import re
import subprocess

def get_running_cachesim_count():
    try:
        out = subprocess.check_output(['pgrep', '-f', 'cachesim'])
        pids = out.decode().strip().split('\n')
        return len(pids)
    except subprocess.CalledProcessError:
        # pgrep returns non-zero when no matching process is found
        return 0

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
# -----------------------------
# parameter parsing
# -----------------------------
def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root_dir", required=True)
    parser.add_argument("--input_dir", required=True)
    parser.add_argument("--output_dir", required=True)
    parser.add_argument("--ignore_obj", action="store_true")
    parser.add_argument("--max_retry", type=int, default=2)
    parser.add_argument("--check_interval", type=float, default=1.0)
    return parser.parse_args()


# -----------------------------
# configuration
# -----------------------------
policy_list = [
    "merlin"
]
policy_name = [
    "merlin-0.10-0.05-1.00-32-1.0"
]

file_lock = threading.Lock()  # file lock for writing results

oom_memory = 100
oom_happen = False
# -----------------------------
# calculate idle resources
# -----------------------------
def get_available_workers():
    cpu_total = psutil.cpu_count() * 0.8  # leave 20% CPU for system and other tasks
    cpu_usage = psutil.cpu_percent(interval=0.5)
    running = get_running_cachesim_count()
    idle_cpus = cpu_total - running
    cpu_free = cpu_total * (1 - cpu_usage/100) // 2  # treat each task as 2 CPU cores, since cachesim is multi-threaded
    cpu_free = min(idle_cpus//2, cpu_free)

    mem = psutil.virtual_memory()
    mem_free_gb = mem.available / (1024**3)
    mem_based = int(mem_free_gb // 20) - 5 # 20GB per task, leave 5 tasks worth of buffer(100GB) for safety
    if oom_happen:
        mem_based // 10

    workers = int(min(cpu_free, mem_based))
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
            eparams = f"filter-size-ratio={filter_size_ratio},staging-size-ratio={staging_size_ratio},ghost-size-ratio={ghost_size_ratio},epoch-update={epoch},sckech-scale={sketch}"
            name = f"merlin-{filter_size_ratio}-{staging_size_ratio}-{ghost_size_ratio}-{epoch}-{sketch}"
            if name not in done:
                todo.append((f"merlin", eparams))
    return todo

# -----------------------------
# execute command with retry and OOM handling
# -----------------------------
def run_cmd(cmd, result_file, max_retry):
    for attempt in range(max_retry+1):
        try:
            # execute the command and capture output
            proc = subprocess.Popen(cmd, shell=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            stdout, stderr = proc.communicate()

            # OOM detection
            if is_oom(stderr):
                print(f"[OOM] retry {attempt+1}/{max_retry} for {cmd}")
                oom_happen = True
                time.sleep(5)
                continue

            # check return code
            # extract important lines and write to result file
            important_lines = extract_important_lines(stdout.splitlines())
            with file_lock:
                with open(result_file, "a") as f:
                    for line in important_lines:
                        # not used, since we re-define output to files
                        # f.write(line + "\n")
                        pass

            return proc.returncode == 0

        except Exception as e:
            print(f"[ERROR] {e} during {cmd}")
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
    args = parse_args()
    root_dir = args.root_dir
    input_dir = args.input_dir
    output_dir = args.output_dir
    ignoreobj = "--ignore-obj-size=true" if args.ignore_obj else ""
    max_retry = args.max_retry
    check_interval = args.check_interval

    tasks = build_tasks(root_dir, input_dir, output_dir, ignoreobj)
    print(f"[INFO] Total tasks: {len(tasks)}")
    if not tasks:
        return

    executor = ThreadPoolExecutor(max_workers=psutil.cpu_count())
    futures = {}  # future -> (cmd, result_file)
    next_sleep = 0
    while tasks or futures:
        free_workers = get_available_workers()
        n_submit = min(free_workers, len(tasks))
        for _ in range(n_submit):
            cmd, result_file = tasks.pop(0)
            print(f"[SUBMIT] {cmd}")
            fut = executor.submit(run_cmd, cmd, result_file, max_retry)
            futures[fut] = (cmd, result_file)
        # wait for at least one task to complete or timeout
        done, _ = wait(futures.keys(), timeout=0, return_when=FIRST_COMPLETED)
        finished = 0
        for fut in done:
            finished += 1
            ok = fut.result()
            cmd, result_file = futures.pop(fut)
            if not ok:
                print(f"[REQUEUE] {cmd}")
                tasks.append((cmd, result_file))

        if n_submit <= 3 or finished <= 3:
            next_sleep += check_interval 
            next_sleep = min(300,next_sleep)
        else:
            next_sleep = check_interval
        
        print(f"[INFO] Submitted {n_submit} tasks, {len(futures)} running, next check in {next_sleep:.1f}s lefting {len(tasks)}")
        time.sleep(next_sleep)

    executor.shutdown(wait=True)
    print("[INFO] All tasks completed.")

#python sensitivity.py   --root_dir /pathto/libCacheSim   --input_dir /pathto/CacheTrace   --output_dir /pathtooutput   --ignore_obj
if __name__ == "__main__":
    main()