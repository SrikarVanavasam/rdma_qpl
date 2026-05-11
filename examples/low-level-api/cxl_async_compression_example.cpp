/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include <iostream>
#include <memory>
#include <vector>
#include <string>
#include <cstring>
#include <numa.h>
#include <numaif.h>

#include "qpl/qpl.h"

#include "examples_utils.hpp"

constexpr const uint32_t source_size = 1024 * 1024; // 1MB

void* numa_alloc_on_node(size_t size, int node) {
    void* ptr = numa_alloc_onnode(size, node);
    if (!ptr) {
        std::cerr << "Failed to allocate " << size << " bytes on NUMA node " << node << std::endl;
        exit(1);
    }
    // Touch memory to ensure physical allocation
    std::memset(ptr, 0, size);
    return ptr;
}

void print_help() {
    std::cout << "Usage: cxl_async_compression_example <mode> <numa_node> [server_ip]\n";
    std::cout << "Modes:\n";
    std::cout << "  local          - Local HW portal (busy-spin)\n";
    std::cout << "  local_umwait   - Local HW portal (umwait)\n";
    std::cout << "  cxl            - Remote CXL proxy (busy-spin)\n";
    std::cout << "  cxl_umwait     - Remote CXL proxy (umwait)\n";
    std::cout << "  cpu            - Software-mediated CPU proxy\n";
    std::cout << "  rdma           - RDMA network proxy\n";
}

auto main(int argc, char** argv) -> int {
    if (argc < 3) {
        print_help();
        return 1;
    }

    std::string mode = argv[1];
    int numa_node = std::stoi(argv[2]);
    const char* server_ip = (argc == 3 || mode == "local" || mode == "local_umwait") ? "127.0.0.1" : argv[3];

    int32_t cxl_numa_id = 0;
    if (mode == "local") cxl_numa_id = -106;
    else if (mode == "local_umwait") cxl_numa_id = -107;
    else if (mode == "cxl") cxl_numa_id = -102;
    else if (mode == "cxl_umwait") cxl_numa_id = -103;
    else if (mode == "cpu") cxl_numa_id = -104;
    else if (mode == "rdma") cxl_numa_id = -105;
    else {
        std::cerr << "Unknown mode: " << mode << std::endl;
        print_help();
        return 1;
    }

    try {
        std::cout << "Intel(R) Query Processing Library version is " << qpl_get_library_version() << ".\n";
        std::cout << "Running in mode: " << mode << " (NUMA ID: " << cxl_numa_id << ") on NUMA node: " << numa_node << "\n";

        // Initialize CXL Proxy Client
        qpl_status status = qpl_cxl_initialize(server_ip, "0000:40:00.1", numa_node);
        if (status != QPL_STS_OK) {
            std::cout << "Failed to initialize CXL client: " << status << "\n";
            return 1;
        }

        qpl_path_t execution_path = (cxl_numa_id <= -102 && cxl_numa_id >= -107) ? qpl_path_pool : qpl_path_hardware;

        // Get compression buffer size estimate
        const uint32_t compression_size = qpl_get_safe_deflate_compression_buffer_size(source_size);

        // Allocate memory on the specified NUMA node
        uint8_t* source_ptr = (uint8_t*)numa_alloc_on_node(source_size, numa_node);
        uint8_t* destination_ptr = (uint8_t*)numa_alloc_on_node(compression_size, numa_node);
        uint8_t* reference_ptr = (uint8_t*)numa_alloc_on_node(source_size, numa_node);

        // Fill source with some data
        for (uint32_t i = 0; i < source_size; i++) source_ptr[i] = (uint8_t)(i % 256);

        // Register buffers with CXL Proxy
        uint64_t iova;
        qpl_cxl_register_buffer(source_ptr, source_size, &iova);
        qpl_cxl_register_buffer(destination_ptr, compression_size, &iova);
        qpl_cxl_register_buffer(reference_ptr, source_size, &iova);

        // Job initialization
        uint32_t job_size = 0;
        qpl_get_job_size(execution_path, &job_size);
        
        // Library handles completion buffer internal management now, 
        // so we don't need 4096-byte alignment or manual registration here.
        uint8_t* job_buffer = (uint8_t*)numa_alloc_on_node(job_size, numa_node);
        qpl_job* job = reinterpret_cast<qpl_job*>(job_buffer);

        qpl_init_job(execution_path, job);
        job->numa_id = cxl_numa_id;

        // Performing a compression operation
        job->op            = qpl_op_compress;
        job->level         = qpl_default_level;
        job->next_in_ptr   = source_ptr;
        job->next_out_ptr  = destination_ptr;
        job->available_in  = source_size;
        job->available_out = compression_size;
        job->flags         = QPL_FLAG_FIRST | QPL_FLAG_LAST | QPL_FLAG_DYNAMIC_HUFFMAN | QPL_FLAG_OMIT_VERIFY;

        std::cout << "Submitting compression... " << std::flush;
        status = qpl_submit_job(job);
        if (status != QPL_STS_OK) {
            std::cout << "Error: " << status << "\n";
            return 1;
        }
        
        status = qpl_wait_job(job);
        if (status != QPL_STS_OK) {
            std::cout << "Error during wait: " << status << "\n";
            return 1;
        }
        std::cout << "Done.\n";

        const uint32_t compressed_size = job->total_out;

        // Performing a decompression operation
        job->op            = qpl_op_decompress;
        job->next_in_ptr   = destination_ptr;
        job->next_out_ptr  = reference_ptr;
        job->available_in  = compressed_size;
        job->available_out = source_size;
        job->flags         = QPL_FLAG_FIRST | QPL_FLAG_LAST;

        std::cout << "Submitting decompression... " << std::flush;
        status = qpl_submit_job(job);
        if (status != QPL_STS_OK) {
            std::cout << "Error: " << status << "\n";
            return 1;
        }
        
        status = qpl_wait_job(job);
        if (status != QPL_STS_OK) {
            std::cout << "Error during wait: " << status << "\n";
            return 1;
        }
        std::cout << "Done.\n";

        // Verification
        bool match = true;
        for (size_t i = 0; i < source_size; i++) {
            if (source_ptr[i] != reference_ptr[i]) {
                match = false;
                break;
            }
        }

        if (match) {
            std::cout << "SUCCESS: Content was successfully compressed and decompressed.\n";
            std::cout << "Input size: " << source_size << ", compressed size: " << compressed_size
                      << ", ratio: " << (float)source_size / (float)compressed_size << "\n";
        } else {
            std::cout << "FAILURE: Content mismatch!\n";
        }

        qpl_cxl_deregister_buffer(source_ptr);
        qpl_cxl_deregister_buffer(destination_ptr);
        qpl_cxl_deregister_buffer(reference_ptr);
        qpl_fini_job(job);

        numa_free(source_ptr, source_size);
        numa_free(destination_ptr, compression_size);
        numa_free(reference_ptr, source_size);
        numa_free(job_buffer, job_size);

        return match ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }
}
