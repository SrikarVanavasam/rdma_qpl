/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPL_CXL_BENCH_UTILS_HPP_
#define QPL_CXL_BENCH_UTILS_HPP_

#include <iostream>
#include <string>
#include <vector>
#include <cstring>
#include <numa.h>
#include <numaif.h>
#include <chrono>
#include <sys/mman.h>
#include <dlfcn.h>

#include "qpl/qpl.h"

// Helper for NUMA-aware allocation
static inline void* numa_alloc_on_node(size_t size, int node) {
    void* ptr = numa_alloc_onnode(size, node);
    if (!ptr) {
        std::cerr << "Failed to allocate " << size << " bytes on NUMA node " << node << std::endl;
        exit(1);
    }
    // Touch memory to ensure physical allocation
    std::memset(ptr, 0, size);
    return ptr;
}

#ifndef MAP_HUGE_1GB
#define MAP_HUGE_1GB (30 << 26)
#endif

#ifndef MAP_HUGE_2MB
#define MAP_HUGE_2MB (21 << 26)
#endif

static inline void* huge_alloc_on_node(size_t size, int node, bool use_1gb = true) {
    size_t huge_page_size = use_1gb ? (1024ULL * 1024 * 1024) : (2ULL * 1024 * 1024);
    size_t rounded_size = ((size + huge_page_size - 1) / huge_page_size) * huge_page_size;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | (use_1gb ? MAP_HUGE_1GB : MAP_HUGE_2MB);

    void* ptr = mmap(NULL, rounded_size, PROT_READ | PROT_WRITE, flags, -1, 0);
    
    if (ptr == MAP_FAILED) {
        std::cerr << "Failed to allocate hugepage (" << (use_1gb ? "1GB" : "2MB") << ") of size " << rounded_size 
                  << ". Ensure hugepages are reserved." << std::endl;
        if (use_1gb) {
             std::cerr << "Hint: echo 4 | sudo tee /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages" << std::endl;
        } else {
             std::cerr << "Hint: echo 512 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages" << std::endl;
        }
        exit(1);
    }

    // Bind to the specific NUMA node
    unsigned long nodemask = (1UL << node);
    if (mbind(ptr, rounded_size, MPOL_BIND, &nodemask, sizeof(nodemask) * 8, 0) != 0) {
        perror("mbind");
    }

    std::memset(ptr, 0, rounded_size);
    return ptr;
}

static inline void huge_free(void* ptr, size_t size, bool use_1gb = true) {
    size_t huge_page_size = use_1gb ? (1024ULL * 1024 * 1024) : (2ULL * 1024 * 1024);
    size_t rounded_size = ((size + huge_page_size - 1) / huge_page_size) * huge_page_size;
    munmap(ptr, rounded_size);
}

static inline void print_cxl_help() {
    std::cout << "Usage: <benchmark> <mode> <numa_node> [server_ip]\n";
    std::cout << "Modes:\n";
    std::cout << "  local          - Local HW portal (busy-spin)\n";
    std::cout << "  local_umwait   - Local HW portal (umwait)\n";
    std::cout << "  cxl            - Remote CXL proxy (busy-spin)\n";
    std::cout << "  cxl_umwait     - Remote CXL proxy (umwait)\n";
    std::cout << "  cpu            - Software-mediated CPU proxy\n";
    std::cout << "  rdma           - RDMA network proxy\n";
    std::cout << "  combined       - Round-robin local + CXL proxy (busy-spin)\n";
    std::cout << "  combined_umwait - Round-robin local + CXL proxy (umwait)\n";
}

static inline int32_t get_cxl_numa_id(const std::string& mode) {
    if (mode == "local") return -106;
    if (mode == "local_umwait") return -107;
    if (mode == "cxl") return -102;
    if (mode == "cxl_umwait") return -103;
    if (mode == "cpu") return -104;
    if (mode == "rdma") return -105;
    if (mode == "combined") return -108;
    if (mode == "combined_umwait") return -109;
    return 0;
}

struct CxlBenchConfig {
    std::string mode;
    int numa_node;
    std::string server_ip;
    int32_t cxl_numa_id;
    qpl_path_t execution_path;
};

static inline int parse_cxl_bench_args(int argc, char** argv, CxlBenchConfig& config) {
    if (argc < 3) {
        print_cxl_help();
        return -1;
    }

    config.mode = argv[1];
    config.numa_node = std::stoi(argv[2]);
    config.cxl_numa_id = get_cxl_numa_id(config.mode);

    if (config.cxl_numa_id == 0) {
        std::cerr << "Unknown mode: " << config.mode << std::endl;
        print_cxl_help();
        return -1;
    }

    int next_arg = 3;
    if (config.mode == "local" || config.mode == "local_umwait") {
        config.server_ip = "127.0.0.1";
        // If they provided an IP anyway, skip it
        if (argc > 3 && std::string(argv[3]).find('.') != std::string::npos) {
            next_arg = 4;
        }
    } else {
        if (argc < 4) {
            std::cerr << "Error: Mode " << config.mode << " requires server_ip.\n";
            print_cxl_help();
            return -1;
        }
        config.server_ip = argv[3];
        next_arg = 4;
    }

    config.execution_path = (config.cxl_numa_id <= -102 && config.cxl_numa_id >= -109) ? qpl_path_pool : qpl_path_hardware;

    return next_arg;
}

static inline qpl_status init_cxl_bench(const CxlBenchConfig& config) {
    std::string final_ip = config.server_ip;
    if (config.mode == "combined" || config.mode == "combined_umwait") {
        final_ip = "combined:" + final_ip;
    }

    qpl_status status = qpl_cxl_initialize(final_ip.c_str(), "0000:40:00.1", config.numa_node);
    if (status != QPL_STS_OK) return status;

    // Register global CXL mempool SGL segments
    struct CxlSglEntryLocal { void* va; size_t size; };
    typedef size_t (*cxl_get_sgl_count_fn)(void);
    typedef void (*cxl_get_sgl_fn)(CxlSglEntryLocal*);

    cxl_get_sgl_count_fn get_sgl_count = (cxl_get_sgl_count_fn)dlsym(RTLD_DEFAULT, "cxl_mempool_get_sgl_count");
    cxl_get_sgl_fn get_sgl = (cxl_get_sgl_fn)dlsym(RTLD_DEFAULT, "cxl_mempool_get_sgl");

    if (get_sgl_count && get_sgl) {
        size_t count = get_sgl_count();
        std::cout << "[CXL BENCH INIT] Registering " << count << " SGL segment(s)..." << std::endl;
        std::vector<CxlSglEntryLocal> entries(count);
        get_sgl(entries.data());
        for (size_t i = 0; i < count; ++i) {
            uint64_t iova = 0;
            qpl_status reg_st = qpl_cxl_register_buffer(entries[i].va, entries[i].size, &iova);
            std::cout << "[CXL BENCH INIT] Segment " << i << " va=" << entries[i].va 
                      << " size=" << entries[i].size << " reg_status=" << reg_st 
                      << " iova=0x" << std::hex << iova << std::dec << std::endl;
        }
    } else {
        std::cout << "[CXL BENCH INIT] dlsym for SGL functions returned NULL!" << std::endl;
    }

    return QPL_STS_OK;
}

#endif // QPL_CXL_BENCH_UTILS_HPP_
