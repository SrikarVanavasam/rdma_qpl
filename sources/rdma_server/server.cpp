/**
 * UCT-based RDMA Server (Full QPL Support)
 * 
 * Replaces the Verbs-based server.cpp.
 * Supports:
 * - IAA WQ Portal Registration
 * - Data Pool Registration (for Staging)
 * - Completion Pool Registration
 * - TCP Handshake to exchange keys
 */

#include <iostream>
#include <string>
#include <vector>
#include <csignal>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#include "uct_transport.hpp"
#include "rdma_protocol.hpp"

using namespace qpl::ml::dispatcher;
using namespace qpl::rdma;

// Global shutdown flag
static volatile bool g_running = true;

void signal_handler(int) {
    g_running = false;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <wq_path>" << std::endl;
        return 1;
    }

    std::string wq_path = argv[1];
    
    std::cout << "[Server] Initializing UCT..." << std::endl;
    UctContext ctx;
    if (!ctx.init("mlx5_0")) { 
        if (!ctx.init("mlx5_2")) {
             std::cerr << "Failed to init UCT" << std::endl;
             return 1;
        }
    }

    // 1. WQ Portals
    std::vector<void*> portal_ptrs;
    std::vector<uct_mem_h> portal_memhs;
    int num_wqs = 0;

    for (int i = 1; i < argc; ++i) {
        if (num_wqs >= MAX_WQS) {
            std::cerr << "[Server] Warning: Max WQs reached (" << MAX_WQS << "). Ignoring " << argv[i] << std::endl;
            break;
        }
        std::string wq_path = argv[i];
        std::cout << "[Server] Opening WQ Portal " << num_wqs << ": " << wq_path << std::endl;
        
        int fd = open(wq_path.c_str(), O_RDWR);
        if (fd < 0) { perror("open wq"); return 1; }
        void* portal_ptr = mmap(nullptr, PORTAL_SIZE, PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0);
        close(fd);
        if (portal_ptr == MAP_FAILED) { perror("mmap wq"); return 1; }

        uct_mem_h portal_memh = ctx.register_memory(portal_ptr, PORTAL_SIZE);
        if (!portal_memh) return 1;

        portal_ptrs.push_back(portal_ptr);
        portal_memhs.push_back(portal_memh);
        num_wqs++;
    }

    // 2. Data Pool (Large buffers for Staging)
    // Size = NUM_JOBS * 3 (src1, src2, dst) * BLOCK_SIZE
    size_t data_pool_size = (size_t)NUM_JOBS * 3 * BLOCK_SIZE;
    void* data_pool_ptr = nullptr;
    if (posix_memalign(&data_pool_ptr, 4096, data_pool_size) != 0) {
        perror("alloc data pool"); return 1;
    }
    memset(data_pool_ptr, 0, data_pool_size); // Fault in
    
    std::cout << "[Server] Registering Data Pool (" << (data_pool_size/1024/1024) << " MB)..." << std::endl;
    uct_mem_h data_memh = ctx.register_memory(data_pool_ptr, data_pool_size);
    if (!data_memh) return 1;

    // 3. Completion Pool
    // Size = NUM_JOBS * COMP_SIZE + extra alignment
    size_t comp_pool_size = NUM_JOBS * COMP_SIZE * 2; // Extra space
    void* comp_pool_ptr = nullptr;
    if (posix_memalign(&comp_pool_ptr, 4096, comp_pool_size) != 0) return 1;
    memset(comp_pool_ptr, 0, comp_pool_size);

    std::cout << "[Server] Registering Completion Pool..." << std::endl;
    uct_mem_h comp_memh = ctx.register_memory(comp_pool_ptr, comp_pool_size);
    if (!comp_memh) return 1;

    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // No SA_RESTART
    sigaction(SIGINT, &sa, nullptr);
    std::cout << "[Server] Server Initialized. Entering accept loop..." << std::endl;

    while (g_running) {
        // Setup UCT Interface (Per connection)
        UctEndpoint ep(ctx);
        if (!ep.open_interface("mlx5_0:1")) {
            if(!ep.open_interface("mlx5_2:1")) {
                std::cerr << "Failed to open UCT interface" << std::endl;
                sleep(1); continue;
            }
        }

        // TCP Listener (Accepts one connection)
        TcpConnection listener;
        // Wait for connection
        if (!listener.listen(SERVER_PORT)) {
            // If interrupted or failed, retry
            if (!g_running) break;
            std::cerr << "Listen failed. Retrying..." << std::endl;
            sleep(1);
            continue;
        }

        std::cout << "[Server] Client Connecting..." << std::endl;
        
        // Send Protocol Data
        ConnPrivateData pdata;
        pdata.num_wqs = num_wqs;
        for(int i=0; i<num_wqs; ++i) {
            pdata.portal_addrs[i] = (uint64_t)portal_ptrs[i];
        }
        pdata.data_pool_addr = (uint64_t)data_pool_ptr;
        pdata.data_pool_count = NUM_JOBS * 3;
        pdata.comp_pool_addr = (uint64_t)comp_pool_ptr;
        pdata.comp_pool_count = NUM_JOBS;

        listener.send_val(pdata);
        
        auto send_rkey = [&](uct_mem_h memh) {
            void* buf = malloc(8192);
            ucs_status_t status = uct_md_mkey_pack(ctx.get_md(), memh, buf);
            if (status != UCS_OK) {
                std::cerr << "[Server] Failed to pack RKey: " << ucs_status_string(status) << std::endl;
            }
            uct_md_attr_t md_attr;
            uct_md_query(ctx.get_md(), &md_attr);
            size_t size = md_attr.rkey_packed_size;
            listener.send_val(size);
            listener.send(buf, size);
            free(buf);
        };

        for(int i=0; i<num_wqs; ++i) {
            send_rkey(portal_memhs[i]);
        }
        send_rkey(data_memh);
        send_rkey(comp_memh);

        // Exchange UCT Addresses
        auto local_addr = ep.get_local_address();
        listener.send_vec(local_addr.dev_addr);
        listener.send_vec(local_addr.iface_addr);

        UctEndpoint::AddressInfo remote_addr;
        listener.recv_vec(remote_addr.dev_addr);
        listener.recv_vec(remote_addr.iface_addr);
        listener.recv_vec(remote_addr.ep_addr);

        if (!ep.connect(remote_addr)) {
            std::cerr << "Failed to connect to client EP" << std::endl;
            continue;
        }
        
        // Send our EP address now that we are ready
        listener.send_vec(local_addr.ep_addr);
        
        std::cout << "[Server] Connected and Ready. Progressing..." << std::endl;

        while (g_running && listener.is_active()) {
            ctx.progress();
            // Poll frequently
            usleep(100); 
        }
        
        std::cout << "[Server] Client Disconnected/Gone. Resetting..." << std::endl;
    }

    std::cout << "[Server] Shutting down..." << std::endl;
    
    // Cleanup UCT resources 
    for(auto h : portal_memhs) ctx.deregister_memory(h);
    if (data_memh) ctx.deregister_memory(data_memh);
    if (comp_memh) ctx.deregister_memory(comp_memh);

    for(void* ptr : portal_ptrs) munmap(ptr, PORTAL_SIZE);
    free(data_pool_ptr);
    free(comp_pool_ptr);
    return 0;
}
