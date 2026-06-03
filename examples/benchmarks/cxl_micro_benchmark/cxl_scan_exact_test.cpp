//* [QPL_CXL_SCAN_BENCHMARK] */

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

/**
 * NOTE : Maximum transfer size per grouped_workqueues of IAA is 2MB
 */
static size_t chunk_size = 2097152;
constexpr const uint32_t input_vector_width = 8;
constexpr const uint32_t boundary           = 65; //'A'

int cxl_iaa_scan(std::string src_data_file_path, const CxlBenchConfig& config, const uint32_t queue_size)
{
    std::cout << "[CXL IAA Scan]" << std::endl;
    
    // Loading source file
    std::ifstream src_file(src_data_file_path, std::ifstream::in | std::ifstream::binary);
    if (!src_file) {
        std::cerr << "File not found : " << src_data_file_path << std::endl;
        return 1;
    }

    src_file.seekg(0, std::ios::end);
    std::size_t src_file_size = static_cast<std::size_t>(src_file.tellg());
    src_file.seekg(0, std::ios::beg);

    // Job initialization
    std::vector<qpl_job *> job(queue_size);
    uint32_t job_size = 0;
    qpl_status status = qpl_get_job_size(config.execution_path, &job_size);
    if (status != QPL_STS_OK) {
        std::cerr << "Failed to get job size: " << status << std::endl;
        return 1;
    }

    // Allocate and register one large buffer for all jobs using 2MB hugepages
    uint8_t* all_jobs_buffer = (uint8_t*)huge_alloc_on_node(queue_size * job_size, config.numa_node, false);
    uint64_t all_jobs_iova;
    qpl_cxl_register_buffer(all_jobs_buffer, queue_size * job_size, &all_jobs_iova);

    for (int i = 0; i < queue_size; ++i) {
        job[i] = reinterpret_cast<qpl_job *>(all_jobs_buffer + i * job_size);
        status = qpl_init_job(config.execution_path, job[i]);
        if (status != QPL_STS_OK) {
            std::cerr << "Failed to init job: " << status << std::endl;
            return 1;
        }
        job[i]->numa_id = config.cxl_numa_id;
    }

    // Allocate source memory on NUMA node using 1GB Hugepages
    uint8_t* whole_src_ptr = (uint8_t*)huge_alloc_on_node(src_file_size, config.numa_node, true);
    uint64_t src_iova;
    qpl_cxl_register_buffer(whole_src_ptr, src_file_size, &src_iova);

    src_file.read(reinterpret_cast<char *>(whole_src_ptr), src_file_size);
    src_file.close();

    // Allocate destination buffers on NUMA node using 1GB Hugepages
    std::vector<uint8_t*> dest_ptrs(queue_size);
    uint8_t* all_dest_buffer = (uint8_t*)huge_alloc_on_node(queue_size * chunk_size, config.numa_node, true);
    uint64_t all_dest_iova;
    qpl_cxl_register_buffer(all_dest_buffer, queue_size * chunk_size, &all_dest_iova);

    for (int i = 0; i < queue_size; ++i) {
        dest_ptrs[i] = all_dest_buffer + i * chunk_size;
    }

    std::chrono::duration<int64_t, std::nano> elapsed_time_ns = std::chrono::nanoseconds::zero();
    std::size_t src_file_left = src_file_size;
    std::size_t current_idx = 0;

    auto whole_start = std::chrono::steady_clock::now();

    while(src_file_left > 0) {
        int enqueue_cnt = 0;
        for (int i = 0; i < queue_size; ++i) {
            size_t vector_size = (src_file_left <= chunk_size) ? src_file_left : chunk_size;

            job[i]->op                 = qpl_op_scan_eq;
            job[i]->level              = qpl_default_level;
            job[i]->next_in_ptr        = whole_src_ptr + current_idx;
            job[i]->next_out_ptr       = dest_ptrs[i];
            job[i]->available_in       = static_cast<uint32_t>(vector_size);
            job[i]->available_out      = static_cast<uint32_t>(chunk_size);
            job[i]->src1_bit_width     = input_vector_width;
            job[i]->num_input_elements = static_cast<uint32_t>(vector_size);
            job[i]->out_bit_width      = qpl_ow_32;
            job[i]->param_low          = boundary;

            current_idx += vector_size;
            src_file_left -= vector_size;
            enqueue_cnt = i + 1;
            if (src_file_left == 0) break;
        }

        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < enqueue_cnt; ++i) {
            status = qpl_submit_job(job[i]);
            if (status != QPL_STS_OK) {
                std::cerr << "Submission error: " << status << std::endl;
                return 1;
            }
        }
        for (int i = 0; i < enqueue_cnt; ++i) {
            status = qpl_wait_job(job[i]);
            if (status != QPL_STS_OK) {
                std::cerr << "Wait error: " << status << std::endl;
                return 1;
            }
        }
        auto end = std::chrono::steady_clock::now();
        elapsed_time_ns += end - start;

    }

    auto whole_end = std::chrono::steady_clock::now();
    auto total_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(whole_end - whole_start).count();
    double exec_time_sec = static_cast<double>(elapsed_time_ns.count()) / 1e9;

    std::cout << "\n\nScan performed successfully." << std::endl;
    std::cout << "Input size      = " << src_file_size << " Bytes" << std::endl;
    std::cout << "Execution Time  = " << elapsed_time_ns.count() << " ns (" << exec_time_sec << " s)" << std::endl;
    std::cout << "Total Time      = " << total_time_ns << " ns" << std::endl;
    std::cout << "Bandwidth       = " << static_cast<double>(src_file_size) / 1024 / 1024 / exec_time_sec << " MB/s" << std::endl;

    // Cleanup
    for (int i = 0; i < queue_size; ++i) {
        qpl_fini_job(job[i]);
    }
    qpl_cxl_deregister_buffer(all_dest_buffer);
    huge_free(all_dest_buffer, queue_size * chunk_size, true);
    qpl_cxl_deregister_buffer(all_jobs_buffer);
    huge_free(all_jobs_buffer, queue_size * job_size, false);
    qpl_cxl_deregister_buffer(whole_src_ptr);
    huge_free(whole_src_ptr, src_file_size, true);

    return 0;
}

int main(int argc, char** argv) {
    CxlBenchConfig config;
    int next_arg = parse_cxl_bench_args(argc, argv, config);
    if (next_arg < 0) return 1;

    if (argc < next_arg + 1) {
        std::cerr << "Usage: " << argv[0] << " <mode> <numa_node> [server_ip] <src_file> [queue_size] [chunk_size]\n";
        return 1;
    }

    std::string src_file = argv[next_arg];
    uint32_t queue_size = 64;
    chunk_size = 2097152; // 2MB

    if (argc > next_arg + 1) {
        queue_size = std::stoi(argv[next_arg + 1]);
    }
    if (argc > next_arg + 2) {
        chunk_size = std::stoul(argv[next_arg + 2]);
    }

    qpl_status status = init_cxl_bench(config);
    if (status != QPL_STS_OK) {
        std::cerr << "CXL Init failed: " << status << std::endl;
        return 1;
    }

    return cxl_iaa_scan(src_file, config, queue_size);
}
