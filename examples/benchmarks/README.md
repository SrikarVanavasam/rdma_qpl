# RDMA QPL Benchmarks

This directory contains benchmarks migrated from the ISPASS-2025-IAA repository, integrated into the RDMA QPL build system.

## Structure

- **micro_benchmark/**: Contains microbenchmarks for various QPL operations (crc64, scan, etc.).
- **end_to_end/pandas/**: Contains the QPL implementation of TPC-H Query 6.
- **data/**: Contains data required for the benchmarks.

## Building

These benchmarks are built automatically when building the `examples` directory of the project.

```bash
mkdir build
cd build
cmake ..
make -j
```

### Running Benchmarks Automatically

Use the provided Python script to run the full benchmark suite comparing Software, Hardware, and Staging (RDMA) paths.

```bash
python3 examples/benchmarks/run_experiments.py
```

This script will:
- Discover built benchmarks in `build/examples/benchmarks`.
- Run experiments on `software_path` (CPU), `hardware_path` (Local accelerator), and `staging_path` (Remote RDMA accelerator).
- Automatically use 8 engines and a 2MB chunk size.
- Skip software execution for pipelined benchmarks where CPU comparison is not relevant.

### Manual Execution

The microbenchmarks are compiled into executables prefixed with `mb_`. For example:

```bash
./examples/benchmarks/mb_crc64_test hardware_path <input_file> <queue_size> <chunk_size>
```

(Check specific source files for argument details).

### Pandas / TPC-H Query 6

The TPC-H Query 6 implementation is compiled into `pandas_decompression_scan` and `pandas_lineitem_compression`.

To run `pandas_decompression_scan`:

```bash
./examples/benchmarks/pandas_decompression_scan <hardware_path|software_path> <compressed_data_dir> <queue_size> <org_file_path>
```

Note: This requires `zlib` and `lz4` to be present on the system.
