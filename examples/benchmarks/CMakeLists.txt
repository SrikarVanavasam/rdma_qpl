# ==========================================================================
# Copyright (C) 2022 Intel Corporation
#
# SPDX-License-Identifier: MIT
# ==========================================================================

# Find dependencies for TPC-H Q6 workloads
find_package(ZLIB)
find_path(LZ4_INCLUDE_DIR lz4.h)
find_library(LZ4_LIBRARY lz4)

# Microbenchmarks
file(GLOB MICROBENCH_SOURCES "micro_benchmark/*.cpp")

foreach(source_file ${MICROBENCH_SOURCES})
    get_filename_component(example_name ${source_file} NAME_WE)
    set(target_name "mb_${example_name}")
    
    add_executable(${target_name} ${source_file})
    qpl_set_common_target_settings(${target_name})
    target_link_libraries(${target_name} PRIVATE qpl)
    
    # Ensure C++17 for any that use filesystem or modern features
    target_compile_features(${target_name} PRIVATE cxx_std_17)
    
    # Link stdc++fs if needed (for older GCC/Clang)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
         target_link_libraries(${target_name} PRIVATE $<$<C_COMPILER_ID:GNU,Clang>:stdc++fs>)
    endif()
endforeach()

add_subdirectory(cxl_micro_benchmark)

# End-to-End TPC-H Q6
set(TPCH_Q6_DIR "end_to_end/tpch_q6")

# tpch_q6_lineitem_compression
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${TPCH_Q6_DIR}/iaa_lineitem_compression.cpp")
    add_executable(tpch_q6_lineitem_compression ${TPCH_Q6_DIR}/iaa_lineitem_compression.cpp)
    qpl_set_common_target_settings(tpch_q6_lineitem_compression)
    target_link_libraries(tpch_q6_lineitem_compression PRIVATE qpl)
    target_compile_features(tpch_q6_lineitem_compression PRIVATE cxx_std_17)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
         target_link_libraries(tpch_q6_lineitem_compression PRIVATE $<$<C_COMPILER_ID:GNU,Clang>:stdc++fs>)
    endif()
endif()

# tpch_q6_decompression_scan
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${TPCH_Q6_DIR}/decompression_scan.cpp")
    if(ZLIB_FOUND AND LZ4_LIBRARY)
        add_executable(tpch_q6_decompression_scan ${TPCH_Q6_DIR}/decompression_scan.cpp)
        qpl_set_common_target_settings(tpch_q6_decompression_scan)
        if(TARGET ZLIB::ZLIB)
             target_link_libraries(tpch_q6_decompression_scan PRIVATE qpl ZLIB::ZLIB ${LZ4_LIBRARY})
        else()
             target_link_libraries(tpch_q6_decompression_scan PRIVATE qpl ${ZLIB_LIBRARIES} ${LZ4_LIBRARY})
             target_include_directories(tpch_q6_decompression_scan PRIVATE ${ZLIB_INCLUDE_DIRS})
        endif()
        target_include_directories(tpch_q6_decompression_scan PRIVATE ${LZ4_INCLUDE_DIR})
        target_compile_features(tpch_q6_decompression_scan PRIVATE cxx_std_17)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_link_libraries(tpch_q6_decompression_scan PRIVATE $<$<C_COMPILER_ID:GNU,Clang>:stdc++fs>)
       endif()
    else()
        message(STATUS "ZLIB or LZ4 not found. Skipping tpch_q6_decompression_scan.")
        if(NOT ZLIB_FOUND)
            message(STATUS "  - ZLIB not found")
        endif()
        if(NOT LZ4_LIBRARY)
            message(STATUS "  - LZ4 not found")
        endif()
    endif()
endif()

# tpch_q6_cxl_decompression_scan
set(TPCH_Q6_CXL_DIR "end_to_end/tpch_q6_cxl")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${TPCH_Q6_CXL_DIR}/decompression_scan.cpp")
    if(ZLIB_FOUND AND LZ4_LIBRARY)
        add_executable(tpch_q6_cxl_decompression_scan ${TPCH_Q6_CXL_DIR}/decompression_scan.cpp)
        qpl_set_common_target_settings(tpch_q6_cxl_decompression_scan)
        target_link_libraries(tpch_q6_cxl_decompression_scan PRIVATE qpl numa ZLIB::ZLIB ${LZ4_LIBRARY})
        target_include_directories(tpch_q6_cxl_decompression_scan PRIVATE ${LZ4_INCLUDE_DIR} cxl_micro_benchmark)
        target_compile_features(tpch_q6_cxl_decompression_scan PRIVATE cxx_std_17)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_link_libraries(tpch_q6_cxl_decompression_scan PRIVATE $<$<C_COMPILER_ID:GNU,Clang>:stdc++fs>)
        endif()
    endif()
endif()
