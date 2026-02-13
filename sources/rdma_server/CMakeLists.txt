cmake_minimum_required(VERSION 3.15.0 FATAL_ERROR)

project(qpl_rdma_server CXX)

# Definitions
set(CORE_SW_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../core-sw/dispatcher)

add_executable(qpl_rdma_server 
    server.cpp
    ${CORE_SW_DIR}/uct_transport.cpp
)

find_library(UCT_LIB uct)
find_library(UCS_LIB ucs)

if(UCT_LIB AND UCS_LIB)
    target_link_libraries(qpl_rdma_server PRIVATE ${UCT_LIB} ${UCS_LIB})
    message(STATUS "Found UCX libraries for server")
else()
    message(FATAL_ERROR "UCX libraries required for server")
endif()

# Link pthread
target_link_libraries(qpl_rdma_server PRIVATE pthread)

target_include_directories(qpl_rdma_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../core-sw/dispatcher
)

install(TARGETS qpl_rdma_server DESTINATION bin)
