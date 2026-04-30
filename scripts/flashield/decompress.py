#!/usr/bin/env python3
"""Decompress every .zst file in a target directory."""

import os
import subprocess
import sys
from pathlib import Path

def decompress_zst_files(folder_path):
    """Decompress all .zst files in the given directory.

    Args:
        folder_path: Directory that contains the .zst files.

    Returns:
        A list of successfully decompressed output paths.
    """
    zst_files = list(Path(folder_path).glob("*.zst"))

    if not zst_files:
        print(f"No .zst files were found in {folder_path}")
        return []

    decompressed_files = []

    for zst_file in zst_files:
        try:
            # Build the output file name by removing the .zst suffix.
            output_file = zst_file.with_suffix('')

            # Skip decompression if the output file already exists.
            if output_file.exists():
                print(f"Output already exists, skip decompression: {output_file.name}")
                decompressed_files.append(str(output_file))
                continue

            print(f"Decompressing: {zst_file.name}")

            # Run the zstd decompression command.
            subprocess.run(
                ['zstd', '-d', '--rm', str(zst_file)],  # Remove the compressed file after decompression.
                capture_output=True,
                text=True,
                check=True
            )

            if output_file.exists():
                decompressed_files.append(str(output_file))
                print(f"✓ Decompressed successfully: {output_file.name}")
            else:
                print(f"✗ Output file was not created: {output_file}")

        except subprocess.CalledProcessError as e:
            print(f"✗ Failed to decompress {zst_file.name}: {e.stderr}")
        except Exception as e:
            print(f"✗ Decompression error for {zst_file.name}: {str(e)}")

    return decompressed_files

def main():
    """Parse arguments and run the decompression workflow."""
    if len(sys.argv) != 2:
        print("Usage: python decompress_files.py <folder_path>")
        print("Example: python decompress_files.py ./data")
        sys.exit(1)

    folder_path = sys.argv[1]

    # Check whether the target directory exists.
    if not Path(folder_path).exists():
        print(f"Error: directory does not exist: {folder_path}")
        sys.exit(1)

    # Check whether the zstd binary is available.
    try:
        subprocess.run(['zstd', '--version'], capture_output=True, check=True)
    except (subprocess.CalledProcessError, FileNotFoundError):
        print("Error: zstd is not installed on this system")
        print("Ubuntu/Debian: sudo apt install zstd")
        print("CentOS/RHEL: sudo yum install zstd")
        print("macOS: brew install zstd")
        sys.exit(1)

    print("Start decompressing ZST files...")
    print(f"Input directory: {folder_path}")
    print("-" * 50)

    # Execute the decompression pass.
    decompressed_files = decompress_zst_files(folder_path)

    if not decompressed_files:
        print("No files were available for processing")
        sys.exit(1)

    print(f"\nDecompression complete. Successfully processed {len(decompressed_files)} files")

if __name__ == "__main__":
    main()
