/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include <iostream>
#include <memory>
#include <vector>

#include "qpl/qpl.h"

#include "examples_utils.hpp" // for argument parsing function

constexpr const uint32_t source_size = 1000;

auto main(int argc, char** argv) -> int {
    try {
        std::cout << "Intel(R) Query Processing Library version is " << qpl_get_library_version() << ".\n";

        // Default to Software Path
        qpl_path_t execution_path = qpl_path_software;

        // Get path from input argument
        const int parse_ret = parse_execution_path(argc, argv, &execution_path);
        if (parse_ret != 0) { return 1; }

        // Get compression buffer size estimate
        const uint32_t compression_size = qpl_get_safe_deflate_compression_buffer_size(source_size);
        if (compression_size == 0) {
            std::cout << "Invalid source size. Source size exceeds the maximum supported size.\n";
            return 1;
        }

        // Source and output containers
        std::vector<uint8_t> source(source_size, 5);
        std::vector<uint8_t> destination(compression_size, 0);
        std::vector<uint8_t> reference(source_size, 0);

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

        // Performing a compression operation
        job->op            = qpl_op_compress;
        job->level         = qpl_default_level;
        job->next_in_ptr   = source.data();
        job->next_out_ptr  = destination.data();
        job->available_in  = source_size;
        job->available_out = static_cast<uint32_t>(destination.size());
        job->flags         = QPL_FLAG_FIRST | QPL_FLAG_LAST | QPL_FLAG_DYNAMIC_HUFFMAN | QPL_FLAG_OMIT_VERIFY;

        // Compression (Async)
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

        // Performing a decompression operation
        job->op            = qpl_op_decompress;
        job->next_in_ptr   = destination.data();
        job->next_out_ptr  = reference.data();
        job->available_in  = compressed_size;
        job->available_out = static_cast<uint32_t>(reference.size());
        job->flags         = QPL_FLAG_FIRST | QPL_FLAG_LAST;

        // Decompression (Async)
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
                return 1;
            }
        }

        std::cout << "Content was successfully compressed and decompressed.\n";
        std::cout << "Input size: " << source.size() << ", compressed size: " << compressed_size
                  << ", compression ratio: " << (float)source.size() / (float)compressed_size << ".\n";

        return 0;
    } catch (...) { return exception_handler(); }
}
