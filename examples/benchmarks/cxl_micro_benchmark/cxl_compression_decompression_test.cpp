//* [QPL_CXL_COMP_DECOMP_BENCHMARK] */

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

int cxl_iaa_comp_decomp(std::string src_data_file_path, const CxlBenchConfig& config, const uint32_t queue_size)
{
    std::cout << "[CXL IAA Compression & Decompression]" << std::endl;
    
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

    // Buffers (Using 1GB Hugepages)
    uint8_t* source_ptr = (uint8_t*)huge_alloc_on_node(src_file_size, config.numa_node, true);
    uint64_t src_iova;
    qpl_cxl_register_buffer(source_ptr, src_file_size, &src_iova);
    src_file.read(reinterpret_cast<char *>(source_ptr), src_file_size);
    src_file.close();

    // Calculate total number of chunks to allocate enough space for all compressed data
    size_t num_chunks = (src_file_size + chunk_size - 1) / chunk_size;
    std::vector<uint32_t> compressed_sizes(num_chunks, 0);
    uint8_t* all_compressed_buffer = (uint8_t*)huge_alloc_on_node(num_chunks * chunk_size, config.numa_node, true);
    uint64_t all_compressed_iova;
    qpl_cxl_register_buffer(all_compressed_buffer, num_chunks * chunk_size, &all_compressed_iova);

    uint8_t* decompressed_ptr = (uint8_t*)huge_alloc_on_node(src_file_size, config.numa_node, true);
    uint64_t decomp_iova;
    qpl_cxl_register_buffer(decompressed_ptr, src_file_size, &decomp_iova);

    // Compression
    std::size_t src_file_left = src_file_size;
    std::size_t current_idx = 0;
    std::size_t current_chunk_idx = 0;
    std::chrono::duration<int64_t, std::nano> comp_elapsed_ns = std::chrono::nanoseconds::zero();

    std::cout << "Compressing " << num_chunks << " chunks..." << std::endl;
    while(src_file_left > 0) {
        int enqueue_cnt = 0;
        size_t batch_chunk_start = current_chunk_idx;
        for (int i = 0; i < queue_size; ++i) {
            size_t vector_size = (src_file_left <= chunk_size) ? src_file_left : chunk_size;
            job[i]->op          = qpl_op_compress;
            job[i]->level       = qpl_default_level;
            job[i]->next_in_ptr = source_ptr + current_idx;
            job[i]->next_out_ptr = all_compressed_buffer + current_chunk_idx * chunk_size;
            job[i]->available_in = static_cast<uint32_t>(vector_size);
            job[i]->available_out = static_cast<uint32_t>(chunk_size);
            job[i]->flags       = QPL_FLAG_FIRST | QPL_FLAG_LAST | QPL_FLAG_DYNAMIC_HUFFMAN;

            current_idx += vector_size;
            src_file_left -= vector_size;
            current_chunk_idx++;
            enqueue_cnt = i + 1;
            if (src_file_left == 0) break;
        }

        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < enqueue_cnt; ++i) qpl_submit_job(job[i]);
        for (int i = 0; i < enqueue_cnt; ++i) qpl_wait_job(job[i]);
        auto end = std::chrono::steady_clock::now();
        comp_elapsed_ns += end - start;

        for (int i = 0; i < enqueue_cnt; ++i) {
            compressed_sizes[batch_chunk_start + i] = job[i]->total_out;
        }
    }

    double comp_bw = static_cast<double>(src_file_size) / 1024 / 1024 / (static_cast<double>(comp_elapsed_ns.count()) / 1e9);
    std::cout << "Compression Bandwidth: " << comp_bw << " MB/s" << std::endl;

    // Decompression
    std::chrono::duration<int64_t, std::nano> decomp_elapsed_ns = std::chrono::nanoseconds::zero();
    std::cout << "Decompressing..." << std::endl;

    src_file_left = src_file_size;
    current_idx = 0;
    current_chunk_idx = 0;
    
    while(src_file_left > 0) {
        int enqueue_cnt = 0;
        for (int i = 0; i < queue_size; ++i) {
            size_t vector_size = (src_file_left <= chunk_size) ? src_file_left : chunk_size;
            job[i]->op          = qpl_op_decompress;
            job[i]->next_in_ptr = all_compressed_buffer + current_chunk_idx * chunk_size;
            job[i]->next_out_ptr = decompressed_ptr + current_idx;
            job[i]->available_in = compressed_sizes[current_chunk_idx];
            job[i]->available_out = static_cast<uint32_t>(vector_size);
            job[i]->flags       = QPL_FLAG_FIRST | QPL_FLAG_LAST;

            current_idx += vector_size;
            src_file_left -= vector_size;
            current_chunk_idx++;
            enqueue_cnt = i + 1;
            if (src_file_left == 0) break;
        }

        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < enqueue_cnt; ++i) qpl_submit_job(job[i]);
        for (int i = 0; i < enqueue_cnt; ++i) qpl_wait_job(job[i]);
        auto end = std::chrono::steady_clock::now();
        decomp_elapsed_ns += end - start;
    }

    double decomp_bw = static_cast<double>(src_file_size) / 1024 / 1024 / (static_cast<double>(decomp_elapsed_ns.count()) / 1e9);
    std::cout << "Decompression Bandwidth: " << decomp_bw << " MB/s" << std::endl;

    // Cleanup
    for (int i = 0; i < queue_size; ++i) {
        qpl_fini_job(job[i]);
    }
    qpl_cxl_deregister_buffer(all_compressed_buffer);
    huge_free(all_compressed_buffer, num_chunks * chunk_size, true);
    qpl_cxl_deregister_buffer(all_jobs_buffer);
    huge_free(all_jobs_buffer, queue_size * job_size, false);
    qpl_cxl_deregister_buffer(source_ptr);
    huge_free(source_ptr, src_file_size, true);
    qpl_cxl_deregister_buffer(decompressed_ptr);
    huge_free(decompressed_ptr, src_file_size, true);

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

    return cxl_iaa_comp_decomp(src_file, config, queue_size);
}
