import subprocess
import os

# --- Configuration ---
RAW_INPUT_FILE = "/dev/shm/random.bin"
COMPRESSED_PREFIX = "/dev/shm/random_compressed"
NUM_FILES = "500"  # Number of compressed chunks to process
NUMA_NODE = "2"
SERVER_IP = "192.168.200.10"
QUEUE_SIZE = "64"

# Find the build directory relative to this script
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
BUILD_DIR = os.path.abspath(os.path.join(SCRIPT_DIR, "../../../../build/examples/benchmarks/cxl_micro_benchmark"))

MODES = [
    "local",
    "local_umwait",
    "cpu",
    "rdma",
    "cxl",
    "cxl_umwait",
    "combined",
    "combined_umwait"
]

# Benchmarks that take (prefix, num_files, queue_size)
DECOMP_SCAN_BENCHMARKS = [
    "cxl_decompression_scan"
]

# Benchmarks that take (prefix, num_files, lower_idx, upper_idx, queue_size)
DECOMP_EXTRACT_BENCHMARKS = [
    "cxl_decompression_extract"
]

# Benchmarks that take (src_file, queue_size)
BASIC_BENCHMARKS = [
    "cxl_scan_exact_test",
    "cxl_compression_decompression_test",
    "cxl_crc64_test",
    "cxl_expand_test",
    "cxl_scan_range_test",
    "cxl_select_test"
]

# Benchmarks that take (src_file, lower_idx, upper_idx, queue_size)
EXTRACT_BENCHMARKS = [
    "cxl_extract_test"
]

def run_bench(bench, mode):
    exe_path = os.path.join(BUILD_DIR, bench)
    if not os.path.exists(exe_path):
        print(f"[ERROR] Executable not found: {exe_path}")
        return

    # Base command: <exe> <mode> <numa_node> <server_ip>
    cmd = [exe_path, mode, NUMA_NODE, SERVER_IP]

    if bench in DECOMP_SCAN_BENCHMARKS:
        cmd += [COMPRESSED_PREFIX, NUM_FILES, QUEUE_SIZE]
    elif bench in DECOMP_EXTRACT_BENCHMARKS:
        # Range: 0 to 128K elements (assuming 8-byte elements for a ~1MB chunk)
        cmd += [COMPRESSED_PREFIX, NUM_FILES, "0", "131072", QUEUE_SIZE]
    elif bench in EXTRACT_BENCHMARKS:
        cmd += [RAW_INPUT_FILE, "0", "131072", QUEUE_SIZE]
    else:
        # Basic: <src_file> <queue_size>
        cmd += [RAW_INPUT_FILE, QUEUE_SIZE]
    
    print(f"\n>>> RUNNING: {' '.join(cmd)}")
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, check=False)
        if result.stdout:
            print(result.stdout)
        if result.stderr:
            print(f"[STDERR]\n{result.stderr}")
    except Exception as e:
        print(f"[FAILED] {bench} in mode {mode}: {e}")

import sys

def main():
    # Sanity checks
    if not os.path.exists(RAW_INPUT_FILE):
        print(f"[WARNING] Raw input file {RAW_INPUT_FILE} not found.")
    
    # Check if at least one compressed chunk exists if we're running decomp benches
    first_chunk = f"{COMPRESSED_PREFIX}.0"
    if not os.path.exists(first_chunk):
        print(f"[WARNING] Compressed prefix {COMPRESSED_PREFIX} (e.g. {first_chunk}) not found.")
        print(f"         Use 'compress_tool' to generate them first!")

    all_benchmarks = (BASIC_BENCHMARKS + EXTRACT_BENCHMARKS + 
                      DECOMP_SCAN_BENCHMARKS + DECOMP_EXTRACT_BENCHMARKS)
    
    # Support individual benchmark selection
    target_benchmarks = all_benchmarks
    if len(sys.argv) > 1:
        requested = sys.argv[1]
        if requested in all_benchmarks:
            target_benchmarks = [requested]
        else:
            print(f"Error: Benchmark '{requested}' not found.")
            print(f"Available benchmarks: {', '.join(all_benchmarks)}")
            sys.exit(1)

    for bench in target_benchmarks:
        print(f"\n{'='*60}")
        print(f" BENCHMARK: {bench}")
        print(f"{'='*60}")
        for mode in MODES:
            run_bench(bench, mode)

if __name__ == "__main__":
    main()
