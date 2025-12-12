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
    std::string dev_path;

    // Portal
    void*          portal_buf = nullptr;
    struct ibv_mr* mr_portal  = nullptr;

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

    // 1. Allocate Protection Domain
    ctx->pd = ibv_alloc_pd(id->verbs);
    if (!ctx->pd) die("ibv_alloc_pd");

    // 2. Register Memory Regions
    // Portal: Local Write (to clear/reset?), Remote Write (Submission), Remote Read (Debugging?)
    // Note: User example used LOCAL_WRITE | REMOTE_WRITE | REMOTE_READ
    ctx->mr_portal = ibv_reg_mr(ctx->pd, ctx->portal_buf, PORTAL_SIZE,
                                IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
    if (!ctx->mr_portal) die("ibv_reg_mr portal");

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

    pdata.portal_addr = (uint64_t)ctx->portal_buf;
    pdata.portal_rkey = ctx->mr_portal->rkey;

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

    if (ctx->mr_portal) {
        ibv_dereg_mr(ctx->mr_portal);
        ctx->mr_portal = nullptr;
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
        std::cerr << "Usage: " << argv[0] << " <device_wq_path>" << std::endl;
        return 1;
    }

    ServerContext ctx;
    ctx.dev_path = argv[1];

    // 1. Map Portal
    int fd = open(ctx.dev_path.c_str(), O_RDWR);
    if (fd < 0) die("open device");

    ctx.portal_buf = mmap(NULL, PORTAL_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ctx.portal_buf == MAP_FAILED) die("mmap portal");
    close(fd); // Can close fd after mmap

    // 2. Allocate Buffers
    // Data Pool: 3 blocks per job * NUM_JOBS * BLOCK_SIZE
    ctx.data_pool_size = NUM_JOBS * 3 * BLOCK_SIZE;
    // TODO: Use hugepages for data pool to ensure large pages are used
    if (posix_memalign(&ctx.data_pool_buf, 2 * 1024 * 1024, ctx.data_pool_size)) die("posix_memalign data pool");
    std::memset(ctx.data_pool_buf, 0, ctx.data_pool_size);

    // Comp Pool: NUM_JOBS * COMP_SIZE (64B)
    ctx.comp_pool_size = NUM_JOBS * COMP_SIZE;
    // TODO: Use hugepages for completion pool
    if (posix_memalign(&ctx.comp_pool_buf, 64, ctx.comp_pool_size)) // 64-byte alignment
        die("posix_memalign comp pool");
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
    if (ctx.data_pool_buf) free(ctx.data_pool_buf);
    if (ctx.comp_pool_buf) free(ctx.comp_pool_buf);
    if (ctx.portal_buf) munmap(ctx.portal_buf, PORTAL_SIZE);

    rdma_destroy_event_channel(ec);
    rdma_destroy_id(ctx.listen_id);

    return 0;
}
