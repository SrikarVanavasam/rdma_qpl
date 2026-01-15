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

    // Initialize the RDMA client connection to the server
    // enable_odp: if true, register ODP MR for zero-copy access; if false, only use staging buffers
    bool initialize(const std::string& server_ip, bool enable_odp = true);

    // Check if the client is initialized and ready
    bool is_initialized() const { return initialized_; }

    // Get an available remote job slot ID
    int get_job_slot();

    // Release a remote job slot ID
    void release_job_slot(int slot_id);

    // Prepare an RDMA Write to be batched
    void prepare_write(const void* local_addr, size_t size, uint64_t remote_addr, uint32_t rkey, bool signaled = false);

    // Commit the prepared batch of writes
    bool commit_batch();

    // Perform an RDMA Write operation (Immediate)
    bool rdma_write(const void* local_addr, size_t size, uint64_t remote_addr, uint32_t rkey, bool signaled = true);

    // Perform an RDMA Read operation
    bool rdma_read(void* local_addr, size_t size, uint64_t remote_addr, uint32_t rkey);

    // Perform an RDMA Read with explicit lkey (for staging mode)
    bool rdma_read_with_lkey(void* local_addr, size_t size, uint64_t remote_addr, uint32_t rkey, uint32_t lkey);

    // Get the remote address for a specific data block type and slot ID
    uint64_t get_remote_data_block_addr(int slot_id, int block_idx); // block_idx: 0=src1, 1=src2, 2=dst
    uint32_t get_remote_data_block_rkey();

    // Get the remote address for a completion record and slot ID
    uint64_t get_remote_comp_addr(int slot_id);
    uint32_t get_remote_comp_rkey();

    // Get the remote portal address and rkey (round-robin across WQs)
    uint64_t get_remote_portal_addr();
    uint32_t get_remote_portal_rkey();
    
    // Get portal for specific WQ index
    uint64_t get_remote_portal_addr(uint32_t wq_idx);
    uint32_t get_remote_portal_rkey(uint32_t wq_idx);
    
    // Get number of WQs and next WQ index (round-robin)
    uint32_t get_num_wqs() const;
    uint32_t get_next_wq_index();  // Returns and increments wq_index_ with wrap

    // Get the local descriptor buffer for a specific slot (ODP mode)
    uint8_t* get_local_desc_buffer(int slot_id);

    // Staging buffer accessors (staging mode)
    void* get_data_staging(int slot_id);
    void* get_desc_staging(int slot_id);
    void* get_comp_staging(int slot_id);
    uint32_t get_data_staging_lkey();
    uint32_t get_desc_staging_lkey();
    uint32_t get_comp_staging_lkey();

    // Sync completion records from remote to local staging
    // num_slots: number of slots to sync (0 = all NUM_JOBS)
    bool sync_completions(uint32_t num_slots = 0);
    
    // Get the highest active slot index (for efficient completion sync)
    int get_max_active_slot();

    // Prepare write with explicit lkey (for staging mode)
    void prepare_write_with_lkey(const void* local_addr, size_t size, uint64_t remote_addr, 
                                  uint32_t rkey, uint32_t lkey, bool signaled = false);

private:
    RdmaClient(); // Private constructor for singleton
    ~RdmaClient();

    // Batching resources
    static const int MAX_BATCH_SIZE = 8;
    struct ibv_send_wr wr_batch_[MAX_BATCH_SIZE];
    struct ibv_sge sge_batch_[MAX_BATCH_SIZE];
    int batch_idx_ = 0;

    // RDMA resources
    struct rdma_event_channel* ec_ = nullptr;
    struct rdma_cm_id* cm_id_ = nullptr;
    struct ibv_pd* pd_ = nullptr;
    struct ibv_cq* cq_ = nullptr;
    struct ibv_qp* qp_ = nullptr;
    
    // Memory regions
    struct ibv_mr* send_mr_ = nullptr; // ODP MR for user data
    
    // Local descriptor pool (ODP mode)
    void* local_desc_pool_ = nullptr;

    // Staging pools (staging mode - explicit MR, no ODP)
    void* data_staging_pool_ = nullptr;  // NUM_JOBS * BLOCK_SIZE
    void* desc_staging_pool_ = nullptr;  // NUM_JOBS * DESC_SIZE
    void* comp_staging_pool_ = nullptr;  // NUM_JOBS * COMP_SIZE
    struct ibv_mr* data_staging_mr_ = nullptr;
    struct ibv_mr* desc_staging_mr_ = nullptr;
    struct ibv_mr* comp_staging_mr_ = nullptr;

    bool initialized_ = false;
    bool odp_enabled_ = false;  // True if ODP MR was registered
    std::string server_ip_;
    
    rdma::ConnPrivateData remote_config_; // Stores the configuration from the server

    // Job slot management - push in reverse order so stack pops 0,1,2...
    std::stack<int> free_job_slots_;
    std::mutex      slot_mutex_;
    int             max_active_slot_ = -1;  // Highest slot ID currently active
    
    // Multi-WQ round-robin dispatch
    // TODO: Make atomic if adding multi-threading support
    uint32_t        wq_index_ = 0;  // Current WQ index for round-robin
    
    // Helper to cleanup RDMA resources
    void cleanup_rdma_resources();
};

} // namespace qpl::ml::dispatcher

#endif // QPL_SOURCES_CORE_SW_DISPATCHER_INCLUDE_RDMA_CLIENT_HPP
