#include "rdma_client.hpp"

#include <iostream>
#include <unistd.h>
#include <cstring>
#include <fcntl.h>

namespace qpl::ml::dispatcher {

// RKey constants removed (dynamic indexing)

RdmaClient& RdmaClient::get_instance() {
    static RdmaClient instance;
    return instance;
}

RdmaClient::RdmaClient() {
    for (uint32_t i = 0; i < rdma::NUM_JOBS; ++i) {
        free_job_slots_.push(rdma::NUM_JOBS - 1 - i);
    }
    meta_states_.resize(rdma::NUM_JOBS);
    data_states_.resize(rdma::NUM_JOBS);
}

RdmaClient::~RdmaClient() {
    cleanup_resources();
}

void RdmaClient::cleanup_resources() {
    if (initialized_) {
        // Destroy EP first to release memory references
        ep_.reset();

        // Release RKeys
        for (auto& bundle : remote_keys_) {
             uct_rkey_release(ctx_.get_component(), &bundle);
        }
        remote_keys_.clear();

        // Dereg
        if (data_staging_memh_) ctx_.deregister_memory(data_staging_memh_);
        if (desc_staging_memh_) ctx_.deregister_memory(desc_staging_memh_);
        if (comp_staging_memh_) ctx_.deregister_memory(comp_staging_memh_);
        
        free(data_staging_pool_);
        free(desc_staging_pool_);
        free(comp_staging_pool_);

        // Deregister user buffers
        std::lock_guard<std::mutex> lock(region_mutex_);
        for (auto& pair : registered_regions_) {
             ctx_.deregister_memory(pair.second.memh);
        }
        registered_regions_.clear();
    }
}

bool RdmaClient::initialize(const std::string& server_ip) {
    if (initialized_) return true;
    server_ip_ = server_ip;

    // 1. Init UCT
    // 1. Initialize UCT Context
    if (!ctx_.init("mlx5_0")) {
        if (!ctx_.init("mlx5_2")) {
            std::cerr << "Failed to init UCT" << std::endl;
            return false;
        }
    }

    // 2. Allocate & Register Staging Memory
    // Note: We use staging for Data, Descriptors, and Completions to match the Verbs path.
    // Data Pool
    size_t data_pool_size = (size_t)rdma::NUM_JOBS * 3 * rdma::BLOCK_SIZE;
    if (posix_memalign(&data_staging_pool_, 4096, data_pool_size) != 0) return false;
    memset(data_staging_pool_, 0, data_pool_size);
    data_staging_memh_ = ctx_.register_memory(data_staging_pool_, data_pool_size);

    // Desc Pool (64B * Jobs)
    size_t desc_pool_size = rdma::NUM_JOBS * rdma::DESC_SIZE * 2; // Alignment
    if (posix_memalign(&desc_staging_pool_, 4096, desc_pool_size) != 0) return false;
    memset(desc_staging_pool_, 0, desc_pool_size);
    desc_staging_memh_ = ctx_.register_memory(desc_staging_pool_, desc_pool_size);

    // Comp Pool
    size_t comp_pool_size = rdma::NUM_JOBS * rdma::COMP_SIZE * 2;
    if (posix_memalign(&comp_staging_pool_, 4096, comp_pool_size) != 0) return false;
    memset(comp_staging_pool_, 0, comp_pool_size);
    comp_staging_memh_ = ctx_.register_memory(comp_staging_pool_, comp_pool_size);


    // 3. Connect TCP
    if (!conn_.connect(server_ip, rdma::SERVER_PORT)) {
        std::cerr << "Failed to connect to " << server_ip << std::endl;
        return false;
    }

    // 4. Receive Protocol Data
    conn_.recv_val(remote_config_); // The struct
    
    // 5. Receive RKeys
    
    // Create Endpoint first to use its unpack helper
    ep_ = std::make_unique<UctEndpoint>(ctx_);
    if (!ep_->open_interface("mlx5_0:1")) { // Try default
         if (!ep_->open_interface("mlx5_2:1")) return false;
    }

    // Recv keys helper
    auto recv_and_store_key = [&](int idx) -> bool {
        size_t size;
        if (!conn_.recv_val(size)) return false;
        std::vector<uint8_t> buf(size);
        if (!conn_.recv(buf.data(), size)) return false;
        
        uct_rkey_bundle_t bundle;
        if (ep_->unpack_rkey(buf.data(), &bundle) != UCS_OK) return false;
        
        if (remote_keys_.size() <= (size_t)idx) remote_keys_.resize(idx + 1);
        remote_keys_[idx] = bundle;
        return true;
    };

    // Receive Portal RKeys (0 to num_wqs-1)
    for (uint32_t i = 0; i < remote_config_.num_wqs; ++i) {
        if (!recv_and_store_key(i)) return false;
    }
    
    // Receive Data Pool RKey (at index num_wqs)
    if (!recv_and_store_key(remote_config_.num_wqs)) return false;

    // Receive Comp Pool RKey (at index num_wqs + 1)
    if (!recv_and_store_key(remote_config_.num_wqs + 1)) return false;


    // 6. Exchange Addresses
    auto local_addr = ep_->get_local_address();
    // Client sends first? No, Server sends first in reference loop.
    // Receive Server Dev/Iface
    UctEndpoint::AddressInfo remote_addr;
    conn_.recv_vec(remote_addr.dev_addr);
    conn_.recv_vec(remote_addr.iface_addr);
    
    // Send Client Dev/Iface/EP
    conn_.send_vec(local_addr.dev_addr);
    conn_.send_vec(local_addr.iface_addr);
    conn_.send_vec(local_addr.ep_addr);

    // Receive Server EP
    conn_.recv_vec(remote_addr.ep_addr);

    if (!ep_->connect(remote_addr)) return false;

    initialized_ = true;
    std::cout << "[RdmaClient] Initialized UCT connection. WQs=" << remote_config_.num_wqs << std::endl;
    return true;
}

static void unpack_cb(void *arg, const void *data, size_t length) {
    memcpy(arg, data, length);
}

bool RdmaClient::write(const void* local_addr, size_t size, uint64_t remote_addr, uint32_t rkey_idx, bool /*signaled*/) {
    if (!initialized_) return false;
    
    // 1. Check if User Registered Buffer (Zero-Copy)
    uct_mem_h memh = get_registered_memh(local_addr, size);
    
    // 2. Fallback: Check Staging Pools
    if (!memh) {
        memh = data_staging_memh_; 

        // Check if in desc pool?
        if (local_addr >= desc_staging_pool_ && local_addr < (char*)desc_staging_pool_ + rdma::DESC_SIZE * rdma::NUM_JOBS * 2) {
            memh = desc_staging_memh_;
        }
    }
    
    ucs_status_t status = ep_->put_zcopy(local_addr, size, memh, remote_addr, get_rkey_val(rkey_idx));
    if (status != UCS_OK && status != UCS_INPROGRESS) {
        std::cerr << "[RdmaClient] write (zcopy) failed: " << ucs_status_string(status) << std::endl;
    }
    return status == UCS_OK || status == UCS_INPROGRESS;
}

bool RdmaClient::write_short(const void* local_addr, size_t size, uint64_t remote_addr, uint32_t rkey_idx) {
    if (!initialized_) return false;
    ucs_status_t status = ep_->put_short(local_addr, size, remote_addr, get_rkey_val(rkey_idx));
    if (status != UCS_OK) {
        std::cerr << "[RdmaClient] write_short failed: " << ucs_status_string(status) 
                  << " size=" << size << " rkey=" << rkey_idx << std::endl;
    }
    return status == UCS_OK;
}

// Callback for completion counting (no-op)
static void simple_comp_cb(uct_completion_t * /*self*/) {}

bool RdmaClient::read(void* local_addr, size_t size, uint64_t remote_addr, uint32_t rkey_idx) {
    uct_completion_t comp;
    comp.func = simple_comp_cb;
    comp.count = 2; // Initial (1) + Operation (1)

    // std::cout << "[RdmaClient] read initiating... size=" << size << std::endl;
    uct_rkey_t rkey = get_rkey_val(rkey_idx);
    std::cout << "[RdmaClient] RDMA Read: VA=" << std::hex << remote_addr << " RKey=" << std::hex << rkey 
              << " Size=" << std::dec << size << std::endl;
    ucs_status_t status = ep_->get_bcopy(unpack_cb, local_addr, size, remote_addr, rkey, &comp);
    
    if (status == UCS_OK) {
        // std::cout << "[RdmaClient] read completed immediately." << std::endl;
        return true;
    }
    
    if (status == UCS_INPROGRESS) {
        comp.count--; // Decrement local reference
        int spins = 0;
        const int MAX_SPINS = 100000000; // Approx 5-10s depending on CPU
        while (comp.count > 0 && spins < MAX_SPINS) {
            ctx_.progress();
            spins++;
            if (spins % 10000000 == 0) {
                 std::cout << "[RdmaClient] read spinning... count=" << comp.count << " spins=" << spins << std::endl;
            }
        }
        if (comp.count > 0) {
             std::cerr << "[RdmaClient] Read Timed Out!" << std::endl;
             return false;
        }
        return true;
    }
    
    std::cerr << "[RdmaClient] read failed: " << ucs_status_string(status) << std::endl;
    return false;
}

// Helpers
uct_rkey_t RdmaClient::get_rkey_val(uint32_t idx) const {
    if (idx < remote_keys_.size()) return remote_keys_[idx].rkey;
    return 0; // Invalid
}

int RdmaClient::get_job_slot() {
    std::lock_guard<std::mutex> lock(slot_mutex_);
    if (free_job_slots_.empty()) return -1;
    int slot = free_job_slots_.top();
    free_job_slots_.pop();
    if (slot > max_active_slot_) max_active_slot_ = slot;
    return slot;
}

void RdmaClient::release_job_slot(int slot_id) {
    std::lock_guard<std::mutex> lock(slot_mutex_);
    free_job_slots_.push(slot_id);
}

// Accessors to mapping logic
uint64_t RdmaClient::get_remote_data_block_addr(int slot_id, int block_idx) {
    // 0=src1, 1=src2, 2=dst
    return remote_config_.data_pool_addr + ((uint64_t)slot_id * 3 + block_idx) * rdma::BLOCK_SIZE;
}
uint32_t RdmaClient::get_remote_data_block_rkey() { 
    return remote_config_.num_wqs; // Index after portals
}

uint64_t RdmaClient::get_remote_comp_addr(int slot_id) {
    return remote_config_.comp_pool_addr + slot_id * rdma::COMP_SIZE;
}
uint32_t RdmaClient::get_remote_comp_rkey() { 
    return remote_config_.num_wqs + 1; // Index after data
}

uint64_t RdmaClient::get_remote_portal_addr() { return remote_config_.portal_addrs[0]; }
uint32_t RdmaClient::get_remote_portal_rkey() { return 0; }

uint64_t RdmaClient::get_remote_portal_addr(uint32_t wq_idx) { 
    if (wq_idx < remote_config_.num_wqs) return remote_config_.portal_addrs[wq_idx];
    return 0;
}
uint32_t RdmaClient::get_remote_portal_rkey(uint32_t wq_idx) { 
    return wq_idx; 
}

uint32_t RdmaClient::get_num_wqs() const { return remote_config_.num_wqs; }
uint32_t RdmaClient::get_next_wq_index() { 
    if (remote_config_.num_wqs == 0) return 0;
    uint32_t idx = wq_index_++;
    return idx % remote_config_.num_wqs;
}


// Staging Accessors
void* RdmaClient::get_data_staging(int slot_id) {
    return (uint8_t*)data_staging_pool_ + (slot_id * 3 * rdma::BLOCK_SIZE);
}
void* RdmaClient::get_desc_staging(int slot_id) {
    return (uint8_t*)desc_staging_pool_ + (slot_id * rdma::DESC_SIZE);
}
void* RdmaClient::get_comp_staging(int slot_id) {
    return (uint8_t*)comp_staging_pool_ + (slot_id * rdma::COMP_SIZE);
}
uint32_t RdmaClient::get_data_staging_lkey() { return 0; } // UCT doesn't explicitly use LKey in zcopy API (uses memh)
uint32_t RdmaClient::get_desc_staging_lkey() { return 0; }
uint32_t RdmaClient::get_comp_staging_lkey() { return 0; }

uint8_t* RdmaClient::get_local_desc_buffer(int slot_id) { return (uint8_t*)get_desc_staging(slot_id); }
int RdmaClient::get_max_active_slot() { return max_active_slot_; }


void RdmaClient::poll() {
    if (initialized_) {
        ctx_.progress();
    }
}

ucs_status_t RdmaClient::read_async(int slot_id, void* dst, size_t size, uint64_t remote_addr, uint32_t rkey_idx) {
    if (slot_id < 0 || slot_id >= (int)data_states_.size()) return UCS_ERR_INVALID_PARAM;
    // Select State Vector based on size
    auto& state = (size <= 512) ? meta_states_[slot_id] : data_states_[slot_id];
    
    if (!state.read_issued) {
        state.comp.count = 2; // Init to 2
        state.comp.func = simple_comp_cb;
        state.is_zcopy = false;

        ucs_status_t status;
        
        // Attempt ZCopy
        uct_mem_h zcopy_memh = nullptr;
        if (size > 512) {
             zcopy_memh = get_registered_memh(dst, size);
        }

        if (zcopy_memh) {
            state.is_zcopy = true;
            status = ep_->get_zcopy(dst, size, zcopy_memh, remote_addr, get_rkey_val(rkey_idx), &state.comp);
        } else if (size <= 512) {
             // BCOPY Path (Meta Channel)
             status = ep_->get_bcopy(unpack_cb, dst, size, remote_addr, get_rkey_val(rkey_idx), &state.comp);
        } else {
             // ZCOPY Path (Data Channel) - Staging Fallback
             if (size > rdma::BLOCK_SIZE) return UCS_ERR_NO_MEMORY;
             uint8_t* bounce_buf = (uint8_t*)get_data_staging(slot_id) + 2 * rdma::BLOCK_SIZE;
             status = ep_->get_zcopy(bounce_buf, size, data_staging_memh_, remote_addr, get_rkey_val(rkey_idx), &state.comp);
        }
        
        if (status == UCS_OK) { // Immediate completion
             if (!state.is_zcopy && size > 512) {
                  uint8_t* bounce_buf = (uint8_t*)get_data_staging(slot_id) + 2 * rdma::BLOCK_SIZE;
                  memcpy(dst, bounce_buf, size);
             }
             return UCS_OK;
        }
        
        if (status == UCS_INPROGRESS) {
            state.read_issued = true;
            return UCS_INPROGRESS;
        }
        return status;
    } else {
        if (state.comp.count > 1) {
            poll(); // Drive progress
            if (state.comp.count > 1) return UCS_INPROGRESS;
        }
        
        // Done (count == 1). 
        if (!state.is_zcopy && size > 512) {
             uint8_t* bounce_buf = (uint8_t*)get_data_staging(slot_id) + 2 * rdma::BLOCK_SIZE;
             memcpy(dst, bounce_buf, size);
        }
        
        state.read_issued = false; 
        return UCS_OK;
    }
}

