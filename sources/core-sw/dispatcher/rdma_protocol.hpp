#ifndef RDMA_PROTOCOL_HPP
#define RDMA_PROTOCOL_HPP

#include <cstdint>

namespace qpl::rdma {

constexpr uint16_t SERVER_PORT = 18516;
constexpr uint32_t BLOCK_SIZE = 2 * 1024 * 1024; // 2MB
constexpr uint32_t COMP_SIZE = 64; // 64 Bytes
constexpr uint32_t NUM_JOBS = 128;
constexpr uint32_t PORTAL_SIZE = 4096;
constexpr uint32_t DESC_SIZE = 64; // Size of IAA descriptor
constexpr uint32_t MAX_WQS = 8;    // Maximum number of WQs supported
constexpr int32_t QPL_RDMA_REMOTE_NUMA_ID = -100;  // ODP mode (On-Demand Paging)
constexpr int32_t QPL_RDMA_STAGING_NUMA_ID = -101; // Staging buffer mode (explicit MR)

// We assume a fixed stride per job for simplicity in this prototype.
// Job 'i' uses:
//   Src1: DataPoolBase + (i * 3 + 0) * BLOCK_SIZE
//   Src2: DataPoolBase + (i * 3 + 1) * BLOCK_SIZE
//   Dst:  DataPoolBase + (i * 3 + 2) * BLOCK_SIZE
//   Comp: CompPoolBase + i * COMP_SIZE

struct ConnPrivateData {
    // Multiple Portal (WQ) Memory Regions for round-robin dispatch
    uint32_t num_wqs;                     // Number of WQs available (1-MAX_WQS)
    uint64_t portal_addrs[MAX_WQS];       // Portal addresses for each WQ
    uint32_t portal_rkeys[MAX_WQS];       // Portal rkeys for each WQ

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