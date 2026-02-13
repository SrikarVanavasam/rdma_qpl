/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * Multi-job RDMA CRC64 test to isolate ODP concurrency issues
 * Based on rdma_crc64_example.cpp but submits multiple jobs concurrently
 */

#include <iostream>
#include <memory>
#include <numeric>
#include <vector>
#include <cstdlib>

#include "qpl/qpl.h"

// Magic NUMA IDs for Remote RDMA
#define QPL_RDMA_REMOTE_NUMA_ID (-100)  // ODP mode

constexpr const uint64_t poly = 0x04C11DB700000000;

auto main(int argc, char** argv) -> int {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <server_ip> <num_jobs> [source_size]\n";
        std::cerr << "  num_jobs: number of concurrent jobs (1, 2, 4, etc.)\n";
        std::cerr << "  source_size: size of source data per job (default: 1000)\n";
        return 1;
    }

    // Set Environment Variable for QPL to find the server
    setenv("QPL_RDMA_SERVER_IP", argv[1], 1);

    const int num_jobs = std::atoi(argv[2]);
    if (num_jobs < 1 || num_jobs > 128) {
        std::cerr << "num_jobs must be between 1 and 128\n";
        return 1;
    }

    const uint32_t source_size = (argc > 3) ? std::atoi(argv[3]) : 1000;
    
    int rdma_numa_id = QPL_RDMA_REMOTE_NUMA_ID;

    std::cout << "Intel(R) Query Processing Library - Multi-Job RDMA CRC64 Test\n";
    std::cout << "Server IP: " << argv[1] << "\n";
    std::cout << "Number of jobs: " << num_jobs << "\n";
    std::cout << "Source size per job: " << source_size << " bytes\n";
    std::cout << "Mode: RDMA Zero-Copy\n";

    qpl_path_t execution_path = qpl_path_hardware;

    // Job initialization
    uint32_t job_size = 0;
    qpl_status status = qpl_get_job_size(execution_path, &job_size);
    if (status != QPL_STS_OK) {
        std::cout << "An error " << status << " acquired during job size getting.\n";
        return 1;
    }

    // Create source data for each job
    std::vector<std::vector<uint8_t>> sources(num_jobs);
    for (int i = 0; i < num_jobs; ++i) {
        sources[i].resize(source_size);
        // Fill with different patterns for each job
        std::iota(std::begin(sources[i]), std::end(sources[i]), static_cast<uint8_t>(i));
    }

    // Prefault all source pages (touch every page to ensure resident)
    std::cout << "Prefaulting source pages...\n";
    volatile uint8_t sum = 0;
    for (int i = 0; i < num_jobs; ++i) {
        for (size_t j = 0; j < source_size; j += 4096) {
            sum += sources[i][j];
        }
        if (source_size > 0) {
            sum += sources[i][source_size - 1];
        }
    }
    (void)sum;

    // Register buffers for Zero-Copy
    for (int i = 0; i < num_jobs; ++i) {
        if (qpl_rdma_register_buffer(sources[i].data(), source_size) != QPL_STS_OK) return 1;
    }
    std::cout << "Registered source buffers.\n";

    // Create job structures
    std::vector<std::unique_ptr<uint8_t[]>> job_buffers(num_jobs);
    std::vector<qpl_job*> jobs(num_jobs);

    for (int i = 0; i < num_jobs; ++i) {
        job_buffers[i] = std::make_unique<uint8_t[]>(job_size);
        jobs[i] = reinterpret_cast<qpl_job*>(job_buffers[i].get());

        status = qpl_init_job(execution_path, jobs[i]);
        if (status != QPL_STS_OK) {
            std::cout << "An error " << status << " acquired during job " << i << " initializing.\n";
            return 1;
        }

        // Enable RDMA mode (ODP or staging based on flag)
        jobs[i]->numa_id = rdma_numa_id;

        // Setup CRC64 operation
        jobs[i]->op           = qpl_op_crc64;
        jobs[i]->next_in_ptr  = sources[i].data();
        jobs[i]->available_in = source_size;
        jobs[i]->crc64_poly   = poly;
    }

    // Submit all jobs
    std::cout << "Submitting " << num_jobs << " CRC64 jobs via RDMA...\n";
    for (int i = 0; i < num_jobs; ++i) {
        status = qpl_submit_job(jobs[i]);
        if (status != QPL_STS_OK) {
            std::cout << "An error " << status << " acquired during job " << i << " submission.\n";
            return 1;
        }
        std::cout << "  Job " << i << " submitted\n";
    }

    // Wait for all jobs
    std::cout << "Waiting for all jobs...\n";
    for (int i = 0; i < num_jobs; ++i) {
        status = qpl_wait_job(jobs[i]);
        if (status != QPL_STS_OK) {
            std::cout << "An error " << status << " acquired during job " << i << " wait.\n";
            return 1;
        }
        std::cout << "  Job " << i << " completed with CRC: " << jobs[i]->crc64 << "\n";
    }

    // Cleanup
    for (int i = 0; i < num_jobs; ++i) {
        status = qpl_fini_job(jobs[i]);
        if (status != QPL_STS_OK) {
            std::cout << "An error " << status << " acquired during job " << i << " finalization.\n";
            return 1;
        }
        qpl_rdma_unregister_buffer(sources[i].data());
    }

    std::cout << "\nAll " << num_jobs << " jobs completed successfully!\n";

    return 0;
}
