/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPL_SOURCES_C_API_LEGACY_HW_PATH_ASYNC_HW_API_H
#define QPL_SOURCES_C_API_LEGACY_HW_PATH_ASYNC_HW_API_H

// middle-layer
#include "accelerator/hw_accelerator_api.h"

#ifdef __cplusplus
extern "C" {
#endif

QPL_API(qpl_status, hw_submit_job, (qpl_job * qpl_job_ptr));

QPL_API(qpl_status, hw_check_job, (qpl_job * qpl_job_ptr));

QPL_API(uint32_t, hw_get_job_size, (void));

#ifdef __cplusplus
}
#endif

#endif //QPL_SOURCES_C_API_LEGACY_HW_PATH_ASYNC_HW_API_H
