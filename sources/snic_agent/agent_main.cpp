/**
 * UCT-based SNIC Agent (Client)
 *
 * Connects to Host Server via TCP, establishes UCT connection,
 * and submits task chains (initially just NOOP descriptors) to Host IAA.
 */

#include <iostream>
#include <string>
#include <vector>
#include <csignal>
#include <unistd.h>
#include <cstring>

#include "uct_transport.hpp"
#include "task_chain.hpp"
#include "rdma_protocol.hpp"

using namespace qpl::ml::dispatcher;
using namespace qpl::rdma;

// Configuration
// SERVER_PORT is defined in rdma_protocol.hpp
constexpr int NUM_DESCRIPTORS = 1;

// IAA Opcode for NOOP
constexpr uint32_t IAX_OPCODE_NOOP = 0x0;
constexpr uint32_t IDXD_OP_FLAG_RCR = 0x2000;  // Request Completion Record
constexpr uint32_t IDXD_OP_FLAG_CRAV = 0x4000; // Completion Record Address Valid

struct iax_hw_desc {
    uint32_t pasid:20;
    uint32_t rsvd:11;
    uint32_t priv:1;
    uint32_t flags:24;
    uint32_t opcode:8;
    uint64_t completion_addr;
    uint64_t src1_addr;
    uint64_t dst_addr;
    uint32_t src1_size;
    uint16_t int_handle;
    uint16_t rsvd2;
    uint64_t src2_addr;
    uint32_t src2_size;
    uint32_t rsvd3;
    uint64_t rsvd4[4];
} __attribute__((packed));

struct iax_completion_record {
    volatile uint8_t status;
    uint8_t error_code;
    uint16_t rsvd;
    uint32_t output_size;
    uint64_t output_crc;
    uint64_t rsvd2[2];
} __attribute__((packed));

void completion_callback(void* arg, const void* data, size_t length) {
    if (length != sizeof(iax_completion_record)) return;
    memcpy(arg, data, length);
}


