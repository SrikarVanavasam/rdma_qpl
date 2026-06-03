//* [QPL_CXL_CRC64_BENCHMARK] */

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
constexpr const uint64_t poly = 0x04C11DB700000000;

int cxl_iaa_crc64(std::string src_data_file_path, const CxlBenchConfig& config, const uint32_t queue_size)
{
    std::cout << "[CXL IAA CRC64]" << std::endl;
    
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

    // Allocate source memory on NUMA node using 1GB Hugepages
    uint8_t* whole_src_ptr = (uint8_t*)huge_alloc_on_node(src_file_size, config.numa_node, true);
    uint64_t src_iova;
    qpl_cxl_register_buffer(whole_src_ptr, src_file_size, &src_iova);

    src_file.read(reinterpret_cast<char *>(whole_src_ptr), src_file_size);
    src_file.close();

    std::chrono::duration<int64_t, std::nano> elapsed_time_ns = std::chrono::nanoseconds::zero();
    std::size_t src_file_left = src_file_size;
    std::size_t current_idx = 0;

    int job_idx = 0;
    int jobs_in_flight = 0;

    auto start_time = std::chrono::steady_clock::now();

    while (src_file_left > 0 || jobs_in_flight > 0) {
        // Submit
        while (src_file_left > 0 && jobs_in_flight < queue_size) {
            size_t vector_size = (src_file_left <= chunk_size) ? src_file_left : chunk_size;

            job[job_idx]->op           = qpl_op_crc64;
            job[job_idx]->next_in_ptr  = whole_src_ptr + current_idx;
            job[job_idx]->available_in = static_cast<uint32_t>(vector_size);
            job[job_idx]->crc64_poly   = poly;

            current_idx   += vector_size;
            src_file_left -= vector_size;

            status = qpl_submit_job(job[job_idx]);
            if (status != QPL_STS_OK) return 1;
            
            jobs_in_flight++;
            job_idx = (job_idx + 1) % queue_size;
        }

        // Wait
        if (jobs_in_flight > 0 && (jobs_in_flight == queue_size || src_file_left == 0)) {
            int wait_idx = (job_idx - jobs_in_flight + queue_size) % queue_size;
            status = qpl_wait_job(job[wait_idx]);
            if (status != QPL_STS_OK) return 1;
            jobs_in_flight--;
        }
    }

    auto end_time = std::chrono::steady_clock::now();
    elapsed_time_ns = end_time - start_time;
    double exec_time_sec = static_cast<double>(elapsed_time_ns.count()) / 1e9;

    std::cout << "\nCRC64 performed successfully." << std::endl;
    std::cout << "Input size      = " << src_file_size << " Bytes" << std::endl;
    std::cout << "Bandwidth       = " << static_cast<double>(src_file_size) / 1024 / 1024 / exec_time_sec << " MB/s" << std::endl;

    // Cleanup
    for (int i = 0; i < queue_size; ++i) {
        qpl_fini_job(job[i]);
    }
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

    std::cout << "Running CRC64 test with:" << std::endl;
    std::cout << "  Mode:       " << config.mode << " (NUMA ID: " << config.cxl_numa_id << ")" << std::endl;
    std::cout << "  NUMA Node:  " << config.numa_node << std::endl;
    std::cout << "  Server IP:  " << config.server_ip << std::endl;
    std::cout << "  Source File: " << src_file << std::endl;
    std::cout << "  Queue Size:  " << queue_size << std::endl;
    std::cout << "  Chunk Size:  " << chunk_size << std::endl;

    qpl_status status = init_cxl_bench(config);
    if (status != QPL_STS_OK) {
        std::cerr << "Failed to initialize CXL bench: " << status << std::endl;
        return 1;
    }

    return cxl_iaa_crc64(src_file, config, queue_size);
}
