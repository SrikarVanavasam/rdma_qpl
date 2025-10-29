# ==========================================================================
# Copyright (C) 2022 Intel Corporation
#
# SPDX-License-Identifier: MIT
# ==========================================================================

# Intel® Query Processing Library (Intel® QPL)
# Build system

enable_language(CXX)

file(GLOB SOURCES *.cpp)

add_library(thread_tests OBJECT ${SOURCES})

qpl_set_common_target_settings(thread_tests)

target_link_libraries(thread_tests
        PRIVATE tests_common
        PRIVATE qpl)

target_compile_definitions(thread_tests
        PRIVATE $<TARGET_PROPERTY:tests_common,COMPILE_DEFINITIONS>)

target_compile_options(thread_tests
        PRIVATE $<TARGET_PROPERTY:tests_common,COMPILE_OPTIONS>)
