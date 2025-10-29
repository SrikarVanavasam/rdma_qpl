/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPL_SOURCES_ISAL_INCLUDE_FLATTEN_LL_H
#define QPL_SOURCES_ISAL_INCLUDE_FLATTEN_LL_H

#include <stdint.h>

#ifdef QPL_LIB
#ifdef __cplusplus
extern "C" {
#endif
#endif

void qpl_flatten_ll(uint32_t* ll_hist);

#ifdef QPL_LIB
#ifdef __cplusplus
} // extern "C"
#endif
#endif

#endif //QPL_SOURCES_ISAL_INCLUDE_FLATTEN_LL_H
