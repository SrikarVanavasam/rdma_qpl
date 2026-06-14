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
    if (remote_ctx_ || local_ctx_) {
        std::lock_guard<std::mutex> lock(map_mutex_);
        // Deregister all mapped buffers
        for (const auto& pair : va_to_handle_map_) {
            if (remote_ctx_) idxd_deregister_buf(idxd_client_get_remote_conn(remote_ctx_), pair.second.remote_handle);
            if (local_ctx_ && local_ctx_ != remote_ctx_)  
                idxd_deregister_buf(idxd_client_get_remote_conn(local_ctx_), pair.second.local_handle);
        }
        
        if (remote_comp_page_) {
            if (remote_ctx_) idxd_deregister_buf(idxd_client_get_remote_conn(remote_ctx_), remote_comp_handle_id_);
            numa_free(remote_comp_page_, 4096);
        }
        if (local_comp_page_ && local_comp_page_ != remote_comp_page_) {
            if (local_ctx_)  idxd_deregister_buf(idxd_client_get_remote_conn(local_ctx_), local_comp_handle_id_);
            numa_free(local_comp_page_, 4096);
        }

        if (remote_ctx_) idxd_client_deinit(remote_ctx_);
        if (local_ctx_ && local_ctx_ != remote_ctx_)  idxd_client_deinit(local_ctx_);
        
        remote_ctx_ = nullptr;
        local_ctx_  = nullptr;
    }
#endif
}

bool CxlClient::initialize(const char* server_ip_in, const char* bdf, int numa_node) {
#ifdef ACC_POOL_PATH
    if (initialized_) return true;

    std::string server_ip = server_ip_in;
    bool is_pure_local = (server_ip == "127.0.0.1");
    bool is_combined   = (server_ip.find("combined:") == 0);

    if (is_combined) {
        server_ip = server_ip.substr(9); // Strip "combined:"
    }

    // --- Initialize LOCAL context ---
    if (is_pure_local || is_combined) {
        idxd_client_init_params local_params = {};
        local_params.server_ip = "127.0.0.1";
        local_params.port = IDXD_REG_DEFAULT_PORT;
        local_params.bdf = bdf;
        local_params.numa_node = numa_node;

        // std::cout << "[QPL CXL] Initializing LOCAL context with server_ip=127.0.0.1" << std::endl;
        int rc = idxd_client_init(&local_params, &local_ctx_);
        if (rc != 0) {
            std::cerr << "[QPL CXL] Failed to initialize LOCAL context, rc=" << rc << std::endl;
            if (is_pure_local || is_combined) return false;
        }
    }

    if (is_pure_local) {
        remote_ctx_ = local_ctx_;
    } else {
        // --- Initialize REMOTE context ---
        idxd_client_init_params remote_params = {};
        remote_params.server_ip = server_ip.c_str();
        remote_params.port = IDXD_REG_DEFAULT_PORT;
        remote_params.bdf = bdf;
        remote_params.numa_node = numa_node;

        // std::cout << "[QPL CXL] Initializing REMOTE context with server_ip=" << server_ip << std::endl;
        int rc = idxd_client_init(&remote_params, &remote_ctx_);
        if (rc != 0) {
            std::cerr << "[QPL CXL] Failed to initialize REMOTE context, rc=" << rc << std::endl;
            return false;
        }
    }

    // --- Allocate and register library-managed completion pages ---
    
    // Local page
    if (local_ctx_) {
        local_comp_page_ = numa_alloc_onnode(4096, numa_node);
        if (local_comp_page_) {
            memset(local_comp_page_, 0, 4096);
            idxd_reg_result reg_res;
            memset(&reg_res, 0, sizeof(reg_res));
            int rc = idxd_register_completion_buf(idxd_client_get_remote_conn(local_ctx_), local_comp_page_, 4096, &reg_res);
            if (rc == 0) {
                local_comp_handle_id_ = reg_res.handle_id;
                local_comp_pin_handle_id_ = reg_res.pin_handle_id;
                comp_page_local_iova_ = reg_res.dma_addr;
            } else {
                std::cerr << "[QPL CXL] Failed to register LOCAL completion buf, rc=" << rc << std::endl;
            }
        } else {
            std::cerr << "[QPL CXL] Failed to allocate LOCAL completion page" << std::endl;
        }
    }

    // Remote page (deduplicate if pure local)
    if (is_pure_local) {
        remote_comp_page_ = local_comp_page_;
        remote_comp_handle_id_ = local_comp_handle_id_;
        remote_comp_pin_handle_id_ = local_comp_pin_handle_id_;
        comp_page_remote_iova_ = comp_page_local_iova_;
    } else if (remote_ctx_) {
        remote_comp_page_ = numa_alloc_onnode(4096, numa_node);
        if (remote_comp_page_) {
            memset(remote_comp_page_, 0, 4096);
            idxd_reg_result reg_res;
            memset(&reg_res, 0, sizeof(reg_res));
            if (idxd_register_completion_buf(idxd_client_get_remote_conn(remote_ctx_), remote_comp_page_, 4096, &reg_res) == 0) {
                remote_comp_handle_id_ = reg_res.handle_id;
                remote_comp_pin_handle_id_ = reg_res.pin_handle_id;
                comp_page_remote_iova_ = reg_res.dma_addr;
                // printf("[QPL CXL] Registered REMOTE completion page: iova=0x%lx, handle=%d\n", comp_page_remote_iova_, remote_comp_handle_id_);
            }
        }
    }

    initialized_ = true;
    return true;
#else
    (void)server_ip_in; (void)bdf; (void)numa_node;
    return false;
#endif
}

