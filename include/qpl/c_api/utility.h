/*******************************************************************************
 * Copyright (C) 2024 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Utility API (public C++ API)
 */

#ifndef QPL_C_API_UTILITY_H
#define QPL_C_API_UTILITY_H

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility push(default)
#endif

#include <stdint.h>

#include "qpl/c_api/defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup UTILITY_API Utility API
 * @{
 */

/**
 * @brief Calculate the maximum buffer size for compression, compression output should not exceed this size.
 *
 * @param[in]  source_size  size of the input buffer
 *
 * @note This only applies to deflate compressions, Huffman Only mode is not supported.
 * @note When performing compression over multiple submissions, the user must call the API for each chunk of data.
 * @note This function does not include overhead for gzip/zlib headers and footers.
 *
 * @return uint32_t
 */
QPL_API(uint32_t, qpl_get_safe_deflate_compression_buffer_size, (uint32_t source_size));

/** @} */

/**
 * @defgroup RDMA_API RDMA API
 * @{
 */

/**
 * @brief Register a memory buffer for RDMA Zero-Copy access.
 *
 * @param[in]  buffer_ptr   Pointer to the start of the buffer.
 * @param[in]  buffer_size  Size of the buffer in bytes.
 *
 * @return QPL_STS_OK on success, or error code on failure.
 */
QPL_API(qpl_status, qpl_rdma_register_buffer, (void* buffer_ptr, size_t buffer_size));

/**
 * @brief Unregister a previously registered memory buffer.
 *
 * @param[in]  buffer_ptr   Pointer to the start of the buffer.
 *
 * @return QPL_STS_OK on success.
 */
QPL_API(qpl_status, qpl_rdma_unregister_buffer, (void* buffer_ptr));

/** @} */

#ifdef __cplusplus
}
#endif

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif

#endif //QPL_C_API_UTILITY_H
