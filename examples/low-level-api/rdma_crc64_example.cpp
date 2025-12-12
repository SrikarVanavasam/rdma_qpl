/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include <iostream>
#include <memory>
#include <numeric>
#include <vector>
#include <cstdlib>

#include "qpl/qpl.h"

// Magic NUMA ID for Remote RDMA
#define QPL_RDMA_REMOTE_NUMA_ID (-100)

constexpr const uint32_t source_size   = 1000;
constexpr const uint64_t poly          = 0x04C11DB700000000;
constexpr const uint64_t reference_crc = 6467333940108591104;

auto main(int argc, char** argv) -> int {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <server_ip>\n";
        return 1;
    }

    // Set Environment Variable for QPL to find the server
    setenv("QPL_RDMA_SERVER_IP", argv[1], 1);

    std::cout << "Intel(R) Query Processing Library Remote RDMA CRC64 Example\n";
    std::cout << "Server IP: " << argv[1] << "\n";

    qpl_path_t execution_path = qpl_path_hardware;

    // Source and output containers
    std::vector<uint8_t> source(source_size, 4);

    std::unique_ptr<uint8_t[]> job_buffer;
    uint32_t                   size = 0;

    // Filling source containers
    std::iota(std::begin(source), std::end(source), 0);

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

    // Performing an operation
    job->op           = qpl_op_crc64;
    job->next_in_ptr  = source.data();
    job->available_in = source_size;
    job->crc64_poly   = poly;

    std::cout << "Submitting CRC64 Job via RDMA...\n";
    status = qpl_submit_job(job);
    if (status != QPL_STS_OK) {
        std::cout << "An error " << status << " acquired during CRC submission.\n";
        return 1;
    }
    
    status = qpl_wait_job(job);
    if (status != QPL_STS_OK) {
        std::cout << "An error " << status << " acquired during CRC wait.\n";
        return 1;
    }

    const auto crc_value = job->crc64;

    // Freeing resources
    status = qpl_fini_job(job);
    if (status != QPL_STS_OK) {
        std::cout << "An error " << status << " acquired during job finalization.\n";
        return 1;
    }

    // Compare with reference
    if (crc_value != reference_crc) {
        std::cout << "CRC value was calculated incorrectly. Got: " << crc_value << " Expected: " << reference_crc << "\n";
        return 1;
    }

    std::cout << "CRC64 was performed successfully. Calculated CRC: " << crc_value << "\n";

    return 0;
}
