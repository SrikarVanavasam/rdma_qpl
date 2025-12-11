#ifndef QPL_SOURCES_CORE_SW_DISPATCHER_INCLUDE_RDMA_CLIENT_HPP
#define QPL_SOURCES_CORE_SW_DISPATCHER_INCLUDE_RDMA_CLIENT_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <stack>
#include <mutex>
#include <memory>
#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>

#include "rdma_protocol.hpp"

namespace qpl::ml::dispatcher {

class RdmaClient {
public:
    static RdmaClient& get_instance();

    // Prevent copy and assign
    RdmaClient(const RdmaClient&) = delete;
    RdmaClient& operator=(const RdmaClient&) = delete;

    // Initialize the client connection to the specified server IP
    bool initialize(const std::string& server_ip);

    // Check if the client is initialized and ready
    bool is_initialized() const { return initialized_; }

    // Get an available remote job slot ID
    int get_job_slot();

    // Release a remote job slot ID
    void release_job_slot(int slot_id);

    // Perform an RDMA Write operation
    bool rdma_write(const void* local_addr, size_t size, uint64_t remote_addr, uint32_t rkey);
    
    // Perform an RDMA Read operation
    bool rdma_read(void* local_addr, size_t size, uint64_t remote_addr, uint32_t rkey);

    // Get the remote address for a specific data block type and slot ID
    uint64_t get_remote_data_block_addr(int slot_id, int block_idx); // block_idx: 0=src1, 1=src2, 2=dst
    uint32_t get_remote_data_block_rkey();

    // Get the remote address for a completion record and slot ID
    uint64_t get_remote_comp_addr(int slot_id);
    uint32_t get_remote_comp_rkey();

    // Get the remote portal address and rkey
    uint64_t get_remote_portal_addr();
    uint32_t get_remote_portal_rkey();

private:
    RdmaClient(); // Private constructor for singleton
    ~RdmaClient();

    // RDMA resources
    struct rdma_event_channel* ec_ = nullptr;
    struct rdma_cm_id* cm_id_ = nullptr;
    struct ibv_pd* pd_ = nullptr;
    struct ibv_cq* cq_ = nullptr;
    struct ibv_qp* qp_ = nullptr;
    
    // Memory regions for local send/recv buffers
    // These will be used for transferring data to/from remote
    struct ibv_mr* send_mr_ = nullptr;
    struct ibv_mr* recv_mr_ = nullptr;
    // We'll use a single buffer for small transfers (descriptor, completion)
    // For large data (2MB blocks), we'll need to register client's actual data
    // or copy to a local staging buffer for RDMA. For simplicity in this
    // prototype, we'll try to just register the user's data buffer on the fly.
    // If that proves too slow/complex, we'll revert to a local staging buffer.

    bool initialized_ = false;
    std::string server_ip_;
    
    rdma::ConnPrivateData remote_config_; // Stores the configuration from the server

    // Job slot management
    std::stack<int> free_job_slots_;
    std::mutex      slot_mutex_;
    
    // Helper to cleanup RDMA resources
    void cleanup_rdma_resources();
};

} // namespace qpl::ml::dispatcher

#endif // QPL_SOURCES_CORE_SW_DISPATCHER_INCLUDE_RDMA_CLIENT_HPP
