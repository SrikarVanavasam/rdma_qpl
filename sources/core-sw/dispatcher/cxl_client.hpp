#ifndef QPL_CXL_CLIENT_HPP
#define QPL_CXL_CLIENT_HPP

#include <cstdint>
#include <unordered_map>
#include <mutex>
#include <cstddef>

extern "C" {
    struct idxd_client_ctx;
}
struct rdma_event_channel;
struct rdma_cm_id;
struct ibv_pd;
struct ibv_cq;
struct ibv_mr;

namespace qpl::ml::dispatcher {

class CxlClient {
public:
    static CxlClient& get_instance() {
        static CxlClient instance;
        return instance;
    }

    // Disallow copy and assign
    CxlClient(const CxlClient&) = delete;
    CxlClient& operator=(const CxlClient&) = delete;

    // Initialize the CXL client
    bool initialize(const char* server_ip, const char* bdf, int numa_node);

    // Check if initialized
    bool is_initialized() const { return initialized_; }

    // Buffer management
    bool register_buffer(void* buffer, size_t size, uint64_t* out_iova);
    bool register_completion_buffer(void* buffer, size_t size, uint64_t* out_iova);
    bool deregister_buffer(void* buffer);
    uint64_t get_iova(void* buffer) { return get_remote_iova(buffer); }
    uint64_t get_remote_iova(void* buffer);
    uint64_t get_local_iova(void* buffer);

    // Proxy Setup (called automatically during first submission based on mode)
    bool setup_cxl_proxy(bool remote);
    bool setup_cpu_proxy();
    bool setup_rdma_proxy(int rdma_port);
    void* map_local_portal(int idxd_id, int wq_id);
    int submit_to_cpud(void* desc);
    uint32_t get_next_rdma_slot();
    int rdma_write_descriptor(void* desc);
    int rdma_clear_completion(uint32_t slot);
    int rdma_read_completion(uint32_t slot, void* comp_ptr);

    // Completion Slot management
    int get_comp_slot(bool remote);
    void release_comp_slot(int slot);
    uint64_t get_comp_iova(int slot);
    void* get_comp_ptr(int slot);

    // Accessors
    void* get_proxy_portal() const;
    void* get_local_portal() const { return local_portal_; }
    int get_remote_conn() const;
    idxd_client_ctx* get_remote_ctx() const { return remote_ctx_; }
    idxd_client_ctx* get_local_ctx() const { return local_ctx_; }

private:
    CxlClient() = default;
    ~CxlClient();

    idxd_client_ctx* remote_ctx_ = nullptr;
    idxd_client_ctx* local_ctx_  = nullptr;
    bool initialized_ = false;

    // Mapping from Virtual Address to IOVA
    struct RegisteredBuffer { uint64_t remote_iova; uint64_t local_iova; size_t size; };
    std::unordered_map<void*, RegisteredBuffer> va_to_iova_map_;
    // Mapping from Virtual Address to handle ID (for deregistration)
    struct RegisteredHandles { uint64_t remote_handle; uint64_t local_handle; };
    std::unordered_map<void*, RegisteredHandles> va_to_handle_map_;
    std::mutex map_mutex_;

    // Completion buffer specific tracking
    uint32_t remote_comp_handle_id_ = 0;
    uint32_t remote_comp_pin_handle_id_ = 0;
    uint32_t local_comp_handle_id_ = 0;
    uint32_t local_comp_pin_handle_id_ = 0;
    
    bool cxl_remote_setup_done_ = false;
    bool cxl_local_setup_done_ = false;
    bool cpu_proxy_setup_done_ = false;
    bool rdma_proxy_setup_done_ = false;

    void* local_portal_ = nullptr;

    // RDMA state
    rdma_event_channel* ec_ = nullptr;
    rdma_cm_id* id_ = nullptr;
    ibv_pd* pd_ = nullptr;
    
    struct ibv_mr* mr_desc_ = nullptr;
    struct ibv_mr* mr_comp_ = nullptr;
    
    // Remote connection metadata
    uint64_t remote_portal_addr_ = 0;
    uint32_t remote_portal_rkey_ = 0;
    uint64_t remote_comp_addr_ = 0;   // Server VA of completion buffer (for RDMA READ)
    uint32_t remote_comp_rkey_ = 0;

    // Local bounce buffers for RDMA
    void* rdma_desc_buf_ = nullptr; // 64 bytes * 32 slots
    void* rdma_comp_buf_ = nullptr; // 4096 bytes
    
    // Library-managed completion pages
    void* remote_comp_page_ = nullptr;
    void* local_comp_page_  = nullptr;
    uint64_t comp_page_remote_iova_ = 0;
    uint64_t comp_page_local_iova_  = 0;
    
    uint64_t remote_comp_slots_mask_ = 0;
    uint64_t local_comp_slots_mask_  = 0;
    std::mutex comp_mutex_;
    std::mutex setup_mutex_;

    int current_rdma_slot_ = 0;
    static constexpr int max_rdma_slots_ = 64;
};

} // namespace qpl::ml::dispatcher

#endif // QPL_CXL_CLIENT_HPP
