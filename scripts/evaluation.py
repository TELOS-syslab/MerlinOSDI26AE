#!/usr/bin/env python3
"""Run the full libCacheSim trace evaluation for Figure 11 and Figure 12.

The script walks an input trace directory, builds one cachesim command per
trace/policy/cache-size pair, and runs commands with adaptive parallelism based
on available CPU and memory. Existing result files are inspected so interrupted
runs can resume without rerunning completed policies.
"""
import os
import subprocess
import time
import argparse
import psutil
import threading
from concurrent.futures import ThreadPoolExecutor, wait, FIRST_COMPLETED
import re
import signal

# =============================
# Global process state shared by worker threads.
# =============================
file_lock = threading.Lock()
oom_happen = False
oom_time = 0

# pid -> start_time
running_tasks = {}
running_lock = threading.Lock()

MEM_SAFE_GB = 100

# -----------------------------
# Default policies evaluated in the main trace-driven experiment.
# The display names must match cachesim output so get_policy_todo() can detect
# completed work in partially generated result files.
# -----------------------------
DEFAULT_POLICY_LIST = [
    "fifo","arc","cacheus","car","lhd","gdsf","twoq","slru","hyperbolic",
    "lecar","tinyLFU","belady","clock","lirs","fifomerge",
    "s3fifo","qdlp","lru","lfu","sieve","merlin"
]

DEFAULT_POLICY_NAME = [
    "FIFO","ARC","Cacheus","CAR","LHD","GDSF","TwoQ","S4LRU(25:25:25:25)",
    "Hyperbolic","LeCaR","WTinyLFU-w0.01-SLRU","Belady","Clock",
    "LIRS","FIFO_Merge_FREQUENCY","S3FIFO-0.1000-2",
    "QDLP-0.1000-0.9000-Clock2-1","LRU","LFU","Sieve","merlin-0.10-0.05-1.00-32-1.0"
]


def _build_default_name_map():
    """Build default policy->display-name map used for resume detection."""
    return dict(zip(DEFAULT_POLICY_LIST, DEFAULT_POLICY_NAME))


def resolve_policy_config(policy_list_arg: str):
    """Resolve policies and display names from CLI args with safe defaults.

    - If no args are provided, use the original defaults.
    - If policy list is provided, reuse known default display names when
      possible. Unknown policies are kept with an empty display name so resume
      detection will not falsely mark them as done.
    """
    if not policy_list_arg:
        return list(DEFAULT_POLICY_LIST), list(DEFAULT_POLICY_NAME)

    selected_policies = [p.strip() for p in policy_list_arg.split(",") if p.strip()]
    if not selected_policies:
        raise ValueError("--policy_list is empty after parsing")

    default_map = _build_default_name_map()
    selected_names = []
    unknown = []
    for policy in selected_policies:
        if policy in default_map:
            selected_names.append(default_map[policy])
        else:
            selected_names.append("")
            unknown.append(policy)

    if unknown:
        print(
            "[WARN] Missing default policy_name mapping for: "
            + ", ".join(unknown)
            + ". Their policy_name entries are left empty."
        )

    return selected_policies, selected_names

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
# Kill short-running jobs when the machine is under pressure. These jobs are
# likely the least expensive to retry, so this reduces wasted progress.
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
                    continue
                alive.append((pid, info))

        if len(alive) <= 1:
            print("[OOM] only 1 task left")
            return

        pid, info = sorted(
            alive,
            key=lambda x: now - x[1]["start_time"]
        )[0]

        runtime = now - info["start_time"]
        print(f"[KILL] PID={pid}, runtime={runtime:.1f}s")

        try:
            # kill the whole process group to avoid orphan processes
            os.killpg(os.getpgid(pid), signal.SIGKILL)
        except Exception as e:
            print(f"[WARN] killpg failed {pid}: {e}")

        killed += 1
        time.sleep(5 * killed)

        if killed >= 10:
            print("[WARN] killed too many tasks, pause")
            time.sleep(60)
            return

