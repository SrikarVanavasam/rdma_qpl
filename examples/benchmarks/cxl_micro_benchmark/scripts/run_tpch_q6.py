import subprocess
import os

# --- Configuration ---
NUMA_NODE = "2"
SERVER_IP = "192.168.200.10"
QUEUE_SIZE = "32"

# Paths to the data (Adjust these to where your TPC-H data lives)
COMPRESSED_DIR = "/fast-lab-share/srikarv2/tpch_data/compressed/"
ORIG_DIR = "/fast-lab-share/srikarv2/tpch_data/raw/"

BUILD_DIR = "/fast-lab-share/srikarv2/rdma_qpl/build/examples/benchmarks"
EXE_NAME = "tpch_q6_cxl_decompression_scan"

MODES = [
    "local",
    "local_umwait",
    "cxl",
    "cxl_umwait",
    "cpu",
    "rdma",
    "combined",
    "combined_umwait"
]

def run_tpch(mode):
    exe_path = os.path.join(BUILD_DIR, EXE_NAME)
    if not os.path.exists(exe_path):
        print(f"[ERROR] Executable not found: {exe_path}")
        return

    # Usage: <mode> <numa_node> [server_ip] <compressed_dir> <queue_size> <orig_dir>
    cmd = [exe_path, mode, NUMA_NODE, SERVER_IP, COMPRESSED_DIR, QUEUE_SIZE, ORIG_DIR]
    
    print(f"\n>>> RUNNING TPC-H Q6: {' '.join(cmd)}")
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, check=False)
        print(result.stdout)
        if result.stderr:
            print(f"[STDERR]\n{result.stderr}")
    except Exception as e:
        print(f"[FAILED] Mode {mode}: {e}")

def main():
    if not os.path.exists(COMPRESSED_DIR):
        print(f"[WARNING] Compressed directory {COMPRESSED_DIR} does not exist.")
    
    for mode in MODES:
        run_tpch(mode)

if __name__ == "__main__":
    main()
