# ==========================================================================
# Copyright (C) 2022 Intel Corporation
#
# SPDX-License-Identifier: MIT
# ==========================================================================

cmake_minimum_required(VERSION 3.15.0 FATAL_ERROR)

# Add all the layers required for the library
#   Core Layer (software optimizations and accelerator-related logic)
add_subdirectory(isal)
add_subdirectory(core-sw)
add_subdirectory(core-iaa)

#   Business logic layer
add_subdirectory(middle-layer)

#   User API
add_subdirectory(c_api)

#   RDMA Server (new)
add_subdirectory(rdma_server)

#   SNIC Agent (builds on ARM)
add_subdirectory(snic_agent)

# Build and install the resulting library
get_property(LIB_DEPS GLOBAL PROPERTY QPL_LIB_DEPS)

if ("${QPL_LIBRARY_TYPE}" STREQUAL "SHARED")
    add_library(qpl SHARED ${LIB_DEPS})
else ()
    add_library(qpl STATIC ${LIB_DEPS})
endif()

# Link UCT libraries (Publicly, so consumers link them too)
find_library(UCT_LIB uct)
find_library(UCS_LIB ucs)
if(UCT_LIB AND UCS_LIB)
    target_link_libraries(qpl PUBLIC ${UCT_LIB} ${UCS_LIB})
endif()

target_include_directories(qpl PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
    $<INSTALL_INTERFACE:include>
)

if (DYNAMIC_LOADING_LIBACCEL_CONFIG)
    target_link_libraries(qpl PRIVATE ${CMAKE_DL_LIBS})
else()
    target_link_libraries(qpl PRIVATE "$<$<PLATFORM_ID:Linux>:accel-config>")
endif()

# RDMA libraries
target_link_libraries(qpl PRIVATE rdmacm ibverbs)

# C++ filesystem library (used in middle-layer) requires additional linking for older GNU/Clang
target_link_libraries(qpl PRIVATE $<$<AND:$<CXX_COMPILER_ID:GNU>,$<VERSION_LESS:$<CXX_COMPILER_VERSION>,9.1>>:stdc++fs>)
target_link_libraries(qpl PRIVATE $<$<AND:$<CXX_COMPILER_ID:Clang>,$<VERSION_LESS:$<CXX_COMPILER_VERSION>,9.0>>:c++fs>)

set_target_properties(qpl PROPERTIES VERSION   ${QPL_SHARED_LIB_VERSION})
set_target_properties(qpl PROPERTIES SOVERSION ${QPL_SHARED_LIB_SOVERSION})

install(TARGETS qpl
        EXPORT ${PROJECT_NAME}Targets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(DIRECTORY ${QPL_PROJECT_DIR}/include/qpl/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/qpl
        FILES_MATCHING PATTERN "*.h")