bool CxlClient::register_buffer(void* buffer, size_t size, uint64_t* out_iova) {
#ifdef ACC_POOL_PATH
    if (!initialized_ || !buffer) return false;

    std::lock_guard<std::mutex> lock(map_mutex_);

    // Check if the requested buffer range falls within any already registered buffer range
    uintptr_t target = reinterpret_cast<uintptr_t>(buffer);
    for (const auto& pair : va_to_iova_map_) {
        uintptr_t base = reinterpret_cast<uintptr_t>(pair.first);
        size_t rsize = pair.second.size;
        if (target >= base && (target + size) <= (base + rsize)) {
            if (out_iova) {
                *out_iova = pair.second.remote_iova + (target - base);
            }
            return true;
        }
    }

    if (va_to_iova_map_.find(buffer) != va_to_iova_map_.end()) {
        RegisteredBuffer& rb = va_to_iova_map_[buffer];
        RegisteredHandles& rh = va_to_handle_map_[buffer];
        idxd_reg_result reg_res;
        
        if (local_ctx_ && rb.local_iova == 0) {
            memset(&reg_res, 0, sizeof(reg_res));
            if (idxd_register_buf(idxd_client_get_remote_conn(local_ctx_), buffer, size, &reg_res) == 0) {
                rb.local_iova = reg_res.dma_addr;
                rh.local_handle = reg_res.handle_id;
            }
        }
        if (remote_ctx_ && rb.remote_iova == 0) {
            if (remote_ctx_ == local_ctx_ && rb.local_iova != 0) {
                rb.remote_iova = rb.local_iova;
                rh.remote_handle = rh.local_handle;
            } else {
                memset(&reg_res, 0, sizeof(reg_res));
                if (idxd_register_buf(idxd_client_get_remote_conn(remote_ctx_), buffer, size, &reg_res) == 0) {
                    rb.remote_iova = reg_res.dma_addr;
                    rh.remote_handle = reg_res.handle_id;
                }
            }
        }
        if (out_iova) *out_iova = rb.remote_iova;
        return true;
    }

    RegisteredBuffer rb = {0, 0, size};
    RegisteredHandles rh = {0, 0};

    // Register with LOCAL first
    idxd_reg_result reg_res;
    if (local_ctx_) {
        memset(&reg_res, 0, sizeof(reg_res));
        if (idxd_register_buf(idxd_client_get_remote_conn(local_ctx_), buffer, size, &reg_res) == 0) {
            rb.local_iova = reg_res.dma_addr;
            rh.local_handle = reg_res.handle_id;
            // printf("[QPL CXL] Registered LOCAL buf %p: iova=0x%lx, handle=%d\n", buffer, rb.local_iova, (int)rh.local_handle);
        } else {
            // std::cerr << "[QPL CXL] Failed to register LOCAL buf " << buffer << std::endl;
        }
    }

    // Handle REMOTE registration
    if (remote_ctx_) {
        if (remote_ctx_ == local_ctx_) {
            rb.remote_iova = rb.local_iova;
            rh.remote_handle = rh.local_handle;
        } else {
            memset(&reg_res, 0, sizeof(reg_res));
            if (idxd_register_buf(idxd_client_get_remote_conn(remote_ctx_), buffer, size, &reg_res) == 0) {
                rb.remote_iova = reg_res.dma_addr;
                rh.remote_handle = reg_res.handle_id;
                // printf("[QPL CXL] Registered REMOTE buf %p: iova=0x%lx, handle=%d\n", buffer, rb.remote_iova, (int)rh.remote_handle);
            } else {
                // std::cerr << "[QPL CXL] Failed to register REMOTE buf " << buffer << std::endl;
            }
        }
    }

    va_to_iova_map_[buffer] = rb;
    va_to_handle_map_[buffer] = rh;

    if (out_iova) *out_iova = rb.remote_iova;
    return true;
#else
    (void)buffer;
    (void)size;
    (void)out_iova;
    return false;
#endif
}

