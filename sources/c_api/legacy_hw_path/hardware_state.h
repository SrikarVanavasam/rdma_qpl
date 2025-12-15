/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @date 03/23/2020
 * @brief Types and Macro Definitions for Intel(R) Query Processing Library (Intel(R) QPL) Hardware Path.
 */

#ifndef QPL_SOURCES_C_API_LEGACY_HW_PATH_HARDWARE_STATE_H
#define QPL_SOURCES_C_API_LEGACY_HW_PATH_HARDWARE_STATE_H

#include "qpl/c_api/defs.h"
#include "qpl/c_api/status.h"

#include "core_deflate_api.h"
#include "hw_aecs_api.h"
#include "hw_completion_record_api.h"
#include "hw_definitions.h"
#include "hw_devices.h"
#include "stdbool.h"

#ifdef __cplusplus
extern "C" {
#endif

#define QPL_INTERNAL_BUFFER_SIZE 1024U /**< Max size of internal buffer */
#define FLIP_AECS_OFFSET(p)      ((p)->aecs_hw_read_offset = (p)->aecs_size - (p)->aecs_hw_read_offset) /**< @todo */

typedef enum { //NOLINT(performance-enum-size)
    qpl_task_execution_step_statistic_collection,
    qpl_task_execution_step_header_inserting,
    qpl_task_execution_step_data_processing,
    qpl_task_execution_step_verification,
    qpl_task_execution_step_completed,
} qpl_task_execution_step;

typedef enum { qpl_cst_fixed, qpl_cst_static, qpl_cst_dynamic } qpl_comp_style; //NOLINT(performance-enum-size)

typedef struct {
    bool                    first_job_has_been_submitted;
    qpl_task_execution_step execution_step;
    qpl_comp_style          comp_style;
    uint32_t                compress_crc; /**< Currently, contains stored CRC to be used for Verification */
    uint8_t*                saved_next_out_ptr;
} qpl_execution_history;

/**
 * @brief Holds temporal data for some operations
 */
typedef struct {
    uint8_t  data[QPL_INTERNAL_BUFFER_SIZE]; /**< Buffer data              */
    uint32_t actual_bytes;                   /**< Written bytes count      */
    uint32_t available_bytes;                /**< Available bytes to write */
} qpl_buffer;

/**
 * @brief Structure that represents the hardware state.
 *
 * This structure holds various information related to the hardware state of the library.
 * It includes descriptors, completion records, buffers, offsets, and various flags
 * that are used for execution on the accelerator.
 *
 * @warning The structure is aligned to 64 bytes to ensure proper memory alignment for descriptors.
 */
typedef struct {
    hw_decompress_analytics_descriptor desc_ptr; /**< @todo */
    hw_iaa_completion_record           comp_ptr; /**< @todo */
    hw_iaa_aecs_analytic               dcfg[2];  /**< @todo */
    hw_iaa_aecs_compress               ccfg[2];  /**< @todo */
    qpl_buffer                         accumulation_buffer;
    qpl_execution_history              execution_history;
    uint32_t                           config_valid;        /**< @todo */
    uint32_t                           aecs_hw_read_offset; /**< AECS read offset for deflate AECS */
    uint32_t                           aecs_size;           /**< @todo */
    hw_huffman_code                    eob_code;
    uint32_t                           saved_num_output_accum_bits; /**< @todo */
    hw_accelerator_context             accel_context;
    uint32_t                           descriptor_not_submitted;
    uint32_t                           verify_aecs_hw_read_offset; /**< AECS read offset for verify AECS */
    bool                      is_page_fault_processed; /**< Flag to limit resubmissions due to Page Fault to 1 */
    bool                      is_sw_fallback;          /**< Fallback to the SW path when Auto path is used */
    hw_multidescriptor_status multi_desc_status;       /**< Steps to be skipped in resubmitted jobs in case of
                                                                                QPL_STS_QUEUES_ARE_BUSY_ERR */
    qpl_status                async_job_status; /**< For asynchronous execution, stores the latest status of the job */
    qpl_execution_record      execution_record; /**< Execution information for logging */
    int32_t                   rdma_slot_id;     /**< RDMA job slot ID for remote execution */
    uint8_t*                  rdma_synced_src1; /**< Last src1 address synced to remote */
    uint8_t*                  rdma_synced_src2; /**< Last src2 address synced to remote (AECS) */
    uint8_t*                  rdma_synced_dst;  /**< Last dst address synced to remote */
} qpl_hw_state;

#ifdef __cplusplus
}
#endif

#endif //QPL_SOURCES_C_API_LEGACY_HW_PATH_HARDWARE_STATE_H
