/*******************************************************************************
 * Copyright (C) 2024 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Utility API (public C API)
 */

#include <cstdint>

#include "qpl/qpl.h"

#include "job.hpp"
#include "own_defs.h"
#include "rdma_client.hpp"

#ifdef __cplusplus
extern "C" {
#endif

// Calculate maximum size needed for compression operations
QPL_FUN(uint32_t, qpl_get_safe_deflate_compression_buffer_size, (uint32_t source_size)) {

    // Define constants
    const uint8_t BLOCK_HEADER_SIZE       = 5U;
    const uint8_t FIRST_BLOCK_HEADER_SIZE = 1U;
    const uint8_t OUTPUT_ACCUMULATOR_SIZE = 32U;
    const uint8_t END_OF_BLOCK_SIZE       = 2U;

    // Efficient divide by 65535, works for all values of x except for 0xFFFFFFFF
    auto div_65535 = [](uint64_t x) -> uint64_t {
        const uint8_t BITS_PER_BYTE          = 8U;
        const uint8_t DIV_ROUND_UP_INCREMENT = 1U;

        const uint64_t y = (x >> (BITS_PER_BYTE * 2)) + x + DIV_ROUND_UP_INCREMENT;
        return (y >> (BITS_PER_BYTE * 2));
    };

// Calculating stored block size, does not include first block header
#ifdef _WIN32
    auto stored_block_size = [&div_65535, BLOCK_HEADER_SIZE, FIRST_BLOCK_HEADER_SIZE](uint64_t osize) -> uint64_t {
#else
    auto stored_block_size = [&div_65535](uint64_t osize) -> uint64_t {
#endif
        const uint32_t ROUND_UP_OFFSET = UINT16_MAX - 1U;

        if (osize == 0) return BLOCK_HEADER_SIZE - FIRST_BLOCK_HEADER_SIZE;
        const uint64_t num_blks = div_65535(osize + ROUND_UP_OFFSET); // round up
        return osize + (num_blks * BLOCK_HEADER_SIZE) - FIRST_BLOCK_HEADER_SIZE;
    };

    // Estimate the maximum size needed for compression operations
    // Add 1 byte for the first block header, 32 bytes for the output accumulator, and 2 bytes for the EOB
    uint64_t num_bytes =
            stored_block_size(source_size) + FIRST_BLOCK_HEADER_SIZE + OUTPUT_ACCUMULATOR_SIZE + END_OF_BLOCK_SIZE;

    // Round up to even number of bytes for BE16 encoding
    if (num_bytes % 2 != 0) { num_bytes++; }

    // Check if the size is greater than UINT32_MAX since the function returns a uint32_t
    if (num_bytes > UINT32_MAX) { return 0U; }

    return static_cast<uint32_t>(num_bytes);
}

/**
 * @enum qpl_execution_record_t
 * @brief Enumeration for types of execution information.
 *
 * This enumeration defines the various types of execution information that can be queried
 * from the Intel® Query Processing Library (Intel® QPL).
 */
typedef enum { // NOLINT(performance-enum-size)
    QPL_EXECUTION_INFO_ELAPSED_TIME = 0,
    QPL_EXECUTION_INFO_WQ_IDX,
    QPL_EXECUTION_INFO_DEVICE_IDX
} qpl_execution_record_t;

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility push(default)
#endif

/**
 * @brief Get execution record information for the given job.
 */
QPL_FUN(qpl_status, qpl_get_execution_record,
        (const qpl_job* const job_ptr, qpl_execution_record_t info_type, void* info_value)) {
    if (job_ptr == nullptr || info_value == nullptr) { return QPL_STS_NULL_PTR_ERR; }

    qpl_hw_state* state_ptr = reinterpret_cast<qpl_hw_state*>(qpl::job::get_state(job_ptr));
    if (state_ptr == nullptr) { return QPL_STS_LIBRARY_INTERNAL_ERR; }

    switch (info_type) {
        case QPL_EXECUTION_INFO_ELAPSED_TIME:
            *reinterpret_cast<double*>(info_value) = state_ptr->execution_record.elapsed_time_;
            break;
        case QPL_EXECUTION_INFO_WQ_IDX:
            *reinterpret_cast<uint32_t*>(info_value) = state_ptr->execution_record.wq_idx_;
            break;
        case QPL_EXECUTION_INFO_DEVICE_IDX:
            *reinterpret_cast<uint32_t*>(info_value) = state_ptr->execution_record.device_idx_;
            break;
        default: return QPL_STS_INVALID_PARAM_ERR;
    }

    return QPL_STS_OK;
}

/**
 * @brief Register a memory buffer for RDMA Zero-Copy access.
 */
QPL_FUN(qpl_status, qpl_rdma_register_buffer, (void* buffer_ptr, size_t buffer_size)) {
    if (buffer_ptr == nullptr || buffer_size == 0) return QPL_STS_NULL_PTR_ERR;
    
    static auto& client = qpl::ml::dispatcher::RdmaClient::get_instance();
    if (client.register_buffer(buffer_ptr, buffer_size)) {
        return QPL_STS_OK;
    } else {
        return QPL_STS_LIBRARY_INTERNAL_ERR; 
    }
}

/**
 * @brief Unregister a previously registered memory buffer.
 */
QPL_FUN(qpl_status, qpl_rdma_unregister_buffer, (void* buffer_ptr)) {
    if (buffer_ptr == nullptr) return QPL_STS_NULL_PTR_ERR;

    static auto& client = qpl::ml::dispatcher::RdmaClient::get_instance();
    client.unregister_buffer(buffer_ptr);
    return QPL_STS_OK;
}

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif

#ifdef __cplusplus
}
#endif