bool CxlClient::register_completion_buffer(void* buffer, size_t size, uint64_t* out_iova) {
#ifdef ACC_POOL_PATH
    if (!initialized_ || !buffer) return false;

    std::lock_guard<std::mutex> lock(map_mutex_);

    if (va_to_iova_map_.find(buffer) != va_to_iova_map_.end()) {
        if (out_iova) *out_iova = va_to_iova_map_[buffer].remote_iova;
        return true;
    }

    RegisteredBuffer rb = {0, 0, size};
    RegisteredHandles rh = {0, 0};

    // Register with LOCAL first
    idxd_reg_result reg_res;
    if (local_ctx_) {
        memset(&reg_res, 0, sizeof(reg_res));
        if (idxd_register_completion_buf(idxd_client_get_remote_conn(local_ctx_), buffer, size, &reg_res) == 0) {
            rb.local_iova = reg_res.dma_addr;
            rh.local_handle = reg_res.handle_id;
            local_comp_handle_id_ = reg_res.handle_id;
            local_comp_pin_handle_id_ = reg_res.pin_handle_id;
        }
    }

    // Handle REMOTE registration (deduplicate if identical to local)
    if (remote_ctx_ == local_ctx_) {
        rb.remote_iova = rb.local_iova;
        rh.remote_handle = rh.local_handle;
        remote_comp_handle_id_ = local_comp_handle_id_;
        remote_comp_pin_handle_id_ = local_comp_pin_handle_id_;
    } else if (remote_ctx_) {
        memset(&reg_res, 0, sizeof(reg_res));
        if (idxd_register_completion_buf(idxd_client_get_remote_conn(remote_ctx_), buffer, size, &reg_res) == 0) {
            rb.remote_iova = reg_res.dma_addr;
            rh.remote_handle = reg_res.handle_id;
            remote_comp_handle_id_ = reg_res.handle_id;
            remote_comp_pin_handle_id_ = reg_res.pin_handle_id;
        }
    }

    va_to_iova_map_[buffer] = rb;
    va_to_handle_map_[buffer] = rh;

    if (out_iova) *out_iova = rb.remote_iova;
    return true;
#else
    (void)buffer;
    (void)size;
    (void)out_iova;
    return false;
#endif
}

bool CxlClient::setup_cxl_proxy(bool remote) {
#ifdef ACC_POOL_PATH
    if (!initialized_) return false;
    
    if (remote) {
        if (cxl_remote_setup_done_) return true;
        if (!remote_ctx_ || remote_comp_handle_id_ == 0) return false;
        if (idxd_client_setup_proxy(remote_ctx_, remote_comp_pin_handle_id_, remote_comp_handle_id_) != 0) {
            return false;
        }
        cxl_remote_setup_done_ = true;
    } else {
        if (cxl_local_setup_done_) return true;
        static bool printed_local_setup_fail = false;
        if (!local_ctx_) {
            if (!printed_local_setup_fail) { std::cerr << "[QPL CXL] setup_cxl_proxy: local_ctx_ is null" << std::endl; printed_local_setup_fail = true; }
            return false;
        }
        if (local_comp_handle_id_ == 0) {
            if (!printed_local_setup_fail) { std::cerr << "[QPL CXL] setup_cxl_proxy: local_comp_handle_id_ is 0" << std::endl; printed_local_setup_fail = true; }
            return false;
        }
        int rc = idxd_client_setup_proxy(local_ctx_, local_comp_pin_handle_id_, local_comp_handle_id_);
        if (rc != 0) {
            if (!printed_local_setup_fail) { std::cerr << "[QPL CXL] setup_cxl_proxy: idxd_client_setup_proxy failed, rc=" << rc << std::endl; printed_local_setup_fail = true; }
            return false;
        }
        cxl_local_setup_done_ = true;
    }
    return true;
#else
    (void)remote;
    return false;
#endif
}

