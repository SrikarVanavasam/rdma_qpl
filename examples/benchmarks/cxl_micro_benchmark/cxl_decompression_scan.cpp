//* [QPL_CXL_DECOMPRESSION_SCAN_BENCHMARK] */

#include <iostream>
#include <fstream>
#include <vector>
#include <memory>
#include <string>
#include <chrono>
#include <thread>
#include <numa.h>

#include "qpl/qpl.h"
#include "cxl_bench_utils.hpp"

static size_t chunk_size = 2097152;
constexpr const uint32_t input_vector_width = 8;
constexpr const uint32_t value_to_find = 66; // 'B'

int cxl_iaa_decompression_scan(std::string compressed_data_prefix, uint32_t num_files, const CxlBenchConfig& config, const uint32_t queue_size)
{
    std::cout << "[CXL IAA Decompression + Scan]" << std::endl;

    std::vector<qpl_job *> job(queue_size);
    uint32_t job_size = 0;
    qpl_status status = qpl_get_job_size(config.execution_path, &job_size);
    if (status != QPL_STS_OK) return 1;

    // Allocate and register one large buffer for all jobs using 2MB hugepages
    uint8_t* all_jobs_buffer = (uint8_t*)huge_alloc_on_node(queue_size * job_size, config.numa_node, false);
    uint64_t all_jobs_iova;
    qpl_cxl_register_buffer(all_jobs_buffer, queue_size * job_size, &all_jobs_iova);

    for (int i = 0; i < queue_size; ++i) {
        job[i] = reinterpret_cast<qpl_job *>(all_jobs_buffer + i * job_size);
        qpl_init_job(config.execution_path, job[i]);
        job[i]->numa_id = config.cxl_numa_id;
    }

    // Pre-scan file sizes and load into one giant hugepage buffer
    std::vector<size_t> file_sizes(num_files);
    size_t total_compressed_size = 0;
    for (uint32_t i = 0; i < num_files; ++i) {
        std::string filename = compressed_data_prefix + "." + std::to_string(i);
        std::ifstream file(filename.c_str(), std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            std::cerr << "Failed to open " << filename << std::endl;
            return 1;
        }
        file_sizes[i] = file.tellg();
        total_compressed_size += file_sizes[i];
    }

    uint8_t* all_compressed_ptr = (uint8_t*)huge_alloc_on_node(total_compressed_size, config.numa_node, true);
    uint64_t all_compressed_iova;
    qpl_cxl_register_buffer(all_compressed_ptr, total_compressed_size, &all_compressed_iova);

    std::vector<size_t> file_offsets(num_files);
    size_t current_offset = 0;
    for (uint32_t i = 0; i < num_files; ++i) {
        std::string filename = compressed_data_prefix + "." + std::to_string(i);
        std::ifstream file(filename.c_str(), std::ios::binary);
        file.read((char*)(all_compressed_ptr + current_offset), file_sizes[i]);
        file_offsets[i] = current_offset;
        current_offset += file_sizes[i];
    }

    // Allocate and register one large buffer for all result chunks using 1GB Hugepages
    uint8_t* all_result_buffer = (uint8_t*)huge_alloc_on_node(queue_size * chunk_size, config.numa_node, true);
    uint64_t all_result_iova;
    qpl_cxl_register_buffer(all_result_buffer, queue_size * chunk_size, &all_result_iova);

    std::chrono::duration<int64_t, std::nano> elapsed_time_ns = std::chrono::nanoseconds::zero();
    uint32_t file_id = 0;

    while (file_id < num_files) {
        int enqueue_cnt = 0;
        for (int i = 0; i < queue_size && file_id < num_files; ++i, ++file_id) {
            job[i]->op = qpl_op_scan_eq;
            job[i]->next_in_ptr = all_compressed_ptr + file_offsets[file_id];
            job[i]->next_out_ptr = all_result_buffer + i * chunk_size;
            job[i]->available_in = static_cast<uint32_t>(file_sizes[file_id]);
            job[i]->available_out = static_cast<uint32_t>(chunk_size);
            job[i]->src1_bit_width = input_vector_width;
            job[i]->out_bit_width = qpl_ow_32;
            job[i]->param_low = value_to_find;
            job[i]->num_input_elements = static_cast<uint32_t>(chunk_size / (input_vector_width / 8));
            job[i]->flags = QPL_FLAG_FIRST | QPL_FLAG_LAST | QPL_FLAG_DECOMPRESS_ENABLE;
            enqueue_cnt++;
        }

        auto s = std::chrono::steady_clock::now();
        for (int i = 0; i < enqueue_cnt; ++i) qpl_submit_job(job[i]);
        for (int i = 0; i < enqueue_cnt; ++i) qpl_wait_job(job[i]);
        auto e = std::chrono::steady_clock::now();
        elapsed_time_ns += e - s;
    }

    double exec_time_sec = static_cast<double>(elapsed_time_ns.count()) / 1e9;
    std::cout << "\nDecompression + Scan performed successfully." << std::endl;
    std::cout << "Bandwidth: " << static_cast<double>(num_files * chunk_size) / 1024 / 1024 / exec_time_sec << " MB/s" << std::endl;

    // Cleanup
    for (int i = 0; i < queue_size; ++i) {
        qpl_fini_job(job[i]);
    }
    qpl_cxl_deregister_buffer(all_result_buffer);
    huge_free(all_result_buffer, queue_size * chunk_size, true);
    qpl_cxl_deregister_buffer(all_compressed_ptr);
    huge_free(all_compressed_ptr, total_compressed_size, true);
    qpl_cxl_deregister_buffer(all_jobs_buffer);
    huge_free(all_jobs_buffer, queue_size * job_size, false);

    return 0;
}

int main(int argc, char** argv) {
    CxlBenchConfig config;
    int next_arg = parse_cxl_bench_args(argc, argv, config);
    if (next_arg < 0) return 1;

    if (argc < next_arg + 2) {
        std::cerr << "Usage: " << argv[0] << " <mode> <numa_node> [server_ip] <prefix> <num_files> [queue_size]\n";
        return 1;
    }

    std::string prefix = argv[next_arg];
    uint32_t num_files = std::stoi(argv[next_arg + 1]);
    uint32_t queue_size = 64;

    if (argc > next_arg + 2) {
        queue_size = std::stoi(argv[next_arg + 2]);
    }

    qpl_status status = init_cxl_bench(config);
    if (status != QPL_STS_OK) {
        std::cerr << "CXL Init failed: " << status << std::endl;
        return 1;
    }

    return cxl_iaa_decompression_scan(prefix, num_files, config, queue_size);
}
