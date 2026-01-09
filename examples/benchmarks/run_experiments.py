
import os
import subprocess
import argparse
import sys
from pathlib import Path

# Configuration
# Default paths relative to script location
SCRIPT_DIR = Path(__file__).parent.resolve()
BUILD_DIR = SCRIPT_DIR.parent.parent / "build" / "examples" / "benchmarks"
DATA_DIR = SCRIPT_DIR / "data"

# Benchmark Configuration
# (Benchmark Name, Supports Software Path, Specific Data Path or None for All Silesia)
BENCHMARKS = [
    ("mb_crc64_test", True, None),
    ("mb_scan_range_test", True, None), 
    ("mb_scan_exact_test", True, None),
    ("mb_select_test", True, None),
    ("mb_expand_test", True, None),
    ("mb_extract_test", True, None),
    ("mb_compression_decompression_test", True, None),
    
    # Pipelined / Complext Benchmarks (Skip SW Path)
    ("mb_decompression_scan", False, "tpc_h_data/tpch_q6_"), # Requires dir prefix
    ("mb_decompression_extract", False, "tpc_h_data/tpch_q6_"),
    
    # TPC-H Q6 End-to-End (Skip SW Path)
    ("tpch_q6_decompression_scan", False, "tpc_h_data/tpch_q6_"),
]

# Execution Settings
ENGINES = 8
CHUNK_SIZE = 2097152 # 2MB

def run_command(cmd):
    """Runs a command and returns output, prints output to console."""
    print(f"Running: {' '.join(cmd)}")
    try:
        # Run and capture output
        result = subprocess.run(cmd, check=True, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        print(result.stdout)
        return True
    except subprocess.CalledProcessError as e:
        print(f"Error running command: {e}")
        print(e.stdout)
        return False

def get_data_path(file_rel_path):
    """Resolves data path, checking for existence."""
    # Special handling for TPC-H which is a directory prefix
    if "tpch_q6_" in file_rel_path:
        # Just return the directory containing the data
        return str(DATA_DIR / "tpc_h_data") + "/"
    
    path = DATA_DIR / file_rel_path
    if not path.exists():
         print(f"Warning: Data file not found: {path}")
         return str(path)
    return str(path)

def get_silesia_files():
    """Returns a list of all files in the silesia_data directory."""
    silesia_dir = DATA_DIR / "silesia_data"
    if not silesia_dir.exists():
        print(f"Warning: Silesia data directory not found at {silesia_dir}")
        return []
    
    # Filter for files only, exclude hidden or system files if necessary
    return [f for f in silesia_dir.iterdir() if f.is_file() and not f.name.startswith('.')]

def main():
    parser = argparse.ArgumentParser(description="Run QPL Benchmarks")
    parser.add_argument("--build-dir", type=str, default=str(BUILD_DIR), help="Path to build directory containing executables")
    args = parser.parse_args()
    
    build_path = Path(args.build_dir)
    if not build_path.exists():
        print(f"Error: Build directory not found: {build_path}")
        sys.exit(1)

    # Define paths to test
    full_paths = ["software_path", "hardware_path", "staging_path"]
    
    # Get Silesia files for standard benchmarks
    silesia_files = get_silesia_files()

    print("=========================================================")
    print(f"Starting QPL Benchmark Suite")
    print(f"Engines: {ENGINES}")
    print(f"Chunk Size: {CHUNK_SIZE}")
    print("=========================================================\n")

    for bench_name, supports_sw, specific_data in BENCHMARKS:
        executable = build_path / bench_name
        
        if not executable.exists():
            print(f"Skipping {bench_name}: Executable not found in {build_path}")
            continue

        # Determine which data files to run
        data_files_to_run = []
        if specific_data:
            data_files_to_run.append(get_data_path(specific_data))
        else:
            # these are standard benchmarks, run all silesia files
            if not silesia_files:
                print(f"Skipping {bench_name}: No silesia data found.")
                continue
            data_files_to_run = [str(f) for f in silesia_files]

        print(f"--- Benchmarking {bench_name} ---")
        
        for data_path in data_files_to_run:
            dataset_name = Path(data_path).name
            if specific_data: 
                 # For directory prefixes like TPC-H, name isn't a file
                 dataset_name = "TPC-H Data"

            for p_path in full_paths:
                # Skip Software Path for benchmarks that don't support it
                if p_path == "software_path" and not supports_sw:
                    continue

                print(f"-> {dataset_name} | {p_path}")

                cmd = [str(executable), p_path, data_path, str(ENGINES)]
                
                if bench_name == "tpch_q6_decompression_scan":
                     # TPCH Q6 signature: path, compressed_dir, queue_size, orig_file
                     # data_path provided above is the dir. We need the orig file.
                     # Assuming orig file is in tpc_h_data/
                     orig_file_path = str(DATA_DIR / "tpc_h_data") + "/"
                     cmd = [str(executable), p_path, data_path, str(ENGINES), orig_file_path]
                elif "mb_decompression_scan" in bench_name or "mb_decompression_extract" in bench_name:
                      cmd.append(str(CHUNK_SIZE))
                else:
                     cmd.append(str(CHUNK_SIZE))

                success = run_command(cmd)
                if not success:
                    print(f"FAIL: {bench_name} | {dataset_name} | {p_path}")

        print("\n")

if __name__ == "__main__":
    main()
