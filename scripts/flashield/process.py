import os
import re

def parse_log_line(line):
    """Parse the last log line and extract miss ratio and write-byte ratio."""
    # Extract the miss ratio.
    miss_ratio_match = re.search(r'miss ratio:\s*([\d.]+)', line)
    miss_ratio = miss_ratio_match.group(1) if miss_ratio_match else "N/A"

    # Extract the trailing value inside parentheses, for example (0.1018).
    write_byte_match = re.search(r'\((\d+\.\d+)\)\s*$', line)
    write_byte = write_byte_match.group(1) if write_byte_match else "N/A"

    return miss_ratio, write_byte

def process_logs():
    """Process all log files from the three result directories."""
    folders = ['results/0.01/dram0.1', 'results/0.01/dram0.01', 'results/0.01/dram0.001']
    dram_ratios = {'dram0.1': '0.1', 'dram0.01': '0.01', 'dram0.001': '0.001'}

    results = []

    for folder in folders:
        # Derive the DRAM ratio from the directory name.
        folder_name = os.path.basename(folder)
        dram_ratio = dram_ratios.get(folder_name, 'unknown')

        # Skip directories that are missing.
        if not os.path.exists(folder):
            print(f"Warning: directory {folder} does not exist, skipping...")
            continue

        # Iterate over all .log files in the current directory.
        for filename in os.listdir(folder):
            if filename.endswith('.log'):
                filepath = os.path.join(folder, filename)

                try:
                    # Read the last line of the file.
                    with open(filepath, 'r', encoding='utf-8') as f:
                        lines = f.readlines()
                        if lines:
                            last_line = lines[-1].strip()

                            # Parse the summary line.
                            miss_ratio, write_byte = parse_log_line(last_line)

                            # Drop the .log suffix.
                            log_name = filename[:-4]

                            # Append the parsed row.
                            results.append({
                                'log_name': log_name,
                                'dram_ratio': dram_ratio,
                                'miss_ratio': miss_ratio,
                                'write_byte': write_byte
                            })

                            print(f"Processed: {filepath}")
                        else:
                            print(f"Warning: {filepath} is empty")

                except Exception as e:
                    print(f"Error: failed to process {filepath}: {e}")

    return results

def write_results(results, output_file='output.txt'):
    """Write the parsed results to a text file."""
    with open(output_file, 'w', encoding='utf-8') as f:
        # Write the header row.
        f.write("log_name, dram_ratio, miss_ratio, write_byte\n")

        # Write the data rows.
        for item in results:
            f.write(f"{item['log_name']}, {item['dram_ratio']}, {item['miss_ratio']}, {item['write_byte']}\n")

    print(f"\nResults were written to {output_file}")
    print(f"Processed {len(results)} log files in total")

if __name__ == "__main__":
    # Process all log files.
    results = process_logs()

    # Write the aggregated result file.
    if results:
        write_results(results)
    else:
        print("No log files were found, or every file failed to process")
