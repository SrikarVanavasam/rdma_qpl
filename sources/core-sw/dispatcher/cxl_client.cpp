#include "cxl_client.hpp"

#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>

#ifdef ACC_POOL_PATH
extern "C" {
#include "idxd_client.h"
#include "idxd_reg_proto.h"
#include "idxd.h"
}
#include <immintrin.h>
#include <x86intrin.h>
#include <numa.h>
#endif

namespace qpl::ml::dispatcher {

CxlClient::~CxlClient() {
#ifdef ACC_POOL_PATH
    if (ctx_) {
        // Deregister all mapped buffers
        for (const auto& pair : va_to_handle_map_) {
            idxd_deregister_buf(idxd_client_get_remote_conn(ctx_), pair.second);
        }
        
        if (comp_page_) {
            idxd_deregister_buf(idxd_client_get_remote_conn(ctx_), comp_handle_id_);
            numa_free(comp_page_, 4096);
        }

        idxd_client_deinit(ctx_);
        ctx_ = nullptr;
    }
#endif
}

bool CxlClient::initialize(const char* server_ip, const char* bdf, int numa_node) {
#ifdef ACC_POOL_PATH
    if (initialized_) return true;

    idxd_client_init_params params = {};
    params.server_ip = server_ip;
    params.port = IDXD_REG_DEFAULT_PORT;
    params.bdf = bdf;
    params.numa_node = numa_node;

    std::cout << "[QPL CXL] Initializing with server_ip=" << server_ip << ", bdf=" << bdf << ", numa_node=" << numa_node << std::endl;
    int rc = idxd_client_init(&params, &ctx_);
    if (rc != 0) {
        std::cerr << "[QPL CXL] Failed to initialize CXL client, rc=" << rc << std::endl;
        return false;
    }

    // Allocate and register library-managed completion page
    comp_page_ = numa_alloc_onnode(4096, numa_node);
    if (!comp_page_) {
        std::cerr << "[QPL CXL] Failed to allocate completion page" << std::endl;
        return false;
    }
    memset(comp_page_, 0, 4096);

    int remote_conn = idxd_client_get_remote_conn(ctx_);
    idxd_reg_result reg_res;
    memset(&reg_res, 0, sizeof(reg_res));

    if (idxd_register_completion_buf(remote_conn, comp_page_, 4096, &reg_res) != 0) {
        std::cerr << "[QPL CXL] Failed to register library-managed completion buffer" << std::endl;
        numa_free(comp_page_, 4096);
        comp_page_ = nullptr;
        return false;
    }
    std::cout << "[QPL CXL] Registered completion buffer: iova=0x" << std::hex << reg_res.dma_addr << std::dec << ", handle=" << reg_res.handle_id << std::endl;

    comp_handle_id_ = reg_res.handle_id;
    comp_pin_handle_id_ = reg_res.pin_handle_id;
    comp_page_iova_ = reg_res.dma_addr;

    initialized_ = true;
    return true;
#else
    (void)server_ip; (void)bdf; (void)numa_node;
    std::cerr << "[QPL CXL] QPL was not compiled with ACC_POOL_PATH support." << std::endl;
    return false;
#endif
}

bool CxlClient::register_buffer(void* buffer, size_t size, uint64_t* out_iova) {
#ifdef ACC_POOL_PATH
    if (!initialized_ || !buffer) return false;

    std::lock_guard<std::mutex> lock(map_mutex_);

    // Check if already registered
    if (va_to_iova_map_.find(buffer) != va_to_iova_map_.end()) {
        if (out_iova) *out_iova = va_to_iova_map_[buffer].iova;
        return true;
    }

    int remote_conn = idxd_client_get_remote_conn(ctx_);
    idxd_reg_result reg_res;
    memset(&reg_res, 0, sizeof(reg_res));

    if (idxd_register_buf(remote_conn, buffer, size, &reg_res) != 0) {
        std::cerr << "[QPL CXL] Failed to register buffer" << std::endl;
        return false;
    }

    va_to_iova_map_[buffer] = {reg_res.dma_addr, size};
    va_to_handle_map_[buffer] = reg_res.handle_id;

    if (out_iova) *out_iova = reg_res.dma_addr;
    return true;
#else
    (void)buffer; (void)size; (void)out_iova;
    return false;
#endif
}

bool CxlClient::register_completion_buffer(void* buffer, size_t size, uint64_t* out_iova) {
#ifdef ACC_POOL_PATH
    if (!initialized_ || !buffer) return false;

    std::lock_guard<std::mutex> lock(map_mutex_);

    if (va_to_iova_map_.find(buffer) != va_to_iova_map_.end()) {
        if (out_iova) *out_iova = va_to_iova_map_[buffer].iova;
        return true;
    }

    int remote_conn = idxd_client_get_remote_conn(ctx_);
    idxd_reg_result reg_res;
    memset(&reg_res, 0, sizeof(reg_res));

    if (idxd_register_completion_buf(remote_conn, buffer, size, &reg_res) != 0) {
        std::cerr << "[QPL CXL] Failed to register completion buffer" << std::endl;
        return false;
    }

    comp_handle_id_ = reg_res.handle_id;
    comp_pin_handle_id_ = reg_res.pin_handle_id;

    va_to_iova_map_[buffer] = {reg_res.dma_addr, size};
    va_to_handle_map_[buffer] = reg_res.handle_id;

    if (out_iova) *out_iova = reg_res.dma_addr;
    return true;
#else
    (void)buffer; (void)size; (void)out_iova;
    return false;
#endif
}

bool CxlClient::setup_cxl_proxy() {
#ifdef ACC_POOL_PATH
    if (!initialized_ || cxl_proxy_setup_done_) return true;
    if (comp_handle_id_ == 0) {
        std::cerr << "[QPL CXL] Cannot setup CXL proxy without a registered completion buffer" << std::endl;
        return false;
    }
    
    if (idxd_client_setup_proxy(ctx_, comp_pin_handle_id_, comp_handle_id_) != 0) {
        std::cerr << "[QPL CXL] Failed to setup CXL proxy" << std::endl;
        return false;
    }
    cxl_proxy_setup_done_ = true;
    return true;
#else
    return false;
#endif
}

bool CxlClient::setup_cpu_proxy() {
#ifdef ACC_POOL_PATH
    if (!initialized_ || cpu_proxy_setup_done_) return true;
    if (comp_handle_id_ == 0) {
        std::cerr << "[QPL CXL] Cannot setup CPU proxy without a registered completion buffer" << std::endl;
        return false;
    }

    if (idxd_client_attach_cpud(ctx_) != 0) {
        std::cerr << "[QPL CXL] Failed to attach cpud" << std::endl;
        return false;
    }

    int remote_conn = idxd_client_get_remote_conn(ctx_);
    uint64_t remote_handle = idxd_client_get_proxy_remote_handle(ctx_);
    
    if (idxd_remote_cpu_proxy_setup(remote_conn, remote_handle, comp_handle_id_) != 0) {
        std::cerr << "[QPL CXL] Failed to setup remote CPU proxy" << std::endl;
        return false;
    }
    cpu_proxy_setup_done_ = true;
    return true;
#else
    return false;
#endif
}

void* CxlClient::map_local_portal(int idxd_id, int wq_id) {
#ifdef ACC_POOL_PATH
    if (!initialized_) return nullptr;
    if (!local_portal_) {
        std::cout << "[QPL CXL] Mapping local portal for idxd=" << idxd_id << ", wq=" << wq_id << std::endl;
        local_portal_ = idxd_client_map_local_portal(ctx_, idxd_id, wq_id);
        if (!local_portal_) {
            std::cerr << "[QPL CXL] Failed to map local portal" << std::endl;
        } else {
            std::cout << "[QPL CXL] Local portal mapped at " << local_portal_ << std::endl;
        }
    }
    return local_portal_;
#else
    (void)idxd_id; (void)wq_id;
    return nullptr;
#endif
}

int CxlClient::submit_to_cpud(void* desc) {
#ifdef ACC_POOL_PATH
    if (!initialized_ || !desc) return -1;
    return idxd_client_submit_cpud(ctx_, static_cast<const iax_hw_desc*>(desc));
#else
    (void)desc;
    return -1;
#endif
}

struct conn_private_data {
    uint64_t portal_addr;
    uint32_t portal_rkey;
    uint64_t comp_addr;
    uint32_t comp_rkey;
    uint64_t comp_dma_addr;
};

bool CxlClient::setup_rdma_proxy(int rdma_port) {
#ifdef ACC_POOL_PATH
    if (rdma_proxy_setup_done_) return true;

    if (!initialized_ || !comp_handle_id_) {
        std::cerr << "[QPL CXL] Cannot setup RDMA proxy: client not initialized or completion buffer not registered." << std::endl;
        return false;
    }

    if (idxd_client_setup_proxy(ctx_, comp_pin_handle_id_, comp_handle_id_) != 0) {
        std::cerr << "[QPL CXL] Failed to setup CXL proxy for RDMA" << std::endl;
        return false;
    }

    // Allocate bounce buffers
    size_t desc_buf_size = max_rdma_slots_ * 64;
    size_t comp_buf_size = 4096;
    if (posix_memalign(&rdma_desc_buf_, 64, desc_buf_size)) return false;
    if (posix_memalign(&rdma_comp_buf_, 4096, comp_buf_size)) return false;
    memset(rdma_desc_buf_, 0, desc_buf_size);
    memset(rdma_comp_buf_, 0, comp_buf_size);

    ec_ = rdma_create_event_channel();
    if (!ec_) return false;
    
    if (rdma_create_id(ec_, &id_, NULL, RDMA_PS_TCP)) return false;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(rdma_port);
    inet_pton(AF_INET, idxd_client_get_server_ip(ctx_), &addr.sin_addr);

    if (rdma_resolve_addr(id_, NULL, (struct sockaddr *)&addr, 2000)) return false;

    struct rdma_cm_event* event = nullptr;
    if (rdma_get_cm_event(ec_, &event)) return false;
    rdma_ack_cm_event(event);

    if (rdma_resolve_route(id_, 2000)) return false;
    if (rdma_get_cm_event(ec_, &event)) return false;
    rdma_ack_cm_event(event);

    pd_ = ibv_alloc_pd(id_->verbs);
    if (!pd_) return false;

    mr_desc_ = ibv_reg_mr(pd_, rdma_desc_buf_, desc_buf_size, IBV_ACCESS_LOCAL_WRITE);
    mr_comp_ = ibv_reg_mr(pd_, rdma_comp_buf_, comp_buf_size, IBV_ACCESS_LOCAL_WRITE);
    if (!mr_desc_ || !mr_comp_) return false;

    struct ibv_qp_init_attr qp_attr;
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.cap.max_send_wr = max_rdma_slots_ * 2 + 10;
    qp_attr.cap.max_recv_wr = 10;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;
    qp_attr.cap.max_inline_data = 64;
    qp_attr.qp_type = IBV_QPT_RC;
    
    if (rdma_create_qp(id_, pd_, &qp_attr)) return false;

    struct rdma_conn_param cm_params;
    memset(&cm_params, 0, sizeof(cm_params));
    cm_params.initiator_depth = 1;
    cm_params.responder_resources = 1;
    cm_params.retry_count = 7;
    
    if (rdma_connect(id_, &cm_params)) return false;

    if (rdma_get_cm_event(ec_, &event)) return false;
    if (event->event != RDMA_CM_EVENT_ESTABLISHED) {
        rdma_ack_cm_event(event);
        return false;
    }

    struct conn_private_data pdata;
    memset(&pdata, 0, sizeof(pdata));
    memcpy(&pdata, event->param.conn.private_data, sizeof(pdata));
    rdma_ack_cm_event(event);

    remote_portal_addr_ = pdata.portal_addr;
    remote_portal_rkey_ = pdata.portal_rkey;
    remote_comp_addr_     = pdata.comp_addr;
    remote_comp_rkey_     = pdata.comp_rkey;

    rdma_proxy_setup_done_ = true;
    return true;
#else
    (void)rdma_port;
    return false;
#endif
}

uint32_t CxlClient::get_next_rdma_slot() {
    uint32_t slot = current_rdma_slot_;
    current_rdma_slot_ = (current_rdma_slot_ + 1) % max_rdma_slots_;
    return slot;
}

int CxlClient::rdma_write_descriptor(void* desc) {
#ifdef ACC_POOL_PATH
    if (!rdma_proxy_setup_done_) return -1;
    
    uint32_t slot = get_next_rdma_slot();
    void* local_desc = (uint8_t*)rdma_desc_buf_ + (slot * 64);
    memcpy(local_desc, desc, 64);

    struct ibv_sge sge_desc;
    std::memset(&sge_desc, 0, sizeof(sge_desc));
    sge_desc.addr = (uint64_t)local_desc;
    sge_desc.length = 64;
    sge_desc.lkey = mr_desc_->lkey;

    struct ibv_send_wr wr_write;
    memset(&wr_write, 0, sizeof(wr_write));
    wr_write.wr_id = slot;
    wr_write.opcode = IBV_WR_RDMA_WRITE;
    wr_write.sg_list = &sge_desc;
    wr_write.num_sge = 1;
    wr_write.send_flags = IBV_SEND_SIGNALED;
    wr_write.wr.rdma.remote_addr = remote_portal_addr_;
    wr_write.wr.rdma.rkey = remote_portal_rkey_;
    
    struct ibv_send_wr* bad_wr;
    if (ibv_post_send(id_->qp, &wr_write, &bad_wr)) {
        std::cerr << "[QPL CXL] RDMA WRITE post_send failed" << std::endl;
        return -1;
    }

    struct ibv_wc wc;
    int poll_count = 0;
    while (ibv_poll_cq(id_->qp->send_cq, 1, &wc) == 0) {
        _mm_pause();
        if (++poll_count > 10000000) {
            std::cerr << "[QPL CXL] RDMA WRITE completion timeout" << std::endl;
            return -1;
        }
    }
    
    if (wc.status != IBV_WC_SUCCESS) {
        std::cerr << "[QPL CXL] RDMA WRITE failed with status " << wc.status << std::endl;
        return -1;
    }

    return slot;
#else
    (void)desc;
    return -1;
#endif
}

int CxlClient::rdma_clear_completion(uint32_t slot) {
#ifdef ACC_POOL_PATH
    if (!rdma_proxy_setup_done_) return -1;

    // Use inline write to send zeros from the stack
    uint8_t zeros[64] = {0};
    struct ibv_sge sge;
    std::memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)zeros;
    sge.length = 64;
    sge.lkey = 0; // Not needed for inline

