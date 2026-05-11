#ifndef QPL_C_API_CXL_H_
#define QPL_C_API_CXL_H_

#include "qpl/c_api/status.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the CXL proxy client singleton.
 *
 * @param server_ip IP address of the registration server (e.g. "127.0.0.1")
 * @param cxl_bdf PCIe BDF of the CXL accelerator
 * @param numa_node Target NUMA node for memory allocations
 * @return qpl_status QPL_STS_OK on success, error otherwise.
 */
qpl_status qpl_cxl_initialize(const char* server_ip, const char* cxl_bdf, int numa_node);

/**
 * @brief Register a generic data buffer with the remote daemon to obtain its IOVA.
 *
 * @param buffer Virtual address of the buffer
 * @param size Size of the buffer in bytes
 * @param out_iova Pointer to store the resulting IOVA (can be NULL)
 * @return qpl_status QPL_STS_OK on success, error otherwise.
 */
qpl_status qpl_cxl_register_buffer(void* buffer, size_t size, uint64_t* out_iova);

/**
 * @brief Register a completion buffer with the remote daemon.
 *
 * This explicitly registers the buffer as a completion buffer and pins it on the remote side.
 * Must be 4KB page aligned.
 *
 * @param buffer Virtual address of the completion buffer
 * @param size Size of the buffer in bytes (typically 4096)
 * @param out_iova Pointer to store the resulting IOVA (can be NULL)
 * @return qpl_status QPL_STS_OK on success, error otherwise.
 */
qpl_status qpl_cxl_register_completion_buffer(void* buffer, size_t size, uint64_t* out_iova);

/**
 * @brief Deregister a previously registered buffer.
 *
 * @param buffer Virtual address of the buffer to deregister
 * @return qpl_status QPL_STS_OK on success, error otherwise.
 */
qpl_status qpl_cxl_deregister_buffer(void* buffer);

#ifdef __cplusplus
}
#endif

#endif // QPL_C_API_CXL_H_