bool CxlClient::setup_cpu_proxy() {
#ifdef ACC_POOL_PATH
    if (!initialized_ || cpu_proxy_setup_done_) return true;
    if (remote_comp_handle_id_ == 0) return false;

    if (idxd_client_attach_cpud(remote_ctx_) != 0) {
        return false;
    }

    int remote_conn = idxd_client_get_remote_conn(remote_ctx_);
    uint64_t remote_handle = idxd_client_get_proxy_remote_handle(remote_ctx_);
    
    if (idxd_remote_cpu_proxy_setup(remote_conn, remote_handle, remote_comp_handle_id_) != 0) {
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
    if (!initialized_ || !local_ctx_) return nullptr;
    if (!local_portal_) {
        local_portal_ = idxd_client_map_local_portal(local_ctx_, idxd_id, wq_id);
    }
    return local_portal_;
#else
    (void)idxd_id;
    (void)wq_id;
    return nullptr;
#endif
}

int CxlClient::submit_to_cpud(void* desc) {
#ifdef ACC_POOL_PATH
    if (!initialized_ || !desc || !remote_ctx_) return -1;
    return idxd_client_submit_cpud(remote_ctx_, static_cast<const iax_hw_desc*>(desc));
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

    if (!initialized_ || !remote_comp_handle_id_) {
        std::cerr << "[QPL CXL] Cannot setup RDMA proxy: client not initialized or completion buffer not registered." << std::endl;
        return false;
    }

    if (idxd_client_setup_proxy(remote_ctx_, remote_comp_pin_handle_id_, remote_comp_handle_id_) != 0) {
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
    inet_pton(AF_INET, idxd_client_get_server_ip(remote_ctx_), &addr.sin_addr);

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
    uint32_t normalized_slot = slot % 64;
    void* local_desc = (uint8_t*)rdma_desc_buf_ + (normalized_slot * 64);
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
    wr_write.wr.rdma.remote_addr = remote_comp_addr_ + ((slot % 64) * 64);
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
    
    uint32_t normalized_slot = slot % 64;
    void* local_comp = (uint8_t*)rdma_comp_buf_ + (normalized_slot * 64);

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
    wr_read.wr.rdma.remote_addr = remote_comp_addr_ + (normalized_slot * 64);
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

    if (remote_ctx_) idxd_deregister_buf(idxd_client_get_remote_conn(remote_ctx_), it->second.remote_handle);
    if (local_ctx_ && local_ctx_ != remote_ctx_)  idxd_deregister_buf(idxd_client_get_remote_conn(local_ctx_), it->second.local_handle);

    va_to_handle_map_.erase(it);
    va_to_iova_map_.erase(buffer);

    return true;
#else
    (void)buffer;
    return false;
#endif
}

uint64_t CxlClient::get_remote_iova(void* buffer) {
    if (!buffer) return 0;
    std::lock_guard<std::mutex> lock(map_mutex_);
    auto it = va_to_iova_map_.find(buffer);
    if (it != va_to_iova_map_.end()) {
        return it->second.remote_iova;
    }
    uintptr_t target = reinterpret_cast<uintptr_t>(buffer);
    for (const auto& pair : va_to_iova_map_) {
        uintptr_t base = reinterpret_cast<uintptr_t>(pair.first);
        size_t size = pair.second.size;
        if (target >= base && target < base + size) {
            return pair.second.remote_iova + (target - base);
        }
    }
    return 0;
}

uint64_t CxlClient::get_local_iova(void* buffer) {
    if (!buffer) return 0;
    std::lock_guard<std::mutex> lock(map_mutex_);
    auto it = va_to_iova_map_.find(buffer);
    if (it != va_to_iova_map_.end()) {
        return it->second.local_iova;
    }
    uintptr_t target = reinterpret_cast<uintptr_t>(buffer);
    for (const auto& pair : va_to_iova_map_) {
        uintptr_t base = reinterpret_cast<uintptr_t>(pair.first);
        size_t size = pair.second.size;
        if (target >= base && target < base + size) {
            return pair.second.local_iova + (target - base);
        }
    }
    return 0;
}

void* CxlClient::get_proxy_portal() const {
#ifdef ACC_POOL_PATH
    if (!initialized_ || !remote_ctx_) return nullptr;
    return idxd_client_get_proxy_portal(remote_ctx_);
#else
    return nullptr;
#endif
}

int CxlClient::get_remote_conn() const {
#ifdef ACC_POOL_PATH
    if (!initialized_ || !remote_ctx_) return -1;
    return idxd_client_get_remote_conn(remote_ctx_);
#else
    return -1;
#endif
}

int CxlClient::get_comp_slot(bool remote) {
    std::lock_guard<std::mutex> lock(comp_mutex_);
    bool use_split_range = (local_ctx_ != nullptr && remote_ctx_ != nullptr && local_ctx_ != remote_ctx_);

    if (remote && use_split_range) {
        for (int i = 0; i < 64; ++i) {
            if (!(remote_comp_slots_mask_ & (1ULL << i))) {
                remote_comp_slots_mask_ |= (1ULL << i);
                int slot = i + 64; // Remote slots: 64-127
                memset(get_comp_ptr(slot), 0, 64);
                return slot;
            }
        }
    } else {
        // Use 0-63 for local or for remote in non-split mode
        for (int i = 0; i < 64; ++i) {
            if (!(local_comp_slots_mask_ & (1ULL << i))) {
                local_comp_slots_mask_ |= (1ULL << i);
                int slot = i; // Slots: 0-63
                memset(get_comp_ptr(slot), 0, 64);
                return slot;
            }
        }
    }
    return -1;
}

void CxlClient::release_comp_slot(int slot) {
    if (slot < 0 || slot >= 128) return;
    std::lock_guard<std::mutex> lock(comp_mutex_);
    if (slot >= 64) {
        remote_comp_slots_mask_ &= ~(1ULL << (slot - 64));
    } else {
        local_comp_slots_mask_ &= ~(1ULL << slot);
    }
}

uint64_t CxlClient::get_comp_iova(int slot) {
    if (slot < 0 || slot >= 128) return 0;
    bool use_split_range = (local_ctx_ != nullptr && remote_ctx_ != nullptr && local_ctx_ != remote_ctx_);

    if (slot >= 64) {
        return comp_page_remote_iova_ + ((slot - 64) * 64);
    } else {
        // In non-split mode, slot 0-63 could be remote or local
        if (!use_split_range && remote_ctx_ && !local_ctx_) {
            return comp_page_remote_iova_ + (slot * 64);
        }
        return comp_page_local_iova_ + (slot * 64);
    }
}

void* CxlClient::get_comp_ptr(int slot) {
    if (slot < 0 || slot >= 128) return nullptr;
    bool use_split_range = (local_ctx_ != nullptr && remote_ctx_ != nullptr && local_ctx_ != remote_ctx_);

    void* ptr = nullptr;
    if (slot >= 64) {
        if (remote_comp_page_) ptr = (uint8_t*)remote_comp_page_ + ((slot - 64) * 64);
    } else {
        // In non-split mode, slot 0-63 could be remote or local
        if (!use_split_range && remote_comp_page_ && !local_comp_page_) {
            ptr = (uint8_t*)remote_comp_page_ + (slot * 64);
        } else if (local_comp_page_) {
            ptr = (uint8_t*)local_comp_page_ + (slot * 64);
        }
    }
    // static thread_local int count = 0;
    // if (count++ % 1000000 == 0) printf("[QPL CXL] get_comp_ptr slot=%d -> %p\n", slot, ptr);
    return ptr;
}

} // namespace qpl::ml::dispatcher