    struct ibv_send_wr wr_write;
    std::memset(&wr_write, 0, sizeof(wr_write));
    wr_write.wr_id = slot + 2000;
    wr_write.opcode = IBV_WR_RDMA_WRITE;
    wr_write.sg_list = &sge;
    wr_write.num_sge = 1;
    wr_write.send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE;
    wr_write.wr.rdma.remote_addr = remote_comp_addr_ + (slot * 64);
    wr_write.wr.rdma.rkey = remote_comp_rkey_;

    struct ibv_send_wr* bad_wr;
    if (ibv_post_send(id_->qp, &wr_write, &bad_wr)) {
        return -1;
    }

    struct ibv_wc wc;
    while (ibv_poll_cq(id_->qp->send_cq, 1, &wc) == 0) {
        _mm_pause();
    }
    
    return (wc.status == IBV_WC_SUCCESS) ? 0 : -1;
#else
    (void)slot;
    return -1;
#endif
}

int CxlClient::rdma_read_completion(uint32_t slot, void* comp_ptr) {
#ifdef ACC_POOL_PATH
    if (!rdma_proxy_setup_done_ || !comp_ptr) return -1;
    
    void* local_comp = (uint8_t*)rdma_comp_buf_ + (slot * 64);

    struct ibv_sge sge_comp;
    std::memset(&sge_comp, 0, sizeof(sge_comp));
    sge_comp.addr = (uint64_t)local_comp;
    sge_comp.length = 64;
    sge_comp.lkey = mr_comp_->lkey;

    struct ibv_send_wr wr_read;
    memset(&wr_read, 0, sizeof(wr_read));
    wr_read.wr_id = slot + 1000;
    wr_read.opcode = IBV_WR_RDMA_READ;
    wr_read.sg_list = &sge_comp;
    wr_read.num_sge = 1;
    wr_read.send_flags = IBV_SEND_SIGNALED;
    wr_read.wr.rdma.remote_addr = remote_comp_addr_ + (slot * 64);
    wr_read.wr.rdma.rkey = remote_comp_rkey_;
    
    struct ibv_send_wr* bad_wr;
    if (ibv_post_send(id_->qp, &wr_read, &bad_wr)) {
        std::cerr << "[QPL CXL] RDMA READ post_send failed" << std::endl;
        return -1;
    }

    struct ibv_wc wc;
    int poll_count = 0;
    while (ibv_poll_cq(id_->qp->send_cq, 1, &wc) == 0) {
        _mm_pause();
        if (++poll_count > 10000000) {
            std::cerr << "[QPL CXL] RDMA READ completion timeout" << std::endl;
            return -1;
        }
    }
    
    if (wc.status != IBV_WC_SUCCESS) {
        std::cerr << "[QPL CXL] RDMA READ failed with status " << wc.status << std::endl;
        return -1;
    }

    // Copy completion to user buffer
    memcpy(comp_ptr, local_comp, 64);

    return 0;
#else
    (void)slot; (void)comp_ptr;
    return -1;
#endif
}

