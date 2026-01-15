#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <infiniband/verbs.h>
#include <iostream>
#include <rdma/rdma_cma.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "rdma_protocol.hpp"

using namespace qpl::rdma;

struct ServerContext {
    std::vector<std::string> wq_paths;  // Paths to WQ devices
    uint32_t                 num_wqs = 0;

    // Portals (one per WQ)
    void*          portal_bufs[MAX_WQS] = {};
    struct ibv_mr* mr_portals[MAX_WQS]  = {};

    // Data Pool
    void*          data_pool_buf  = nullptr;
    size_t         data_pool_size = 0;
    struct ibv_mr* mr_data_pool   = nullptr;

    // Completion Pool
    void*          comp_pool_buf  = nullptr;
    size_t         comp_pool_size = 0;
    struct ibv_mr* mr_comp_pool   = nullptr;

    // RDMA Resources
    struct rdma_cm_id* listen_id = nullptr;
    struct rdma_cm_id* cm_id     = nullptr; // Active connection
    struct ibv_pd*     pd        = nullptr;
};

static void die(const std::string& reason) {
    perror(reason.c_str());
    exit(EXIT_FAILURE);
}

static int on_connect_request(struct rdma_cm_id* id, ServerContext* ctx) {
    std::cout << "[Server] Received connection request." << std::endl;
    ctx->cm_id = id;

    // Zero completion and data pools to prevent stale state from previous connections
    std::memset(ctx->comp_pool_buf, 0, ctx->comp_pool_size);
    std::memset(ctx->data_pool_buf, 0, ctx->data_pool_size);
    std::cout << "[Server] Cleared completion and data pools for new connection." << std::endl;

    // 1. Allocate Protection Domain
    ctx->pd = ibv_alloc_pd(id->verbs);
    if (!ctx->pd) die("ibv_alloc_pd");

    // 2. Register Memory Regions
    // Portals: Local Write (to clear/reset?), Remote Write (Submission), Remote Read (Debugging?)
    for (uint32_t i = 0; i < ctx->num_wqs; i++) {
        ctx->mr_portals[i] = ibv_reg_mr(ctx->pd, ctx->portal_bufs[i], PORTAL_SIZE,
                                         IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
        if (!ctx->mr_portals[i]) die("ibv_reg_mr portal " + std::to_string(i));
    }

    // Data Pool: Local Write (Init), Remote Write (Input), Remote Read (Output)
    ctx->mr_data_pool = ibv_reg_mr(ctx->pd, ctx->data_pool_buf, ctx->data_pool_size,
                                   IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
    if (!ctx->mr_data_pool) die("ibv_reg_mr data pool");

    // Comp Pool: Local Write (Init), Remote Write (Completion from IAA), Remote Read (Polling)
    // IMPORTANT: The IAA hardware writes to this. Does it need REMOTE_WRITE?
    // Usually Hardware writes are "Local" from the device's perspective, but for RDMA access...
    // The *Client* will RDMA READ this.
    // The *IAA* (Device) writes to this via DMA.
    // So IBV_ACCESS_REMOTE_READ is essential.
    ctx->mr_comp_pool = ibv_reg_mr(ctx->pd, ctx->comp_pool_buf, ctx->comp_pool_size,
                                   IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
    if (!ctx->mr_comp_pool) die("ibv_reg_mr comp pool");

    // 3. Create Queue Pair
    struct ibv_qp_init_attr qp_attr = {};
    qp_attr.qp_context              = ctx;
    qp_attr.cap.max_send_wr         = 16;
    qp_attr.cap.max_recv_wr         = 16;
    qp_attr.cap.max_send_sge        = 1;
    qp_attr.cap.max_recv_sge        = 1;
    qp_attr.qp_type                 = IBV_QPT_RC;

    if (rdma_create_qp(id, ctx->pd, &qp_attr)) die("rdma_create_qp");

    // 4. Accept with Private Data
    struct rdma_conn_param cm_params = {};
    ConnPrivateData        pdata     = {};

    // Populate portal info for all WQs
    pdata.num_wqs = ctx->num_wqs;
    for (uint32_t i = 0; i < ctx->num_wqs; i++) {
        pdata.portal_addrs[i] = (uint64_t)ctx->portal_bufs[i];
        pdata.portal_rkeys[i] = ctx->mr_portals[i]->rkey;
    }

    pdata.data_pool_addr  = (uint64_t)ctx->data_pool_buf;
    pdata.data_pool_rkey  = ctx->mr_data_pool->rkey;
    pdata.data_pool_count = NUM_JOBS * 3; // 3 blocks per job

    pdata.comp_pool_addr  = (uint64_t)ctx->comp_pool_buf;
    pdata.comp_pool_rkey  = ctx->mr_comp_pool->rkey;
    pdata.comp_pool_count = NUM_JOBS;

    cm_params.private_data        = &pdata;
    cm_params.private_data_len    = sizeof(pdata);
    cm_params.responder_resources = 1;
    cm_params.initiator_depth     = 1;

    if (rdma_accept(id, &cm_params)) die("rdma_accept");

    std::cout << "[Server] Connection accepted. Setup complete." << std::endl;
    return 0;
}

static int on_disconnect(struct rdma_cm_id* id, ServerContext* ctx) {
    std::cout << "[Server] Client disconnected." << std::endl;

    rdma_destroy_qp(id);

    // Deregister portal MRs
    for (uint32_t i = 0; i < ctx->num_wqs; i++) {
        if (ctx->mr_portals[i]) {
            ibv_dereg_mr(ctx->mr_portals[i]);
            ctx->mr_portals[i] = nullptr;
        }
    }
    if (ctx->mr_data_pool) {
        ibv_dereg_mr(ctx->mr_data_pool);
        ctx->mr_data_pool = nullptr;
    }
    if (ctx->mr_comp_pool) {
        ibv_dereg_mr(ctx->mr_comp_pool);
        ctx->mr_comp_pool = nullptr;
    }

    if (ctx->pd) {
        ibv_dealloc_pd(ctx->pd);
        ctx->pd = nullptr;
    }

    ctx->cm_id = nullptr;
    return 0; // Keep listening
}

static int on_event(struct rdma_cm_event* event, ServerContext* ctx) {
    if (event->event == RDMA_CM_EVENT_CONNECT_REQUEST)
        return on_connect_request(event->id, ctx);
    else if (event->event == RDMA_CM_EVENT_DISCONNECTED)
        return on_disconnect(event->id, ctx);
    else
        std::cout << "[Server] Unknown event: " << rdma_event_str(event->event) << std::endl;
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <device_wq_path> [<device_wq_path>...]" << std::endl;
        std::cerr << "Example: " << argv[0] << " /dev/iax/wq0.0 /dev/iax/wq0.1" << std::endl;
        return 1;
    }

    ServerContext ctx;
    
    // Parse multiple WQ paths (limit to MAX_WQS)
    ctx.num_wqs = std::min(static_cast<uint32_t>(argc - 1), MAX_WQS);
    for (uint32_t i = 0; i < ctx.num_wqs; i++) {
        ctx.wq_paths.push_back(argv[i + 1]);
    }

    std::cout << "[Server] Configuring " << ctx.num_wqs << " WQ(s):" << std::endl;
    
    // 1. Map Portals for each WQ
    for (uint32_t i = 0; i < ctx.num_wqs; i++) {
        int fd = open(ctx.wq_paths[i].c_str(), O_RDWR);
        if (fd < 0) {
            std::cerr << "[Server] Failed to open WQ " << i << ": " << ctx.wq_paths[i] << std::endl;
            die("open device");
        }

        ctx.portal_bufs[i] = mmap(NULL, PORTAL_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (ctx.portal_bufs[i] == MAP_FAILED) {
            std::cerr << "[Server] Failed to mmap portal " << i << std::endl;
            die("mmap portal");
        }
        close(fd); // Can close fd after mmap
        
        std::cout << "  WQ " << i << ": " << ctx.wq_paths[i] << " -> " << ctx.portal_bufs[i] << std::endl;
    }

    // 2. Allocate Buffers
    // Data Pool: 3 blocks per job * NUM_JOBS * BLOCK_SIZE
    ctx.data_pool_size = NUM_JOBS * 3 * BLOCK_SIZE;
    
    // Use huge pages (2MB) for better TLB performance and pre-faulted memory
    ctx.data_pool_buf = mmap(nullptr, ctx.data_pool_size,
                              PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_POPULATE,
                              -1, 0);
    
    if (ctx.data_pool_buf == MAP_FAILED) {
        std::cerr << "[Server] Huge pages unavailable for data pool, falling back to regular pages" << std::endl;
        ctx.data_pool_buf = mmap(nullptr, ctx.data_pool_size,
                                  PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                                  -1, 0);
        if (ctx.data_pool_buf == MAP_FAILED) die("mmap data pool");
    } else {
        std::cout << "[Server] Using huge pages for data pool (" << (ctx.data_pool_size / (1024*1024)) << " MB)" << std::endl;
    }
    
    // Pin in memory to prevent swapping and ensure consistent performance
    if (mlock(ctx.data_pool_buf, ctx.data_pool_size) != 0) {
        std::cerr << "[Server] Warning: mlock failed for data pool: " << strerror(errno) << std::endl;
    }
    std::memset(ctx.data_pool_buf, 0, ctx.data_pool_size);

    // Comp Pool: NUM_JOBS * COMP_SIZE (64B)
    ctx.comp_pool_size = NUM_JOBS * COMP_SIZE;
    
    // Completion pool is smaller, use huge pages with fallback
    ctx.comp_pool_buf = mmap(nullptr, ctx.comp_pool_size,
                              PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_POPULATE,
                              -1, 0);
    
    if (ctx.comp_pool_buf == MAP_FAILED) {
        std::cerr << "[Server] Huge pages unavailable for comp pool, falling back to regular pages" << std::endl;
        ctx.comp_pool_buf = mmap(nullptr, ctx.comp_pool_size,
                                  PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                                  -1, 0);
        if (ctx.comp_pool_buf == MAP_FAILED) die("mmap comp pool");
    } else {
        std::cout << "[Server] Using huge pages for comp pool (" << (ctx.comp_pool_size / 1024) << " KB)" << std::endl;
    }
    
    // Pin completion pool in memory
    if (mlock(ctx.comp_pool_buf, ctx.comp_pool_size) != 0) {
        std::cerr << "[Server] Warning: mlock failed for comp pool: " << strerror(errno) << std::endl;
    }
    std::memset(ctx.comp_pool_buf, 0, ctx.comp_pool_size);

    // 3. Setup RDMA Listener
    struct rdma_event_channel* ec = rdma_create_event_channel();
    if (!ec) die("rdma_create_event_channel");

    if (rdma_create_id(ec, &ctx.listen_id, NULL, RDMA_PS_TCP)) die("rdma_create_id");

    struct sockaddr_in addr = {};
    addr.sin_family         = AF_INET;
    addr.sin_port           = htons(SERVER_PORT);
    addr.sin_addr.s_addr    = INADDR_ANY;

    if (rdma_bind_addr(ctx.listen_id, (struct sockaddr*)&addr)) die("rdma_bind_addr");
    if (rdma_listen(ctx.listen_id, 1)) die("rdma_listen");

    std::cout << "[Server] Listening on port " << SERVER_PORT << "..." << std::endl;

    struct rdma_cm_event* event = nullptr;
    while (rdma_get_cm_event(ec, &event) == 0) {
        struct rdma_cm_event event_copy = *event;
        rdma_ack_cm_event(event);
        if (on_event(&event_copy, &ctx)) {
            // Handle error or break if needed, for now just log
        }
    }

    // Cleanup
    if (ctx.data_pool_buf) {
        munlock(ctx.data_pool_buf, ctx.data_pool_size);
        munmap(ctx.data_pool_buf, ctx.data_pool_size);
    }
    if (ctx.comp_pool_buf) {
        munlock(ctx.comp_pool_buf, ctx.comp_pool_size);
        munmap(ctx.comp_pool_buf, ctx.comp_pool_size);
    }
    // Unmap all portals
    for (uint32_t i = 0; i < ctx.num_wqs; i++) {
        if (ctx.portal_bufs[i]) munmap(ctx.portal_bufs[i], PORTAL_SIZE);
    }

    rdma_destroy_event_channel(ec);
    rdma_destroy_id(ctx.listen_id);

    return 0;
}
