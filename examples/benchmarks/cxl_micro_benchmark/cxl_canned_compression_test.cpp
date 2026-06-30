//* [QPL_CXL_CANNED_COMP_DECOMP_BENCHMARK] */

#include <iostream>
#include <fstream>
#include <vector>
#include <memory>
#include <string>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cstdlib>

#include "qpl/qpl.h"
#include "cxl_bench_utils.hpp"

static size_t chunk_size = 2097152;

int cxl_iaa_canned_comp_decomp(std::string src_data_file_path, const CxlBenchConfig& config, const uint32_t queue_size)
{
    std::cout << "[CXL IAA Canned Compression & Decompression]" << std::endl;
    std::cout << "  Mode:       " << config.mode << " (NUMA ID: " << config.cxl_numa_id << ")" << std::endl;
    std::cout << "  NUMA Node:  " << config.numa_node << std::endl;
    
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
        std::cerr << "qpl_get_job_size failed: " << status << std::endl;
        return 1;
    }

    uint8_t* all_jobs_buffer = static_cast<uint8_t*>(std::malloc(queue_size * job_size));
    for (uint32_t i = 0; i < queue_size; ++i) {
        job[i] = reinterpret_cast<qpl_job *>(all_jobs_buffer + i * job_size);
        qpl_init_job(config.execution_path, job[i]);
        job[i]->numa_id = config.cxl_numa_id;
    }

    // Source buffer
    uint8_t* source_ptr = static_cast<uint8_t*>(std::malloc(src_file_size));
    src_file.read(reinterpret_cast<char *>(source_ptr), src_file_size);
    src_file.close();

    // Huffman table initialization (using DEFAULT_ALLOCATOR_C since libcxl_malloc intercepts malloc)
    qpl_huffman_table_t huffman_table = nullptr;
    status = qpl_deflate_huffman_table_create(combined_table_type, qpl_path_auto, DEFAULT_ALLOCATOR_C, &huffman_table);
    if (status != QPL_STS_OK) {
        std::cerr << "Failed to create Huffman table: " << status << std::endl;
        return 1;
    }

    qpl_histogram* deflate_histogram = static_cast<qpl_histogram*>(std::malloc(sizeof(qpl_histogram)));
    std::memset(deflate_histogram, 0, sizeof(qpl_histogram));
    size_t sample_size = std::min(src_file_size, static_cast<size_t>(2097152));
    std::cout << "[DEBUG] src_file_size=" << src_file_size << " sample_size=" << sample_size << " source_ptr=" << (void*)source_ptr << " histogram_ptr=" << (void*)deflate_histogram << std::endl;
    status = qpl_gather_deflate_statistics(source_ptr, sample_size, deflate_histogram, qpl_default_level, qpl_path_auto);
    std::cout << "[DEBUG] qpl_gather_deflate_statistics returned status=" << status << std::endl;
    if (status != QPL_STS_OK) {
        std::cerr << "Failed to gather statistics: " << status << std::endl;
        std::free(deflate_histogram);
        qpl_huffman_table_destroy(huffman_table);
        return 1;
    }

    status = qpl_huffman_table_init_with_histogram(huffman_table, deflate_histogram);
    std::free(deflate_histogram);
    if (status != QPL_STS_OK) {
        std::cerr << "Failed to init Huffman table: " << status << std::endl;
        qpl_huffman_table_destroy(huffman_table);
        return 1;
    }

    size_t num_chunks = (src_file_size + chunk_size - 1) / chunk_size;
    std::vector<uint32_t> compressed_sizes(num_chunks, 0);
    uint8_t* all_compressed_buffer = static_cast<uint8_t*>(std::malloc(num_chunks * chunk_size));
    uint8_t* decompressed_ptr = static_cast<uint8_t*>(std::malloc(src_file_size));

    // Canned Compression
    std::size_t src_file_left = src_file_size;
    std::size_t current_idx = 0;
    std::size_t current_chunk_idx = 0;
    std::chrono::duration<int64_t, std::nano> comp_elapsed_ns = std::chrono::nanoseconds::zero();

    std::cout << "Compressing (canned mode) " << num_chunks << " chunks..." << std::endl;
    while(src_file_left > 0) {
        uint32_t enqueue_cnt = 0;
        size_t batch_chunk_start = current_chunk_idx;
        for (uint32_t i = 0; i < queue_size; ++i) {
            size_t vector_size = (src_file_left <= chunk_size) ? src_file_left : chunk_size;
            job[i]->op          = qpl_op_compress;
            job[i]->level       = qpl_default_level;
            job[i]->next_in_ptr = source_ptr + current_idx;
            job[i]->next_out_ptr = all_compressed_buffer + current_chunk_idx * chunk_size;
            job[i]->available_in = static_cast<uint32_t>(vector_size);
            job[i]->available_out = static_cast<uint32_t>(chunk_size);
            job[i]->flags       = QPL_FLAG_FIRST | QPL_FLAG_LAST | QPL_FLAG_CANNED_MODE | QPL_FLAG_OMIT_VERIFY;
            job[i]->huffman_table = huffman_table;
            job[i]->numa_id     = config.cxl_numa_id;

            current_idx += vector_size;
            src_file_left -= vector_size;
            current_chunk_idx++;
            enqueue_cnt = i + 1;
            if (src_file_left == 0) break;
        }

        auto start = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < enqueue_cnt; ++i) qpl_submit_job(job[i]);
        for (uint32_t i = 0; i < enqueue_cnt; ++i) qpl_wait_job(job[i]);
        auto end = std::chrono::steady_clock::now();
        comp_elapsed_ns += end - start;

        for (uint32_t i = 0; i < enqueue_cnt; ++i) {
            compressed_sizes[batch_chunk_start + i] = job[i]->total_out;
        }
    }

    double comp_bw = static_cast<double>(src_file_size) / 1024 / 1024 / (static_cast<double>(comp_elapsed_ns.count()) / 1e9);
    std::cout << "Canned Compression Bandwidth: " << comp_bw << " MB/s" << std::endl;

    // Decompression
    std::chrono::duration<int64_t, std::nano> decomp_elapsed_ns = std::chrono::nanoseconds::zero();
    std::cout << "Decompressing (canned mode)..." << std::endl;

    src_file_left = src_file_size;
    current_idx = 0;
    current_chunk_idx = 0;
    
    while(src_file_left > 0) {
        uint32_t enqueue_cnt = 0;
        for (uint32_t i = 0; i < queue_size; ++i) {
            size_t vector_size = (src_file_left <= chunk_size) ? src_file_left : chunk_size;
            job[i]->op          = qpl_op_decompress;
            job[i]->next_in_ptr = all_compressed_buffer + current_chunk_idx * chunk_size;
            job[i]->next_out_ptr = decompressed_ptr + current_idx;
            job[i]->available_in = compressed_sizes[current_chunk_idx];
            job[i]->available_out = static_cast<uint32_t>(vector_size);
            job[i]->flags       = QPL_FLAG_FIRST | QPL_FLAG_LAST | QPL_FLAG_CANNED_MODE;
            job[i]->huffman_table = huffman_table;
            job[i]->numa_id     = config.cxl_numa_id;

            current_idx += vector_size;
            src_file_left -= vector_size;
            current_chunk_idx++;
            enqueue_cnt = i + 1;
            if (src_file_left == 0) break;
        }

        auto start = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < enqueue_cnt; ++i) qpl_submit_job(job[i]);
        for (uint32_t i = 0; i < enqueue_cnt; ++i) qpl_wait_job(job[i]);
        auto end = std::chrono::steady_clock::now();
        decomp_elapsed_ns += end - start;
    }

    double decomp_bw = static_cast<double>(src_file_size) / 1024 / 1024 / (static_cast<double>(decomp_elapsed_ns.count()) / 1e9);
    std::cout << "Canned Decompression Bandwidth: " << decomp_bw << " MB/s" << std::endl;

    // Cleanup
    for (uint32_t i = 0; i < queue_size; ++i) {
        qpl_fini_job(job[i]);
    }
    qpl_huffman_table_destroy(huffman_table);
    std::free(all_compressed_buffer);
    std::free(all_jobs_buffer);
    std::free(source_ptr);
    std::free(decompressed_ptr);

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

    return cxl_iaa_canned_comp_decomp(src_file, config, queue_size);
}
