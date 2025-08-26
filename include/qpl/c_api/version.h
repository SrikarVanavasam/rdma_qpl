/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Job API (public C API)
 */

#ifndef QPL_C_API_VERSION_H
#define QPL_C_API_VERSION_H

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility push(default)
#endif

#include "qpl/c_api/defs.h"
#include "qpl/c_api/project_version.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Returns a string with a version of the library
 */
QPL_API(const char*, qpl_get_library_version, (void));

#ifdef __cplusplus
}
#endif

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC visibility pop
#endif

#endif //QPL_C_API_VERSION_H
