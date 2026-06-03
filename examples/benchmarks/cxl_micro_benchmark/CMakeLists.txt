# ##############################################################################
# Copyright (C) 2022 Intel Corporation
#
# SPDX-License-Identifier: MIT
# ##############################################################################

set(QPL_CXL_BENCH_SOURCES
    cxl_scan_exact_test.cpp
    cxl_compression_decompression_test.cpp
    cxl_crc64_test.cpp
    cxl_extract_test.cpp
    cxl_decompression_scan.cpp
    cxl_decompression_extract.cpp
    cxl_expand_test.cpp
    cxl_scan_range_test.cpp
    cxl_select_test.cpp
)

foreach(source_file ${QPL_CXL_BENCH_SOURCES})
    get_filename_component(exe_name ${source_file} NAME_WE)
    add_executable(${exe_name} ${source_file})
    target_link_libraries(${exe_name} qpl numa)
    target_include_directories(${exe_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()
