#include "rdma_client.hpp"

#include <arpa/inet.h>
#include <climits>
#include <cstring>
#include <iostream>
#include <netdb.h>

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
    std::cout << "[RdmaClient] RDMA resources cleaned up." << std::endl;
}

bool RdmaClient::initialize(const std::string& server_ip) {
    if (initialized_) return true;

    server_ip_ = server_ip;
    std::cout << "[RdmaClient] Initializing ODP connection to " << server_ip_ << std::endl;

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

    send_mr_ = ibv_reg_mr(
            pd_, NULL, SIZE_MAX,
            IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_ON_DEMAND);

    if (!send_mr_) {
        std::cerr << "[RdmaClient] Failed to register ODP MR: " << strerror(errno) << std::endl;
        std::cerr << "             Ensure your device and kernel support On-Demand Paging." << std::endl;
        cleanup_rdma_resources();
        return false;
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
        std::cout << "[RdmaClient] Warning: Private data larger than expected. Got "
                  << (int)event->param.conn.private_data_len << ", using first " << sizeof(rdma::ConnPrivateData)
                  << " bytes." << std::endl;
    }
    remote_config_ = *reinterpret_cast<const rdma::ConnPrivateData*>(event->param.conn.private_data);
    rdma_ack_cm_event(event);

    std::cout << "[RdmaClient] Connected to server. Remote config received." << std::endl;
    std::cout << "  Portal: 0x" << std::hex << remote_config_.portal_addr << " (rkey: 0x" << remote_config_.portal_rkey
              << ")" << std::dec << std::endl;
    std::cout << "  Data Pool: 0x" << std::hex << remote_config_.data_pool_addr << " (rkey: 0x"
              << remote_config_.data_pool_rkey << ")" << std::dec << " (count: " << remote_config_.data_pool_count
              << ")" << std::endl;
    std::cout << "  Comp Pool: 0x" << std::hex << remote_config_.comp_pool_addr << " (rkey: 0x"
              << remote_config_.comp_pool_rkey << ")" << std::dec << " (count: " << remote_config_.comp_pool_count
              << ")" << std::endl;

    // Allocate local descriptor pool (64-byte aligned)
    // No need to register MR as we use ODP (send_mr_)
    if (posix_memalign((void**)&local_desc_pool_, 64, rdma::NUM_JOBS * rdma::DESC_SIZE)) {
        cleanup_rdma_resources();
        return false;
    }
    std::memset(local_desc_pool_, 0, rdma::NUM_JOBS * rdma::DESC_SIZE);

    std::lock_guard<std::mutex> lock(slot_mutex_);
    for (uint32_t i = 0; i < rdma::NUM_JOBS; ++i) {
        free_job_slots_.push(i);
    }

    initialized_ = true;
    std::cout << "[RdmaClient] RDMA ODP Connection Established." << std::endl;
    return true;
}

int RdmaClient::get_job_slot() {
    std::lock_guard<std::mutex> lock(slot_mutex_);
    if (free_job_slots_.empty()) return -1;
    int slot_id = free_job_slots_.top();
    free_job_slots_.pop();
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
    wr->send_flags = signaled ? IBV_SEND_SIGNALED : 0;
    // if (size <= 64) wr->send_flags |= IBV_SEND_INLINE;

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
    std::cout << "[RdmaClient] Committing batch of " << batch_idx_ << " WRs." << std::endl;
    int ret = ibv_post_send(qp_, &wr_batch_[0], &bad_wr);

    batch_idx_ = 0; // Reset

    if (ret) {
        std::cerr << "Post Send Failed: " << strerror(ret) << std::endl;
        return false;
    }
    return true;
}

bool RdmaClient::rdma_write(const void* local_addr, size_t size, uint64_t remote_addr, uint32_t rkey, bool signaled) {
    // Debug dump for 64-byte writes (descriptors)
    if (size == 64) {
        std::cout << "[RdmaClient] Debug: RDMA Write 64 bytes to 0x" << std::hex << remote_addr << " WR ID 0x"
                  << remote_addr << std::dec << std::endl;
        const uint8_t* data = static_cast<const uint8_t*>(local_addr);
        for (int i = 0; i < 64; ++i) {
            std::cout << std::hex << (int)data[i] << (i % 8 == 7 ? "\n" : " ");
        }
        std::cout << std::dec << std::endl;
    } else if (size == 1000) {
        std::cout << "[RdmaClient] Debug: RDMA Write 1000 bytes (Src1) to 0x" << std::hex << remote_addr << " WR ID 0x"
                  << remote_addr << std::dec << std::endl;
        const uint8_t* data = static_cast<const uint8_t*>(local_addr);
        for (int i = 0; i < 32; ++i) {
            std::cout << std::hex << (int)data[i] << (i % 8 == 7 ? "\n" : " ");
        }
        std::cout << std::dec << std::endl;
    } else {
        std::cout << "[RdmaClient] Debug: RDMA Write " << size << " bytes to 0x" << std::hex << remote_addr
                  << " WR ID 0x" << remote_addr << std::dec << std::endl;
    }

    struct ibv_sge sge = {};
    sge.addr           = reinterpret_cast<uint64_t>(const_cast<void*>(local_addr));
    sge.length         = static_cast<uint32_t>(size);
    sge.lkey           = send_mr_->lkey;

    struct ibv_send_wr wr = {};
    wr.wr_id              = remote_addr;
    wr.opcode             = IBV_WR_RDMA_WRITE;
    wr.send_flags         = signaled ? IBV_SEND_SIGNALED : 0;
    // if (size <= 64) { wr.send_flags |= IBV_SEND_INLINE; }
    wr.sg_list             = &sge;
    wr.num_sge             = 1;
    wr.wr.rdma.remote_addr = remote_addr;
    wr.wr.rdma.rkey        = rkey;

    struct ibv_send_wr* bad_wr = nullptr;
    std::cout << "[RdmaClient] Posting WR ID 0x" << std::hex << wr.wr_id << std::dec << std::endl;
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
            } else if (wc.wr_id != 1) { // Found a write completion (anything not 1)
                std::cout << "[RdmaClient] Debug: Write operation (WR ID 0x" << std::hex << wc.wr_id
                          << ") completed successfully. Opcode: " << (int)wc.opcode << " ByteLen: " << std::dec
                          << wc.byte_len << " VendorErr: 0x" << std::hex << wc.vendor_err << std::dec << std::endl;
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
    return remote_config_.portal_addr;
}
uint32_t RdmaClient::get_remote_portal_rkey() {
    return remote_config_.portal_rkey;
}

uint8_t* RdmaClient::get_local_desc_buffer(int slot_id) {
    if (slot_id < 0 || static_cast<uint32_t>(slot_id) >= rdma::NUM_JOBS || !local_desc_pool_) return nullptr;
    return static_cast<uint8_t*>(local_desc_pool_) + slot_id * rdma::DESC_SIZE;
}

} // namespace qpl::ml::dispatcher
