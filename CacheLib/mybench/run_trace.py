import os
import subprocess

file_names = [
    # "fiu_madmax-110108-112108.oracleGeneral",
    # "fiu_online.cs.fiu.edu-110108-113008.oracleGeneral",
    # "fiu_webmail.cs.fiu.edu-110108-113008.oracleGeneral",
    # "fiu_webmail+online.cs.fiu.edu-110108-113008.oracleGeneral",
    # "fiu_webresearch-030409-033109.oracleGeneral",
    # "fiu_webusers-030409-033109.oracleGeneral"
    "fiu_ikki-110108-112108.oracleGeneral"
]

input_dir = "data/fiu/fiu_expand"
output_dir = "results/fiu_expand"

os.makedirs(output_dir, exist_ok=True)

def run_and_save(cmd, out_path):
    """Run a command and save its stdout to a file."""
    print(f"Running: {' '.join(cmd)}")
    with open(out_path, "a") as f:
        result = subprocess.run(cmd, stdout=f, stderr=subprocess.PIPE, text=True)

    print(f"Saved result to {out_path}")


for name in file_names:
    input_path = os.path.join(input_dir, name)

    # flex
    out_flex = os.path.join(output_dir, f"{name}.txt")
    run_and_save(["bash", "run.sh", "flex", "8000", "21", input_path], out_flex)

    # s3fifo
    out_s3fifo = os.path.join(output_dir, f"{name}.txt")
    run_and_save(["bash", "run.sh", "s3fifo", "8000", "21", input_path], out_s3fifo)

print("All tasks completed.")
