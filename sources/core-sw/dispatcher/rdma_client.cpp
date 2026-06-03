#include "rdma_client.hpp"

#include <arpa/inet.h>
#include <climits>
#include <cstring>
#include <iostream>
#include <netdb.h>
#include <sys/mman.h>

namespace qpl::ml::dispatcher {

RdmaClient& RdmaClient::get_instance() {
    static RdmaClient instance;
    return instance;
}

RdmaClient::RdmaClient() = default;

RdmaClient::~RdmaClient() {
    if (initialized_) { cleanup_rdma_resources(); }
}

void RdmaClient::cleanup_rdma_resources() {
    if (send_mr_) {
        ibv_dereg_mr(send_mr_);
        send_mr_ = nullptr;
    }
    if (local_desc_pool_) {
        free(local_desc_pool_);
        local_desc_pool_ = nullptr;
    }
    // Cleanup staging MRs
    if (data_staging_mr_) { ibv_dereg_mr(data_staging_mr_); data_staging_mr_ = nullptr; }
    if (desc_staging_mr_) { ibv_dereg_mr(desc_staging_mr_); desc_staging_mr_ = nullptr; }
    if (comp_staging_mr_) { ibv_dereg_mr(comp_staging_mr_); comp_staging_mr_ = nullptr; }
    // Cleanup staging pools
    if (data_staging_pool_) {
        munlock(data_staging_pool_, rdma::NUM_JOBS * 3 * rdma::BLOCK_SIZE);
        munmap(data_staging_pool_, rdma::NUM_JOBS * 3 * rdma::BLOCK_SIZE);
        data_staging_pool_ = nullptr;
    }
    if (desc_staging_pool_) { free(desc_staging_pool_); desc_staging_pool_ = nullptr; }
    if (comp_staging_pool_) { free(comp_staging_pool_); comp_staging_pool_ = nullptr; }
    if (qp_) {
        rdma_destroy_qp(cm_id_);
        qp_ = nullptr;
    }
    if (cq_) {
        ibv_destroy_cq(cq_);
        cq_ = nullptr;
    }
    if (pd_) {
        ibv_dealloc_pd(pd_);
        pd_ = nullptr;
    }
    if (cm_id_) {
        rdma_destroy_id(cm_id_);
        cm_id_ = nullptr;
    }
    if (ec_) {
        rdma_destroy_event_channel(ec_);
        ec_ = nullptr;
    }

    initialized_ = false;
}

bool RdmaClient::initialize(const std::string& server_ip, bool enable_odp) {
    if (initialized_) return true;

    server_ip_ = server_ip;
    odp_enabled_ = enable_odp;

    ec_ = rdma_create_event_channel();
    if (!ec_) return false;

    if (rdma_create_id(ec_, &cm_id_, NULL, RDMA_PS_TCP)) {
        cleanup_rdma_resources();
        return false;
    }

    struct sockaddr_in addr = {};
    addr.sin_family         = AF_INET;
    addr.sin_port           = htons(rdma::SERVER_PORT);
    inet_pton(AF_INET, server_ip_.c_str(), &addr.sin_addr);

    if (rdma_resolve_addr(cm_id_, NULL, (struct sockaddr*)&addr, 2000)) {
        cleanup_rdma_resources();
        return false;
    }

    struct rdma_cm_event* event = nullptr;
    if (rdma_get_cm_event(ec_, &event)) {
        cleanup_rdma_resources();
        return false;
    }
    rdma_ack_cm_event(event);

    if (rdma_resolve_route(cm_id_, 2000)) {
        cleanup_rdma_resources();
        return false;
    }
    if (rdma_get_cm_event(ec_, &event)) {
        cleanup_rdma_resources();
        return false;
    }
    rdma_ack_cm_event(event);

    pd_ = ibv_alloc_pd(cm_id_->verbs);
    if (!pd_) {
        cleanup_rdma_resources();
        return false;
    }

    // Only register ODP MR if ODP mode is enabled
    if (enable_odp) {
        send_mr_ = ibv_reg_mr(
                pd_, NULL, SIZE_MAX,
                IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_ON_DEMAND);

        if (!send_mr_) {
            std::cerr << "[RdmaClient] Failed to register ODP MR: " << strerror(errno) << std::endl;
            std::cerr << "             Ensure your device and kernel support On-Demand Paging." << std::endl;
            cleanup_rdma_resources();
            return false;
        }
    } else {
        send_mr_ = nullptr;  // Not using ODP, staging buffers only
    }

    cq_ = ibv_create_cq(cm_id_->verbs, rdma::NUM_JOBS * 4, NULL, NULL, 0);
    if (!cq_) {
        cleanup_rdma_resources();
        return false;
    }

    struct ibv_qp_init_attr qp_attr = {};
    qp_attr.send_cq                 = cq_;
    qp_attr.recv_cq                 = cq_;
    qp_attr.cap.max_send_wr         = rdma::NUM_JOBS * 4;
    qp_attr.cap.max_recv_wr         = 1;
    qp_attr.cap.max_send_sge        = 1;
    qp_attr.cap.max_recv_sge        = 1;
    qp_attr.cap.max_inline_data     = 64; // Required for IBV_SEND_INLINE
    qp_attr.qp_type                 = IBV_QPT_RC;

    if (rdma_create_qp(cm_id_, pd_, &qp_attr)) {
        cleanup_rdma_resources();
        return false;
    }
    qp_ = cm_id_->qp;

    struct rdma_conn_param cm_params = {};
    cm_params.initiator_depth        = 1;
    cm_params.responder_resources    = 1;
    cm_params.retry_count            = 7;

    if (rdma_connect(cm_id_, &cm_params)) {
        cleanup_rdma_resources();
        return false;
    }
    if (rdma_get_cm_event(ec_, &event)) {
        cleanup_rdma_resources();
        return false;
    }

    if (event->event != RDMA_CM_EVENT_ESTABLISHED) {
        rdma_ack_cm_event(event);
        cleanup_rdma_resources();
        return false;
    }

    if (event->param.conn.private_data_len < sizeof(rdma::ConnPrivateData)) {
        std::cerr << "[RdmaClient] Error: Private data too small. Expected at least " << sizeof(rdma::ConnPrivateData)
                  << ", got " << (int)event->param.conn.private_data_len << std::endl;
        rdma_ack_cm_event(event);
        cleanup_rdma_resources();
        return false;
    }
    if (event->param.conn.private_data_len > sizeof(rdma::ConnPrivateData)) {
        // Private data larger than expected, using first bytes
    }
    remote_config_ = *reinterpret_cast<const rdma::ConnPrivateData*>(event->param.conn.private_data);
    rdma_ack_cm_event(event);
    
    // std::cout << "[RdmaClient] Connected to server with " << remote_config_.num_wqs << " WQ(s)" << std::endl;

    // Allocate local descriptor pool (64-byte aligned)
    // No need to register MR as we use ODP (send_mr_)
    if (posix_memalign((void**)&local_desc_pool_, 64, rdma::NUM_JOBS * rdma::DESC_SIZE)) {
        cleanup_rdma_resources();
        return false;
    }
    std::memset(local_desc_pool_, 0, rdma::NUM_JOBS * rdma::DESC_SIZE);

    // Allocate and register staging pools (for staging mode - explicit MR, no ODP)
    // Data staging pool: NUM_JOBS * 3 * BLOCK_SIZE (src1, src2, dst per slot to match server)
    // Use huge pages (2MB) for better TLB performance and pre-faulted memory
    size_t data_pool_size = rdma::NUM_JOBS * 3 * rdma::BLOCK_SIZE;
    
    // Try huge pages first (MAP_HUGETLB), fall back to regular pages if unavailable
    data_staging_pool_ = mmap(nullptr, data_pool_size,
                               PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_POPULATE,
                               -1, 0);
    
    if (data_staging_pool_ == MAP_FAILED) {
        std::cerr << "[RdmaClient] Huge pages unavailable, falling back to regular pages" << std::endl;
        data_staging_pool_ = mmap(nullptr, data_pool_size,
                                   PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                                   -1, 0);
        if (data_staging_pool_ == MAP_FAILED) {
            std::cerr << "[RdmaClient] Failed to allocate data staging pool: " << strerror(errno) << std::endl;
            data_staging_pool_ = nullptr;
            cleanup_rdma_resources();
            return false;
        }
    } else {
        // std::cout << "[RdmaClient] Using huge pages for data staging pool" << std::endl;
    }
    
    // Pin in memory to prevent swapping and ensure consistent performance
    if (mlock(data_staging_pool_, data_pool_size) != 0) {
        std::cerr << "[RdmaClient] Warning: mlock failed (may need CAP_IPC_LOCK): " << strerror(errno) << std::endl;
        // Continue anyway - mlock is optional
    }
    
    // Touch all pages to ensure they're faulted in (MAP_POPULATE should do this, but be safe)
    std::memset(data_staging_pool_, 0, data_pool_size);
    
    data_staging_mr_ = ibv_reg_mr(pd_, data_staging_pool_, data_pool_size,
                                   IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
    if (!data_staging_mr_) {
        std::cerr << "[RdmaClient] Failed to register data staging MR: " << strerror(errno) << std::endl;
        cleanup_rdma_resources();
        return false;
    }

    // Descriptor staging pool: NUM_JOBS * DESC_SIZE
    if (posix_memalign(&desc_staging_pool_, 64, rdma::NUM_JOBS * rdma::DESC_SIZE)) {
        cleanup_rdma_resources();
        return false;
    }
    std::memset(desc_staging_pool_, 0, rdma::NUM_JOBS * rdma::DESC_SIZE);
    
    desc_staging_mr_ = ibv_reg_mr(pd_, desc_staging_pool_, rdma::NUM_JOBS * rdma::DESC_SIZE,
                                   IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
    if (!desc_staging_mr_) {
        std::cerr << "[RdmaClient] Failed to register desc staging MR: " << strerror(errno) << std::endl;
        cleanup_rdma_resources();
        return false;
    }

    // Completion staging pool: NUM_JOBS * COMP_SIZE
    if (posix_memalign(&comp_staging_pool_, 64, rdma::NUM_JOBS * rdma::COMP_SIZE)) {
        cleanup_rdma_resources();
        return false;
    }
    std::memset(comp_staging_pool_, 0, rdma::NUM_JOBS * rdma::COMP_SIZE);
    
    comp_staging_mr_ = ibv_reg_mr(pd_, comp_staging_pool_, rdma::NUM_JOBS * rdma::COMP_SIZE,
                                   IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
    if (!comp_staging_mr_) {
        std::cerr << "[RdmaClient] Failed to register comp staging MR: " << strerror(errno) << std::endl;
        cleanup_rdma_resources();
        return false;
    }

    // std::cout << "[RdmaClient] Staging pools allocated: Data=" << (rdma::NUM_JOBS * 3 * rdma::BLOCK_SIZE / (1024*1024)) 
    //           << "MB, Desc=" << (rdma::NUM_JOBS * rdma::DESC_SIZE / 1024) << "KB, Comp=" 
    //           << (rdma::NUM_JOBS * rdma::COMP_SIZE / 1024) << "KB" << std::endl;

    std::lock_guard<std::mutex> lock(slot_mutex_);
    // Push in reverse order so stack pops 0, 1, 2... (low slots first for accurate max_active_slot)
    for (int i = rdma::NUM_JOBS - 1; i >= 0; --i) {
        free_job_slots_.push(i);
    }

    initialized_ = true;
    return true;
}

int RdmaClient::get_job_slot() {
    std::lock_guard<std::mutex> lock(slot_mutex_);
    if (free_job_slots_.empty()) return -1;
    int slot_id = free_job_slots_.top();
    free_job_slots_.pop();
    // Track highest active slot for efficient completion sync
    if (slot_id > max_active_slot_) max_active_slot_ = slot_id;
    return slot_id;
}

void RdmaClient::release_job_slot(int slot_id) {
    std::lock_guard<std::mutex> lock(slot_mutex_);
    if (slot_id >= 0 && static_cast<uint32_t>(slot_id) < rdma::NUM_JOBS) free_job_slots_.push(slot_id);
}

void RdmaClient::prepare_write(const void* local_addr, size_t size, uint64_t remote_addr, uint32_t rkey,
                               bool signaled) {
    if (batch_idx_ >= MAX_BATCH_SIZE) {
        std::cerr << "[RdmaClient] Batch overflow!" << std::endl;
        return;
    }

    struct ibv_sge*     sge = &sge_batch_[batch_idx_];
    struct ibv_send_wr* wr  = &wr_batch_[batch_idx_];

    sge->addr   = reinterpret_cast<uint64_t>(const_cast<void*>(local_addr));
    sge->length = static_cast<uint32_t>(size);
    sge->lkey   = send_mr_->lkey; // ODP

    wr->wr_id      = remote_addr;
    wr->opcode     = IBV_WR_RDMA_WRITE;
    // Use inline for small payloads (64B descriptors) - avoids HCA DMA read
    wr->send_flags = (signaled ? IBV_SEND_SIGNALED : 0) | (size <= 64 ? IBV_SEND_INLINE : 0);

    wr->sg_list             = sge;
    wr->num_sge             = 1;
    wr->wr.rdma.remote_addr = remote_addr;
    wr->wr.rdma.rkey        = rkey;

    wr->next = NULL;
    if (batch_idx_ > 0) { wr_batch_[batch_idx_ - 1].next = wr; }

    batch_idx_++;
    // Debug print
    // std::cout << "[RdmaClient] Prepared WR #" << batch_idx_ << " ID 0x" << std::hex << remote_addr << std::dec << std::endl;
}

bool RdmaClient::commit_batch() {
    if (batch_idx_ == 0) return true;

    struct ibv_send_wr* bad_wr = nullptr;
    int                 ret    = ibv_post_send(qp_, &wr_batch_[0], &bad_wr);

    batch_idx_ = 0; // Reset

    if (ret) {
        std::cerr << "Post Send Failed: " << strerror(ret) << std::endl;
        return false;
    }
    return true;
}

bool RdmaClient::rdma_write(const void* local_addr, size_t size, uint64_t remote_addr, uint32_t rkey, bool signaled) {
    struct ibv_sge sge = {};
    sge.addr           = reinterpret_cast<uint64_t>(const_cast<void*>(local_addr));
    sge.length         = static_cast<uint32_t>(size);
    sge.lkey           = send_mr_->lkey;

    struct ibv_send_wr wr  = {};
    wr.wr_id               = remote_addr;
    wr.opcode              = IBV_WR_RDMA_WRITE;
    // Use inline for small payloads (64B descriptors) - avoids HCA DMA read
    wr.send_flags          = (signaled ? IBV_SEND_SIGNALED : 0) | (size <= 64 ? IBV_SEND_INLINE : 0);
    wr.sg_list             = &sge;
    wr.num_sge             = 1;
    wr.wr.rdma.remote_addr = remote_addr;
    wr.wr.rdma.rkey        = rkey;

    struct ibv_send_wr* bad_wr = nullptr;
    if (ibv_post_send(qp_, &wr, &bad_wr)) return false;

    return true;
}

bool RdmaClient::rdma_read(void* local_addr, size_t size, uint64_t remote_addr, uint32_t rkey) {
    struct ibv_sge sge = {};
    sge.addr           = reinterpret_cast<uint64_t>(local_addr);
    sge.length         = static_cast<uint32_t>(size);
    sge.lkey           = send_mr_->lkey;

    struct ibv_send_wr wr  = {};
    wr.wr_id               = 1;
    wr.opcode              = IBV_WR_RDMA_READ;
    wr.send_flags          = IBV_SEND_SIGNALED;
    wr.sg_list             = &sge;
    wr.num_sge             = 1;
    wr.wr.rdma.remote_addr = remote_addr;
    wr.wr.rdma.rkey        = rkey;

    struct ibv_send_wr* bad_wr = nullptr;
    if (ibv_post_send(qp_, &wr, &bad_wr)) return false;

    struct ibv_wc wc;
    int           completions = 0;
    do {
        completions = ibv_poll_cq(cq_, 1, &wc);
        if (completions > 0) {
            if (wc.status != IBV_WC_SUCCESS) {
                std::cerr << "[RdmaClient] CQ Error: " << ibv_wc_status_str(wc.status) << " for WR ID 0x" << std::hex
                          << wc.wr_id << " Opcode: " << (int)wc.opcode << " ByteLen: " << std::dec << wc.byte_len
                          << " VendorErr: 0x" << std::hex << wc.vendor_err << std::dec << std::endl;
                // If it's a read that failed (wr_id=1), propagate failure.
                if (wc.wr_id == 1) return false;
            } else if (wc.wr_id != 1) {
                // Write completion - ignore
            }
        }
    } while (completions == 0 || wc.wr_id != 1); // Keep polling until a read completion (WR ID 1) is found

    if (completions < 0) { // An actual error from poll_cq
        std::cerr << "RDMA Read Failed (poll_cq error): " << strerror(errno) << std::endl;
        return false;
    }

    // The previous loop ensures we break only when wc.wr_id == 1 and wc.status == IBV_WC_SUCCESS
    // No need for the while (wc.wr_id != 1) loop anymore.
    // The loop condition is `while (completions == 0 || wc.wr_id != 1)`.
    // It continues as long as no completion, or a write completion.
    // It exits when read completion.
    // So if it exits, wc.wr_id MUST be 1.

    return true;
}

bool RdmaClient::rdma_read_with_lkey(void* local_addr, size_t size, uint64_t remote_addr, uint32_t rkey, uint32_t lkey) {
    struct ibv_sge sge = {};
    sge.addr           = reinterpret_cast<uint64_t>(local_addr);
    sge.length         = static_cast<uint32_t>(size);
    sge.lkey           = lkey;  // Explicit lkey

    struct ibv_send_wr wr  = {};
    wr.wr_id               = 1;
    wr.opcode              = IBV_WR_RDMA_READ;
    wr.send_flags          = IBV_SEND_SIGNALED;
    wr.sg_list             = &sge;
    wr.num_sge             = 1;
    wr.wr.rdma.remote_addr = remote_addr;
    wr.wr.rdma.rkey        = rkey;

    struct ibv_send_wr* bad_wr = nullptr;
    if (ibv_post_send(qp_, &wr, &bad_wr)) return false;

    struct ibv_wc wc;
    int           completions = 0;
    do {
        completions = ibv_poll_cq(cq_, 1, &wc);
        if (completions > 0) {
            if (wc.status != IBV_WC_SUCCESS) {
                std::cerr << "[RdmaClient] CQ Error: " << ibv_wc_status_str(wc.status) << " for WR ID 0x" << std::hex
                          << wc.wr_id << " Opcode: " << (int)wc.opcode << " ByteLen: " << std::dec << wc.byte_len
                          << " VendorErr: 0x" << std::hex << wc.vendor_err << std::dec << std::endl;
                if (wc.wr_id == 1) return false;
            } else if (wc.wr_id != 1) {
                // Write completion - ignore
            }
        }
    } while (completions == 0 || wc.wr_id != 1);

    if (completions < 0) {
        std::cerr << "RDMA Read Failed (poll_cq error): " << strerror(errno) << std::endl;
        return false;
    }

    return true;
}

uint64_t RdmaClient::get_remote_data_block_addr(int slot_id, int block_idx) {
    return remote_config_.data_pool_addr + (static_cast<uint64_t>(slot_id) * 3 + block_idx) * rdma::BLOCK_SIZE;
}
uint32_t RdmaClient::get_remote_data_block_rkey() {
    return remote_config_.data_pool_rkey;
}

uint64_t RdmaClient::get_remote_comp_addr(int slot_id) {
    return remote_config_.comp_pool_addr + static_cast<uint64_t>(slot_id) * rdma::COMP_SIZE;
}
uint32_t RdmaClient::get_remote_comp_rkey() {
    return remote_config_.comp_pool_rkey;
}

uint64_t RdmaClient::get_remote_portal_addr() {
    // Use round-robin: get current WQ index and use it
    // Note: wq_index_ should be incremented by get_next_wq_index() before calling this
    uint32_t idx = wq_index_ % remote_config_.num_wqs;
    return remote_config_.portal_addrs[idx];
}
uint32_t RdmaClient::get_remote_portal_rkey() {
    uint32_t idx = wq_index_ % remote_config_.num_wqs;
    return remote_config_.portal_rkeys[idx];
}

uint64_t RdmaClient::get_remote_portal_addr(uint32_t wq_idx) {
    if (wq_idx >= remote_config_.num_wqs) wq_idx = 0;
    return remote_config_.portal_addrs[wq_idx];
}

uint32_t RdmaClient::get_remote_portal_rkey(uint32_t wq_idx) {
    if (wq_idx >= remote_config_.num_wqs) wq_idx = 0;
    return remote_config_.portal_rkeys[wq_idx];
}

uint32_t RdmaClient::get_num_wqs() const {
    return remote_config_.num_wqs;
}

uint32_t RdmaClient::get_next_wq_index() {
    // Return current index and increment (with wrap)
    // TODO: Make atomic if adding multi-threading support
    uint32_t idx = wq_index_;
    wq_index_ = (wq_index_ + 1) % remote_config_.num_wqs;
    return idx;
}

uint8_t* RdmaClient::get_local_desc_buffer(int slot_id) {
    if (slot_id < 0 || static_cast<uint32_t>(slot_id) >= rdma::NUM_JOBS || !local_desc_pool_) return nullptr;
    return static_cast<uint8_t*>(local_desc_pool_) + slot_id * rdma::DESC_SIZE;
}

// Staging buffer accessors
void* RdmaClient::get_data_staging(int slot_id) {
    if (slot_id < 0 || static_cast<uint32_t>(slot_id) >= rdma::NUM_JOBS || !data_staging_pool_) return nullptr;
    // Each slot has 3 blocks: src1, src2, dst (matches remote server layout)
    return static_cast<uint8_t*>(data_staging_pool_) + static_cast<size_t>(slot_id) * 3 * rdma::BLOCK_SIZE;
}

void* RdmaClient::get_desc_staging(int slot_id) {
    if (slot_id < 0 || static_cast<uint32_t>(slot_id) >= rdma::NUM_JOBS || !desc_staging_pool_) return nullptr;
    return static_cast<uint8_t*>(desc_staging_pool_) + slot_id * rdma::DESC_SIZE;
}

void* RdmaClient::get_comp_staging(int slot_id) {
    if (slot_id < 0 || static_cast<uint32_t>(slot_id) >= rdma::NUM_JOBS || !comp_staging_pool_) return nullptr;
    return static_cast<uint8_t*>(comp_staging_pool_) + slot_id * rdma::COMP_SIZE;
}

uint32_t RdmaClient::get_data_staging_lkey() { return data_staging_mr_ ? data_staging_mr_->lkey : 0; }
uint32_t RdmaClient::get_desc_staging_lkey() { return desc_staging_mr_ ? desc_staging_mr_->lkey : 0; }
uint32_t RdmaClient::get_comp_staging_lkey() { return comp_staging_mr_ ? comp_staging_mr_->lkey : 0; }

void RdmaClient::prepare_write_with_lkey(const void* local_addr, size_t size, uint64_t remote_addr,
                                          uint32_t rkey, uint32_t lkey, bool signaled) {
    if (batch_idx_ >= MAX_BATCH_SIZE) {
        std::cerr << "[RdmaClient] Batch overflow!" << std::endl;
        return;
    }

    struct ibv_sge*     sge = &sge_batch_[batch_idx_];
    struct ibv_send_wr* wr  = &wr_batch_[batch_idx_];

    sge->addr   = reinterpret_cast<uint64_t>(const_cast<void*>(local_addr));
    sge->length = static_cast<uint32_t>(size);
    sge->lkey   = lkey;  // Explicit lkey

    wr->wr_id      = remote_addr;
    wr->opcode     = IBV_WR_RDMA_WRITE;
    // Use inline for small payloads (64B descriptors) - avoids HCA DMA read
    wr->send_flags = (signaled ? IBV_SEND_SIGNALED : 0) | (size <= 64 ? IBV_SEND_INLINE : 0);

    wr->sg_list             = sge;
    wr->num_sge             = 1;
    wr->wr.rdma.remote_addr = remote_addr;
    wr->wr.rdma.rkey        = rkey;

    wr->next = NULL;
    if (batch_idx_ > 0) { wr_batch_[batch_idx_ - 1].next = wr; }

    batch_idx_++;
}

bool RdmaClient::sync_completions(uint32_t num_slots) {
    if (!comp_staging_pool_ || !comp_staging_mr_) return false;
    
    // If num_slots is 0 or > NUM_JOBS, sync all
    uint32_t slots_to_sync = (num_slots == 0 || num_slots > rdma::NUM_JOBS) 
                              ? rdma::NUM_JOBS : num_slots;
    
    // Read completion records for first 'slots_to_sync' slots
    uint64_t remote_comp_base = remote_config_.comp_pool_addr;
    size_t total_size = slots_to_sync * rdma::COMP_SIZE;
    
    return rdma_read_with_lkey(comp_staging_pool_, total_size, remote_comp_base,
                               remote_config_.comp_pool_rkey, comp_staging_mr_->lkey);
}

int RdmaClient::get_max_active_slot() {
    std::lock_guard<std::mutex> lock(slot_mutex_);
    return max_active_slot_;
}

} // namespace qpl::ml::dispatcher
