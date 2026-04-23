import re
import os
from collections import defaultdict

cache_sizes = [0.01, 0.1]

# 你要处理哪些算法，就在这里开关
input_files = []
for cache_size in cache_sizes:
    input_files.extend([
        # f"{cache_size}/arcfix-{cache_size}.txt",
        # f"{cache_size}/cacheus-{cache_size}.txt",
        f"{cache_size}/merlin-{cache_size}.txt",
        # f"{cache_size}/fifo-{cache_size}.txt",
        # f"{cache_size}/flashield-{cache_size}.txt",
        # f"{cache_size}/s3fifo-{cache_size}.txt",
    ])

# 数据结构：
# data[algo][cache_size][dram_ratio] = { "miss": [], "wa": [] }
data = defaultdict(lambda: defaultdict(lambda: defaultdict(lambda: {"miss": [], "wa": []})))

# 正则表达式
patterns = {
    "ARCfix": re.compile(
        r"ARCfix-(?P<dram_ratio>[0-9.]+).*?miss ratio (?P<miss_ratio>[0-9.]+).*?write_amp (?P<write_amp>[0-9.]+)",
        re.IGNORECASE
    ),
    "Cacheus": re.compile(
        r"Cacheus-(?P<dram_ratio>[0-9.]+).*?miss ratio (?P<miss_ratio>[0-9.]+).*?write_amp (?P<write_amp>[0-9.]+)",
        re.IGNORECASE
    ),
    "Merlin": re.compile(
        r"merlin-(?P<dram_ratio>[0-9.]+)-[0-9.]+-[0-9.]+-[0-9.]+-[0-9.]+.*?miss ratio (?P<miss_ratio>[0-9.]+).*?write_amp (?P<write_amp>[0-9.]+)",
        re.IGNORECASE
    ),
    "FIFO": re.compile(
        r"FIFO.*?miss ratio (?P<miss_ratio>[0-9.]+).*?write_amp (?P<write_amp>[0-9.]+)",
        re.IGNORECASE
    ),
    "S3FIFO": re.compile(
        r"S3FIFO-(?P<dram_ratio>[0-9.]+)-\d+.*?miss ratio (?P<miss_ratio>[0-9.]+).*?write_amp (?P<write_amp>[0-9.]+)",
        re.IGNORECASE
    )
}

# Flashield 特殊格式： w63.bin, dram_ratio, miss_ratio, write_amp
flashield_re = re.compile(
    r"[^,]+,\s*(?P<dram_ratio>[0-9.]+),\s*(?P<miss_ratio>[0-9.]+),\s*(?P<write_amp>[0-9.]+)"
)

for filename in input_files:
    if not os.path.exists(filename):
        print(f"跳过不存在的文件: {filename}")
        continue

    # 从路径里提取 cache_size，比如 "0.01/merlin-0.01.txt"
    cache_size = float(os.path.normpath(filename).split(os.sep)[0])

    print(f"正在处理: {filename} (cache_size={cache_size})")

    with open(filename, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue

            # ---------- Flashield（CSV 格式） ----------
            if "flashield" in filename.lower():
                m = flashield_re.search(line)
                if m:
                    dram = float(m.group("dram_ratio"))
                    miss = float(m.group("miss_ratio"))
                    wa = float(m.group("write_amp"))
                    data["Flashield"][cache_size][dram]["miss"].append(miss)
                    data["Flashield"][cache_size][dram]["wa"].append(wa)
                continue

            # ---------- S3FIFO ----------
            if "s3fifo" in filename.lower():
                m = patterns["S3FIFO"].search(line)
                if m:
                    dram = float(m.group("dram_ratio"))
                    miss = float(m.group("miss_ratio"))
                    wa = float(m.group("write_amp"))
                    print("S3FIFO", cache_size, dram, miss, wa)
                    data["S3FIFO"][cache_size][dram]["miss"].append(miss)
                    data["S3FIFO"][cache_size][dram]["wa"].append(wa)
                continue

            # ---------- 其他算法 ----------
            for algo_name, ptn in patterns.items():
                m = ptn.search(line)
                if m:
                    # FIFO 没有 dram_ratio，给默认值
                    if algo_name == "FIFO" and "dram_ratio" not in m.groupdict():
                        dram = 0.1
                    else:
                        dram = float(m.groupdict().get("dram_ratio", "0.1"))

                    miss = float(m.group("miss_ratio"))
                    wa = float(m.group("write_amp"))

                    print(algo_name, cache_size, dram, miss, wa)

                    data[algo_name][cache_size][dram]["miss"].append(miss)
                    data[algo_name][cache_size][dram]["wa"].append(wa)
                    break

# ---------- 写入输出文件 ----------
for algo in data.keys():
    output_name = f"{algo.lower()}.txt"

    with open(output_name, "w", encoding="utf-8") as f:
        f.write("cache_size, dram_ratio, miss_ratio, write_amp\n")
        for cache_size in sorted(data[algo].keys()):
            for dram in sorted(data[algo][cache_size].keys()):
                miss_list = data[algo][cache_size][dram]["miss"]
                wa_list = data[algo][cache_size][dram]["wa"]

                if not miss_list or not wa_list:
                    continue

                miss_avg = sum(miss_list) / len(miss_list)
                wa_avg = sum(wa_list) / len(wa_list)
                f.write(f"{cache_size}, {dram}, {miss_avg:.4f}, {wa_avg:.4f}\n")

print("处理完成！")