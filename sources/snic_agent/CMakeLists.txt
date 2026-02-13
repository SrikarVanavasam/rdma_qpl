# SNIC Agent CMakeLists.txt
# 
# Reuses existing RDMA client code for IAA submission
#
# Build on SNIC (ARM):
#   mkdir build-arm && cd build-arm
#   cmake ../sources/snic_agent
#   make

cmake_minimum_required(VERSION 3.16)

# Check if we're building standalone
if(NOT TARGET qpl)
    project(snic_agent LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    
    # Define source directories for standalone build
    set(CORE_SW_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../core-sw/dispatcher)
endif()

# If building as part of QPL project
if(TARGET qpl)
    set(CORE_SW_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../core-sw/dispatcher)
endif()

# SNIC Agent target
add_executable(snic_agent
    agent_main.cpp
    # New UCT transport code
    ${CORE_SW_DIR}/uct_transport.cpp
)

target_include_directories(snic_agent PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CORE_SW_DIR}
)

# Link RDMA libraries
find_library(RDMACM_LIB rdmacm)
find_library(IBVERBS_LIB ibverbs)

# Link UCX libraries (UCT, UCS)
find_library(UCT_LIB uct)
find_library(UCS_LIB ucs)

if(RDMACM_LIB AND IBVERBS_LIB)
    target_link_libraries(snic_agent PRIVATE ${RDMACM_LIB} ${IBVERBS_LIB})
    message(STATUS "Found RDMA libraries for snic_agent")
else()
    message(FATAL_ERROR "RDMA libraries required but not found")
endif()

if(UCT_LIB AND UCS_LIB)
    target_link_libraries(snic_agent PRIVATE ${UCT_LIB} ${UCS_LIB})
    message(STATUS "Found UCX libraries: ${UCT_LIB}, ${UCS_LIB}")
else()
    message(WARNING "UCX libraries (uct/ucs) not found. UCT transport will fail to link.")
endif()

# Link pthread
target_link_libraries(snic_agent PRIVATE pthread)

# Set output directory
set_target_properties(snic_agent PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# Installation
install(TARGETS snic_agent
    RUNTIME DESTINATION bin
)