bool CxlClient::deregister_buffer(void* buffer) {
#ifdef ACC_POOL_PATH
    if (!initialized_ || !buffer) return false;

    std::lock_guard<std::mutex> lock(map_mutex_);
    
    auto it = va_to_handle_map_.find(buffer);
    if (it == va_to_handle_map_.end()) {
        return false;
    }

    int remote_conn = idxd_client_get_remote_conn(ctx_);
    idxd_deregister_buf(remote_conn, it->second);

    va_to_handle_map_.erase(it);
    va_to_iova_map_.erase(buffer);

    return true;
#else
    (void)buffer;
    return false;
#endif
}

uint64_t CxlClient::get_iova(void* buffer) {
    if (!buffer) return 0;
    std::lock_guard<std::mutex> lock(map_mutex_);
    auto it = va_to_iova_map_.find(buffer);
    if (it != va_to_iova_map_.end()) {
        return it->second.iova;
    }
    uintptr_t target = reinterpret_cast<uintptr_t>(buffer);
    for (const auto& pair : va_to_iova_map_) {
        uintptr_t base = reinterpret_cast<uintptr_t>(pair.first);
        size_t size = pair.second.size;
        if (target >= base && target < base + size) {
            return pair.second.iova + (target - base);
        }
    }
    return 0;
}