int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <host_ip>" << std::endl;
        return 1;
    }
    std::string host_ip = argv[1];

    std::cout << "[Agent] Initializing UCT..." << std::endl;
    UctContext ctx;
    // On SNIC (ARM), device might satisfy "mlx5_0" via Bluefield shim, or "mlx5_2" etc.
    // Try mlx5_0 first (typical for BF2/BF3 host-rep or function).
    // Note: If running on X86 host (loopback), mlx5_0 works.
    if (!ctx.init("mlx5_0")) {
        if (!ctx.init("mlx5_2")) {
            std::cerr << "Failed to init UCT" << std::endl;
            return 1;
        }
    }

    // Allocate local completion buffer
    // Moved declarations here to ensure scope visibility
    iax_completion_record local_comp = {};

    // Create Endpoint
    UctEndpoint ep(ctx);
    if (ctx.supports_xgvmi()) {
        // Just checking, not using yet
        std::cout << "[Agent] XGVMI supported!" << std::endl;
    }
    
    // Open interface
    // Try mlx5_0:1 or mlx5_2:1
    if (!ep.open_interface("mlx5_0:1")) {
         if (!ep.open_interface("mlx5_2:1")) {
             std::cerr << "Failed to open UCT interface" << std::endl;
             // Don't exit yet, maybe connection will fail later if truly broken
             // but assume open_interface is critical.
             return 1;
         }
    }

    // Connect via TCP
    TcpConnection conn;
    std::cout << "[Agent] Connecting to Host " << host_ip << ":" << SERVER_PORT << "..." << std::endl;
    
    if (!conn.connect(host_ip, SERVER_PORT)) {
        sleep(1);
        if (!conn.connect(host_ip, SERVER_PORT)) {
            std::cerr << "Failed to connect to host" << std::endl;
            return 1;
        }
    }

    // Receive Server Info (ConnPrivateData)
    ConnPrivateData pdata;
    if (!conn.recv_val(pdata)) return 1;
    
    // Receive RKeys
    auto recv_rkey = [&]() -> uct_rkey_t {
         size_t len;
         if (!conn.recv_val(len)) return 0;
         std::vector<uint8_t> buf(len);
         if (!conn.recv(buf.data(), len)) return 0;
         
         uct_rkey_bundle_t bundle;
         ep.unpack_rkey(buf.data(), &bundle);
         return bundle.rkey;
    };
    
    // Receive Portal RKeys
    std::vector<uct_rkey_t> portal_rkeys;
    for (uint32_t i=0; i<pdata.num_wqs; ++i) {
        portal_rkeys.push_back(recv_rkey());
    }

    // Receive Data & Comp RKey
    uct_rkey_t data_pool_rkey = recv_rkey();
    uct_rkey_t comp_pool_rkey = recv_rkey();
    
    // Pick first portal
    uint64_t portal_va = pdata.portal_addrs[0];
    uct_rkey_t portal_rkey = portal_rkeys[0];
    
    // Pick first comppool slot (job 0)
    uint64_t comp_va = pdata.comp_pool_addr + 0 * COMP_SIZE;
    uct_rkey_t comp_rkey = comp_pool_rkey; // Shadowing variable name? No, reassign or use same scope?
    // Wait, line 101: uct_rkey_t comp_pool_rkey = recv_rkey();
    // Here: uct_rkey_t comp_rkey = comp_pool_rkey;
    // But comp_rkey was NOT declared before?
    // Ah, Step 3253 snippet had `uct_rkey_t comp_rkey = recv_rkey();` which shadowed or was new.
    // I should use distinct names.
    // Actually, distinct is better.
    uct_rkey_t remote_comp_rkey = comp_pool_rkey;

    std::cout << "[Agent] Host Info: WQs=" << pdata.num_wqs << ", Portal0 VA=" << std::hex << portal_va 
              << ", Comp VA=" << comp_va << std::dec << std::endl;
              
    // Exchange Address Info
    auto local_addr = ep.get_local_address();
    
    UctEndpoint::AddressInfo remote_addr;
    conn.recv_vec(remote_addr.dev_addr);
    conn.recv_vec(remote_addr.iface_addr);
    
    conn.send_vec(local_addr.dev_addr);
    conn.send_vec(local_addr.iface_addr);
    conn.send_vec(local_addr.ep_addr);

    conn.recv_vec(remote_addr.ep_addr);
    
    if (!ep.connect(remote_addr)) {
         std::cerr << "Failed to UCT connect" << std::endl;
         return 1;
    }
    std::cout << "[Agent] UCT Connected!" << std::endl;

    // TEST MODE: Write to Host Memory (Comp Pool) to verify Transport
    // iax_hw_desc desc = {};
    // desc.opcode = IAX_OPCODE_NOOP;
    // ...
    // ucs_status_t status = ep.put_short(&desc, sizeof(desc), portal_va, portal_rkey);
    
    std::cout << "[Agent] DEBUG: Writing magic value to Remote Host Memory (Comp Pool)..." << std::endl;
    uint64_t magic = 0xDEADBEEFCAFEBABE;
    ucs_status_t status = ep.put_short(&magic, sizeof(magic), comp_va, comp_rkey); // Write to COMP (Host Mem)
    
    if (status != UCS_OK) {
        std::cerr << "Put Short Failed: " << ucs_status_string(status) << std::endl;
        return 1;
    }
    
    // Flush to ensure write completes
    ep.flush();
    std::cout << "[Agent] Write flushed. Now reading back..." << std::endl;
    
    // Poll Completion via RDMA Read
    // We repeatedly read the remote completion record into our local buffer
    std::cout << "[Agent] Polling completion..." << std::endl;
    bool completed = false;
    for (int i=0; i<10000; ++i) {
        // Read remote completion record
        status = ep.get_bcopy(completion_callback, &local_comp, sizeof(local_comp), comp_va, remote_comp_rkey);
        if (status == UCS_OK) {
            // Check status
             if (local_comp.status != 0) {
                 completed = true;
                 break;
             }
        }
        ctx.progress(); 
        
        if (i % 1000 == 0) usleep(100);
    }
    
    if (completed) {
        uint64_t val;
        memcpy(&val, (void*)&local_comp, sizeof(uint64_t));
        std::cout << "[Agent] Read Status: " << (int)local_comp.status << std::endl;
        std::cout << "[Agent] Read Data: 0x" << std::hex << val << std::dec << std::endl;
        if (val == 0xDEADBEEFCAFEBABE) {
             std::cout << "[Agent] SUCCESS! Magic value matches. Transport is GOOD." << std::endl;
        } else {
             std::cout << "[Agent] FAILURE! Magic value mismatch." << std::endl;
        }
    } else {
        std::cout << "[Agent] TIMEOUT! No completion status." << std::endl;
    }

    return 0;
}
