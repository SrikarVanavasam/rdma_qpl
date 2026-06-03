//* [QPL_CXL_SELECT_BENCHMARK] */

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
constexpr const uint32_t boundary = 71; // 'G'

int cxl_iaa_select(std::string src_data_file_path, const CxlBenchConfig& config, const uint32_t queue_size)
{
    std::cout << "[CXL IAA Select]" << std::endl;
    
    std::ifstream src_file(src_data_file_path, std::ifstream::in | std::ifstream::binary);
    if (!src_file) return 1;

    src_file.seekg(0, std::ios::end);
    std::size_t src_file_size = static_cast<std::size_t>(src_file.tellg());
    src_file.seekg(0, std::ios::beg);

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

    // Use 1GB Hugepages for source
    uint8_t* whole_src_ptr = (uint8_t*)huge_alloc_on_node(src_file_size, config.numa_node, true);
    uint64_t src_iova;
    qpl_cxl_register_buffer(whole_src_ptr, src_file_size, &src_iova);
    src_file.read(reinterpret_cast<char *>(whole_src_ptr), src_file_size);
    src_file.close();

    std::vector<uint8_t*> mask_ptrs(queue_size);
    std::vector<uint8_t*> dest_ptrs(queue_size);
    // Use 1GB Hugepages for mask and destination
    uint8_t* all_mask_buffer = (uint8_t*)huge_alloc_on_node(queue_size * chunk_size, config.numa_node, true);
    uint64_t all_mask_iova;
    qpl_cxl_register_buffer(all_mask_buffer, queue_size * chunk_size, &all_mask_iova);

    uint8_t* all_dest_buffer = (uint8_t*)huge_alloc_on_node(queue_size * chunk_size, config.numa_node, true);
    uint64_t all_dest_iova;
    qpl_cxl_register_buffer(all_dest_buffer, queue_size * chunk_size, &all_dest_iova);

    for (int i = 0; i < queue_size; ++i) {
        mask_ptrs[i] = all_mask_buffer + i * chunk_size;
        dest_ptrs[i] = all_dest_buffer + i * chunk_size;
    }

    std::chrono::duration<int64_t, std::nano> elapsed_time_ns = std::chrono::nanoseconds::zero();
    std::size_t src_file_left = src_file_size;
    std::size_t current_idx = 0;
    std::vector<uint32_t> mask_lengths(queue_size, 0);

    auto start_time = std::chrono::steady_clock::now();

    while(src_file_left > 0) {
        int enqueue_cnt = 0;
        size_t batch_start_idx = current_idx;
        size_t batch_src_left = src_file_left;

        // --- Step 1: Scan to generate masks for the batch ---
        for (int i = 0; i < queue_size; ++i) {
            size_t vector_size = (batch_src_left <= chunk_size) ? batch_src_left : chunk_size;
            job[i]->op = qpl_op_scan_eq;
            job[i]->next_in_ptr = whole_src_ptr + batch_start_idx;
            job[i]->next_out_ptr = mask_ptrs[i];
            job[i]->available_in = static_cast<uint32_t>(vector_size);
            job[i]->available_out = static_cast<uint32_t>(vector_size);
            job[i]->src1_bit_width = input_vector_width;
            job[i]->num_input_elements = static_cast<uint32_t>(vector_size);
            job[i]->out_bit_width = qpl_ow_nom;
            job[i]->param_low = boundary;
            
            batch_start_idx += vector_size;
            batch_src_left -= vector_size;
            enqueue_cnt = i + 1;
            if (batch_src_left == 0) break;
        }

        auto s1 = std::chrono::steady_clock::now();
        for (int i = 0; i < enqueue_cnt; ++i) qpl_submit_job(job[i]);
        for (int i = 0; i < enqueue_cnt; ++i) qpl_wait_job(job[i]);
        auto e1 = std::chrono::steady_clock::now();
        elapsed_time_ns += (e1 - s1);

        for (int i = 0; i < enqueue_cnt; ++i) mask_lengths[i] = job[i]->total_out;

        // --- Step 2: Select using generated masks ---
        batch_start_idx = current_idx;
        batch_src_left = src_file_left;
        for (int i = 0; i < enqueue_cnt; ++i) {
            size_t vector_size = (batch_src_left <= chunk_size) ? batch_src_left : chunk_size;
            job[i]->op = qpl_op_select;
            job[i]->next_in_ptr = whole_src_ptr + batch_start_idx;
            job[i]->next_out_ptr = dest_ptrs[i];
            job[i]->available_in = static_cast<uint32_t>(vector_size);
            job[i]->available_out = static_cast<uint32_t>(vector_size);
            job[i]->src1_bit_width = input_vector_width;
            job[i]->num_input_elements = static_cast<uint32_t>(vector_size);
            job[i]->out_bit_width = qpl_ow_nom;
            job[i]->next_src2_ptr = mask_ptrs[i];
            job[i]->available_src2 = mask_lengths[i];
            job[i]->src2_bit_width = 1;

            batch_start_idx += vector_size;
            batch_src_left -= vector_size;
        }

        auto s2 = std::chrono::steady_clock::now();
        for (int i = 0; i < enqueue_cnt; ++i) qpl_submit_job(job[i]);
        for (int i = 0; i < enqueue_cnt; ++i) qpl_wait_job(job[i]);
        auto e2 = std::chrono::steady_clock::now();
        elapsed_time_ns += (e2 - s2);

        current_idx += (src_file_left - batch_src_left);
        src_file_left = batch_src_left;
    }

    double exec_time_sec = static_cast<double>(elapsed_time_ns.count()) / 1e9;
    std::cout << "\nSelect performed successfully." << std::endl;
    std::cout << "Bandwidth       = " << static_cast<double>(src_file_size) / 1024 / 1024 / exec_time_sec << " MB/s" << std::endl;

    // Cleanup
    for (int i = 0; i < queue_size; ++i) {
        qpl_fini_job(job[i]);
    }
    qpl_cxl_deregister_buffer(all_mask_buffer);
    huge_free(all_mask_buffer, queue_size * chunk_size, true);
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

    return cxl_iaa_select(src_file, config, queue_size);
}
