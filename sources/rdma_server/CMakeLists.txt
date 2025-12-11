cmake_minimum_required(VERSION 3.15.0 FATAL_ERROR)

project(qpl_rdma_server CXX)

add_executable(qpl_rdma_server server.cpp)

target_link_libraries(qpl_rdma_server PRIVATE rdmacm ibverbs)

target_include_directories(qpl_rdma_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../core-sw/dispatcher
)

install(TARGETS qpl_rdma_server DESTINATION bin)
