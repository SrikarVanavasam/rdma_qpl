#ifndef RDMA_PROTOCOL_HPP
#define RDMA_PROTOCOL_HPP

#include <cstdint>

namespace qpl::rdma {

constexpr uint16_t SERVER_PORT = 18516;
constexpr uint32_t BLOCK_SIZE = 2 * 1024 * 1024; // 2MB
constexpr uint32_t COMP_SIZE = 64; // 64 Bytes
constexpr uint32_t NUM_JOBS = 16; 
constexpr uint32_t PORTAL_SIZE = 4096;

constexpr int32_t QPL_RDMA_REMOTE_NUMA_ID = -100; // Special NUMA ID to indicate remote execution

// We assume a fixed stride per job for simplicity in this prototype.
// Job 'i' uses:
//   Src1: DataPoolBase + (i * 3 + 0) * BLOCK_SIZE
//   Src2: DataPoolBase + (i * 3 + 1) * BLOCK_SIZE
//   Dst:  DataPoolBase + (i * 3 + 2) * BLOCK_SIZE
//   Comp: CompPoolBase + i * COMP_SIZE

struct ConnPrivateData {
    // The Portal (WQ) Memory Region
    uint64_t portal_addr;
    uint32_t portal_rkey;

    // The Data Buffer Pool (Large 2MB blocks)
    uint64_t data_pool_addr;
    uint32_t data_pool_rkey;
    uint32_t data_pool_count; // Total number of 2MB blocks

    // The Completion Record Pool (64B records)
    uint64_t comp_pool_addr;
    uint32_t comp_pool_rkey;
    uint32_t comp_pool_count; // Total number of 64B records
};

} // namespace qpl::rdma

#endif // RDMA_PROTOCOL_HPP