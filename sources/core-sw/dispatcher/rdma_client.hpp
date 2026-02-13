#ifndef QPL_SOURCES_CORE_SW_DISPATCHER_INCLUDE_RDMA_CLIENT_HPP
#define QPL_SOURCES_CORE_SW_DISPATCHER_INCLUDE_RDMA_CLIENT_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <stack>
#include <mutex>
#include <memory>
#include <map>

#include "rdma_protocol.hpp"
#include "uct_transport.hpp" // New UCT Wrapper

namespace qpl::ml::dispatcher {

class RdmaClient {
public:
    static RdmaClient& get_instance();

    RdmaClient(const RdmaClient&) = delete;
    RdmaClient& operator=(const RdmaClient&) = delete;

    // Initialize UCT connection    // Initialization
    bool initialize(const std::string& server_ip);

    bool is_initialized() const { return initialized_; }

    int get_job_slot();
    void release_job_slot(int slot_id);

    // Async RDMA Write (put_zcopy). No flush.
    bool write(const void* local_addr, size_t size, uint64_t remote_addr, uint32_t rkey_idx, bool signaled = true);

    // Async RDMA Write (put_short). For small messages (inline).
    // Buffer logic: Uses local buffer directly (copied immediately). No registration needed.
    bool write_short(const void* local_addr, size_t size, uint64_t remote_addr, uint32_t rkey_idx);

    // Async RDMA Read (get_bcopy). No flush (requires check/flush later).
    bool read(void* local_addr, size_t size, uint64_t remote_addr, uint32_t rkey_idx);

    // --- Address Accessors ---
    uint64_t get_remote_data_block_addr(int slot_id, int block_idx);
    uint32_t get_remote_data_block_rkey(); // Return stored rkey index or value?
                                           // We'll return an Index into our vector of unpacked keys

    uint64_t get_remote_comp_addr(int slot_id);
    uint32_t get_remote_comp_rkey();

    uint64_t get_remote_portal_addr();
    uint32_t get_remote_portal_rkey();
    
    uint64_t get_remote_portal_addr(uint32_t wq_idx);
    uint32_t get_remote_portal_rkey(uint32_t wq_idx);
    
    uint32_t get_num_wqs() const;
    uint32_t get_next_wq_index();

    // Staging / ODP Accessors
    uint8_t* get_local_desc_buffer(int slot_id); // ODP

    void* get_data_staging(int slot_id);
    void* get_desc_staging(int slot_id);
    void* get_comp_staging(int slot_id);
    uint32_t get_data_staging_lkey();
    uint32_t get_desc_staging_lkey();
    uint32_t get_comp_staging_lkey();


    int get_max_active_slot();

    // --- Buffer Registration (Zero-Copy) ---
    bool register_buffer(void* addr, size_t length);
    void unregister_buffer(void* addr);
    bool is_registered(const void* addr, size_t length);

    // Internal helper to get unpacked RKey from "Index"
    uct_rkey_t get_rkey_val(uint32_t idx) const;

    // Async Read Support
    struct AsyncState {
        uct_completion_t comp;
        bool read_issued;
        bool is_zcopy; // True if direct zero-copy was used
        AsyncState() : comp{NULL, 2, UCS_OK}, read_issued(false), is_zcopy(false) {}
    };

    ucs_status_t read_async(int slot_id, void* dst, size_t size, uint64_t remote_addr, uint32_t rkey_idx);
    void poll();

private:
    RdmaClient();
    ~RdmaClient();

    // UCT Resources
    UctContext ctx_;
    std::unique_ptr<UctEndpoint> ep_;
    TcpConnection conn_;

    bool initialized_ = false;
    std::string server_ip_;
    
    rdma::ConnPrivateData remote_config_;

    // Unpacked Remote Keys
    // 0 = Portal, 1 = Data Pool, 2 = Comp Pool
    // Corresponds to legacy "rkey" which we now treat as indices into this vector
    std::vector<uct_rkey_bundle_t> remote_keys_;
    std::vector<void*> remote_key_bufs_; // keep buffers to release? No, bundle copy or ref?
                                         // uct_rkey_unpack docs say we must release bundle.
    
    // Staging Pools (Local)
    // We register these with UCT to get memh
    void* data_staging_pool_ = nullptr;
    void* desc_staging_pool_ = nullptr;
    void* comp_staging_pool_ = nullptr;
    
    uct_mem_h data_staging_memh_ = nullptr;
    uct_mem_h desc_staging_memh_ = nullptr;
    uct_mem_h comp_staging_memh_ = nullptr;

    // Slots
    std::stack<int> free_job_slots_;
    std::vector<AsyncState> meta_states_;
    std::vector<AsyncState> data_states_;
    std::mutex      slot_mutex_;
    int             max_active_slot_ = -1;
    uint32_t        wq_index_ = 0;

    void cleanup_resources();

    // Zero-Copy Registration
    struct MemRegion {
        uct_mem_h memh;
        void*     start;
        size_t    length;
    };
    // Map start_addr -> Region. Using map allows upper_bound lookup for ranges.
    std::map<void*, MemRegion> registered_regions_; 
    std::mutex                 region_mutex_;
    
    // Helper to find handle for a given buffer range
    uct_mem_h get_registered_memh(const void* addr, size_t length);
};

} // namespace qpl::ml::dispatcher

#endif // QPL_SOURCES_CORE_SW_DISPATCHER_INCLUDE_RDMA_CLIENT_HPP
