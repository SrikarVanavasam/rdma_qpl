/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include <iostream>
#include <memory>
#include <vector>
#include <cstdlib>
#include <cstring>

#include "qpl/qpl.h"

// Magic NUMA ID for Remote RDMA
#define QPL_RDMA_REMOTE_NUMA_ID (-100)

constexpr const uint32_t source_size = 1000;

auto main(int argc, char** argv) -> int {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <server_ip>\n";
        return 1;
    }

    // Set Environment Variable for QPL to find the server
    setenv("QPL_RDMA_SERVER_IP", argv[1], 1);

    std::cout << "Intel(R) Query Processing Library Remote RDMA Example\n";
    std::cout << "Server IP: " << argv[1] << "\n";

    // Force Hardware Path
    qpl_path_t execution_path = qpl_path_hardware;

    // Get compression buffer size estimate
    // Note: Remote path uses hardware sizing
    const uint32_t compression_size = qpl_get_safe_deflate_compression_buffer_size(source_size);

    // Source and output containers
    std::vector<uint8_t> source(source_size, 5);
    std::vector<uint8_t> destination(compression_size, 4);
    std::vector<uint8_t> reference(source_size, 7);

    // Initialize source data
    for (size_t i = 0; i < source.size(); ++i) {
        source[i] = i % 255;
    }

    std::unique_ptr<uint8_t[]> job_buffer;
    uint32_t                   size = 0;

    // Job initialization
    qpl_status status = qpl_get_job_size(execution_path, &size);
    if (status != QPL_STS_OK) {
        std::cout << "An error " << status << " acquired during job size getting.\n";
        return 1;
    }

    job_buffer   = std::make_unique<uint8_t[]>(size);
    qpl_job* job = reinterpret_cast<qpl_job*>(job_buffer.get());

    status = qpl_init_job(execution_path, job);
    if (status != QPL_STS_OK) {
        std::cout << "An error " << status << " acquired during job initializing.\n";
        return 1;
    }

    // *** ENABLE REMOTE MODE ***
    job->numa_id = QPL_RDMA_REMOTE_NUMA_ID;

    // Performing a compression operation
    job->op            = qpl_op_compress;
    job->level         = qpl_default_level;
    job->next_in_ptr   = source.data();
    job->next_out_ptr  = destination.data();
    job->available_in  = source_size;
    job->available_out = static_cast<uint32_t>(destination.size());
    job->flags         = QPL_FLAG_FIRST | QPL_FLAG_LAST | QPL_FLAG_DYNAMIC_HUFFMAN | QPL_FLAG_OMIT_VERIFY;

    // Compression (Async Submit + Wait)
    std::cout << "Submitting Compression Job via RDMA...\n";
    status = qpl_submit_job(job);
    if (status != QPL_STS_OK) {
        std::cout << "An error " << status << " acquired during compression submission.\n";
        return 1;
    }

    status = qpl_wait_job(job);
    if (status != QPL_STS_OK) {
        std::cout << "An error " << status << " acquired during compression wait.\n";
        return 1;
    }

    const uint32_t compressed_size = job->total_out;
    std::cout << "Compression Completed. Size: " << compressed_size << "\n";

    // Performing a decompression operation
    job->op            = qpl_op_decompress;
    job->next_in_ptr   = destination.data();
    job->next_out_ptr  = reference.data();
    job->available_in  = compressed_size;
    job->available_out = static_cast<uint32_t>(reference.size());
    job->flags         = QPL_FLAG_FIRST | QPL_FLAG_LAST;
    job->numa_id       = QPL_RDMA_REMOTE_NUMA_ID; // Ensure remote mode is still set

    // Decompression
    std::cout << "Submitting Decompression Job via RDMA...\n";
    status = qpl_submit_job(job);
    if (status != QPL_STS_OK) {
        std::cout << "An error " << status << " acquired during decompression submission.\n";
        return 1;
    }

    status = qpl_wait_job(job);
    if (status != QPL_STS_OK) {
        std::cout << "An error " << status << " acquired during decompression wait.\n";
        return 1;
    }

    std::cout << "Decompression Completed.\n";

    // Freeing resources
    status = qpl_fini_job(job);
    if (status != QPL_STS_OK) {
        std::cout << "An error " << status << " acquired during job finalization.\n";
        return 1;
    }

    // Compare reference functions
    for (size_t i = 0; i < source.size(); i++) {
        if (source[i] != reference[i]) {
            std::cout << "Content wasn't successfully compressed and decompressed.\n";
            std::cout << "Mismatch at " << i << ": Source " << (int)source[i] << " != Ref " << (int)reference[i] << "\n";
            return 1;
        }
    }

    std::cout << "Content was successfully compressed and decompressed.\n";
    std::cout << "Input size: " << source.size() << ", compressed size: " << compressed_size
              << ", compression ratio: " << (float)source.size() / (float)compressed_size << ".\n";

    return 0;
}