# =============================
# Extract important lines
# =============================
def extract_important_lines(stdout_lines):
    """
    Extract important lines from cachesim output and normalize the cache-size format.
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
    Convert a cache-size string to an integer number of bytes.
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
# Estimate how many new worker jobs can be submitted without exhausting memory.
# =============================
def get_available_workers():
    cpu_total = psutil.cpu_count()
    cpu_usage = psutil.cpu_percent(interval=0.5)
    cpu_usage += 15  # Conservative buffer for background activity.
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
# Recover from existing result files and only schedule policies that have not yet completed.
# -----------------------------
def get_policy_todo(result_file, policy_list, policy_name):
    done = set()
    if not os.path.exists(result_file):
        pass
    else:
        with open(result_file) as f:
            for line in f:
                parts = line.strip().split()
                if len(parts) > 1:
                    done.add(parts[1])
    todo = []
    for i, name in enumerate(policy_name):
        if not name:
            # Unknown display name: cannot reliably check completion from
            # existing output lines, so keep this policy in the todo set.
            todo.append(policy_list[i])
            continue
        if name not in done:
            todo.append(policy_list[i])
    return todo

# -----------------------------
# Execute one cachesim command. The caller retries commands that return False or "KILLED".
# -----------------------------
def run_cmd(cmd, result_file):
    global oom_happen, oom_time
    try:
        proc = subprocess.Popen(
            cmd,
            shell=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            preexec_fn=os.setsid   # Run in a new process group for cleanup.
        )

        with running_lock:
            running_tasks[proc.pid] = {
                "start_time": time.time(),
                "cmd": cmd,
            }

        stdout, stderr = proc.communicate()
        with running_lock:
            running_tasks.pop(proc.pid, None)
            
        rc = proc.returncode

        if rc != 0:
            # killed by SIGKILL
            if rc == -signal.SIGKILL:
                print(f"[KILLED] {cmd}")
                return "KILLED"

            if is_oom(stderr):
                print(f"[OOM] {cmd}")
                oom_happen = True
                oom_time = time.time()
                return False

            print(f"[FAIL rc={rc}] {cmd}")
            return False

        # -----------------------------
        # normal completion
        # -----------------------------
        lines = extract_important_lines(stdout.splitlines())

        with file_lock:
            with open(result_file, "a") as f:
                for line in lines:
                    # Not used because cachesim writes detailed output files via
                    # --outputdir. Keep parsing here for optional debugging.
                    # f.write(line + "\n")
                    pass

        return True

    except Exception as e:
        print(f"[ERROR] {e}")
        return False

# -----------------------------
# Construct tasks from the dataset tree. Expected layout:
# input_dir/<dataset>/<trace files>.
# -----------------------------
def build_tasks(root_dir, input_dir, output_dir, ignoreobj, policy_list, policy_name):
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
            policies = get_policy_todo(result_file, policy_list, policy_name)
            if not policies:
                continue
            for policy in policies:
                for ratio in ["0.003,0.01","0.03,0.1","0.2,0.4"]:
                    cmd = f"{root_dir}/bin/cachesim {input_file} oracleGeneral {policy} {ratio} --num-thread 2 --outputdir {out_dir} {ignoreobj}"
                    tasks.append((cmd, result_file))
    return tasks

# -----------------------------
# Main adaptive scheduling loop.
# -----------------------------
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root_dir", required=True)
    parser.add_argument("--input_dir", required=True)
    parser.add_argument("--output_dir", required=True)
    parser.add_argument(
        "--policy_list",
        default="",
        help="Comma-separated eviction policies to run. Default uses built-in policy list.",
    )
    parser.add_argument("--ignore_obj", action="store_true")
    parser.add_argument("--check_interval", type=float, default=1.0)
    args = parser.parse_args()

    try:
        policy_list, policy_name = resolve_policy_config(args.policy_list)
    except ValueError as e:
        parser.error(str(e))

    ignoreobj = "--ignore-obj-size=true" if args.ignore_obj else ""

    tasks = build_tasks(
        args.root_dir,
        args.input_dir,
        args.output_dir,
        ignoreobj,
        policy_list,
        policy_name,
    )
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

        done, _ = wait(futures.keys(), timeout=0.5, return_when=FIRST_COMPLETED)

        finished = 0
        for fut in done:
            finished += 1
            try:
                result = fut.result()
            except Exception as e:
                print(f"[FUTURE ERROR] {e}")
                result = False

            cmd, result_file = futures.pop(fut)

            # retry logic: if OOM or killed, retry the task later
            if result in ("KILLED", False):
                finished -= 1
                print(f"[RETRY] {cmd}")
                tasks.append((cmd, result_file))

        # Simple congestion control: slow down submissions when few jobs finish
        # and speed up when the system is making progress.
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

# Example:
# python3 scripts/evaluation.py --root_dir ./libCacheSim/_build \
#   --input_dir ./CacheTrace --output_dir ./results/eval_ignore_obj --ignore_obj
if __name__ == "__main__":
    main()