    // --- Buffer Registration ---

    bool RdmaClient::register_buffer(void* addr, size_t length) {
        if (!initialized_) {
            const char* server_ip = std::getenv("QPL_RDMA_SERVER_IP");
            if (server_ip) {
                if (!initialize(server_ip)) {
                    std::cerr << "[RdmaClient] Auto-init failed during register_buffer." << std::endl;
                    return false;
                }
            } else {
                 std::cerr << "[RdmaClient] register_buffer called before init and QPL_RDMA_SERVER_IP not set." << std::endl;
                 return false;
            }
        }

        std::lock_guard<std::mutex> lock(region_mutex_);

        // Check for overlaps? For now assume user is well-behaved.
        // Actually, UCT reg is expensive, so let's do it.
        uct_mem_h memh = ctx_.register_memory(addr, length);
        if (!memh) {
            std::cerr << "[RdmaClient] UCT register failed." << std::endl;
            return false;
        }

        registered_regions_[addr] = {memh, addr, length};
        return true;
    }

    void RdmaClient::unregister_buffer(void* addr) {
         std::lock_guard<std::mutex> lock(region_mutex_);
         auto it = registered_regions_.find(addr);
         if (it != registered_regions_.end()) {
             ctx_.deregister_memory(it->second.memh);
             registered_regions_.erase(it);
         }
    }

    uct_mem_h RdmaClient::get_registered_memh(const void* addr, size_t length) {
        // Optimistic lock?
        std::lock_guard<std::mutex> lock(region_mutex_);
        
        if (registered_regions_.empty()) return nullptr;

        // Find the first region that strictly starts AFTER addr
        auto it = registered_regions_.upper_bound((void*)addr);

        // If upper_bound returns begin(), then ALL regions start > addr.
        // So no region can contain [addr, addr+len).
        if (it == registered_regions_.begin()) return nullptr;

        // Move back one to find the region that starts <= addr
        --it;

        // Check bounds
        // it->first is start_addr
        // it->second is MemRegion struct
        const char* region_start = (const char*)it->second.start;
        const char* region_end   = region_start + it->second.length;
        const char* req_start    = (const char*)addr;
        const char* req_end      = req_start + length;

        if (req_start >= region_start && req_end <= region_end) {
            return it->second.memh;
        }

            return nullptr;
    }

    bool RdmaClient::is_registered(const void* addr, size_t length) {
        return get_registered_memh(addr, length) != nullptr;
    }

} // namespace
