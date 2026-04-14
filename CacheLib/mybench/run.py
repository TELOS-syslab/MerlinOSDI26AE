#!/usr/bin/env python3
import os
import sys
import subprocess

def main(path):
    if not os.path.isdir(path):
        print(f"{path} is not a directory")
        return

    for fname in os.listdir(path):
        file_path = os.path.join(path, fname)
        if not os.path.isfile(file_path):
            continue

        # flex
        cmd1 = ["bash", "run.sh", "flex", "4000", "21", file_path]
        with open("results/" + fname + ".txt", "a") as f:
            subprocess.run(cmd1, stdout=f)

        # s3fifo
        cmd2 = ["bash", "run.sh", "s3fifo", "4000", "21", file_path]
        with open("results/" + fname + ".txt", "a") as f:
            subprocess.run(cmd2, stdout=f)

        print(f"Finished {fname}")

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <path>")
        sys.exit(1)
    main(sys.argv[1])