void* CxlClient::get_proxy_portal() const {
#ifdef ACC_POOL_PATH
    if (!initialized_) return nullptr;
    return idxd_client_get_proxy_portal(ctx_);
#else
    return nullptr;
#endif
}

int CxlClient::get_remote_conn() const {
#ifdef ACC_POOL_PATH
    if (!initialized_) return -1;
    return idxd_client_get_remote_conn(ctx_);
#else
    return -1;
#endif
}

int CxlClient::get_comp_slot() {
    std::lock_guard<std::mutex> lock(comp_mutex_);
    for (int i = 0; i < 64; ++i) {
        if (!(comp_slots_mask_ & (1ULL << i))) {
            comp_slots_mask_ |= (1ULL << i);
            // Clear the slot
            void* ptr = get_comp_ptr(i);
            if (ptr) memset(ptr, 0, 64);
            return i;
        }
    }
    return -1;
}

void CxlClient::release_comp_slot(int slot) {
    if (slot < 0 || slot >= 64) return;
    std::lock_guard<std::mutex> lock(comp_mutex_);
    comp_slots_mask_ &= ~(1ULL << slot);
}

uint64_t CxlClient::get_comp_iova(int slot) {
    if (slot < 0 || slot >= 64) return 0;
    return comp_page_iova_ + (slot * 64);
}

void* CxlClient::get_comp_ptr(int slot) {
    if (slot < 0 || slot >= 64 || !comp_page_) return nullptr;
    return (uint8_t*)comp_page_ + (slot * 64);
}

} // namespace qpl::ml::dispatcher
