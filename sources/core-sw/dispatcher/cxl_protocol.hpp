#ifndef CXL_PROTOCOL_HPP
#define CXL_PROTOCOL_HPP

#include <cstdint>

namespace qpl::cxl {

constexpr int32_t QPL_CXL_PROXY_NUMA_ID        = -102; // Coherent CXL HW proxy (busy-spin)
constexpr int32_t QPL_CXL_PROXY_UMWAIT_NUMA_ID = -103; // Coherent CXL HW proxy (umwait)
constexpr int32_t QPL_CPU_PROXY_NUMA_ID        = -104; // Software-mediated CPU proxy
constexpr int32_t QPL_RDMA_PROXY_NUMA_ID       = -105; // RDMA network baseline (using idxd_client)
constexpr int32_t QPL_LOCAL_PROXY_NUMA_ID      = -106; // Local HW portal (busy-spin)
constexpr int32_t QPL_LOCAL_PROXY_UMWAIT_NUMA_ID = -107; // Local HW portal (umwait)

} // namespace qpl::cxl

#endif // CXL_PROTOCOL_HPP
