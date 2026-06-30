/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @date 03/23/2020
 * @brief Internal HW API functions for @ref hw_enqueue_descriptor API implementation
 */

#include <cstdlib> // For getenv
#include <cstring> // For memcpy
#include <iostream>
#include <new> // For std::nothrow
#include <ostream>
#include <unordered_map>
#include <cstddef> // For offsetof
#include <cerrno>  // For EAGAIN

#include "dispatcher/hw_dispatcher.hpp"
#include "hw_definitions.h"
#include "hw_descriptors_api.h"
#include "rdma_client.hpp"   // RDMA Client
#include "rdma_protocol.hpp" // For QPL_RDMA_REMOTE_NUMA_ID
#include "cxl_client.hpp"    // CXL Client
#include "cxl_protocol.hpp"  // For QPL_LOCAL_PROXY_NUMA_ID
#include "util/hw_timing_util.hpp"
#include "hardware_state.h"  // For qpl_hw_state and rdma_slot_id
#include <x86intrin.h>

/* If we didn't successfully submit, we need to prioritize the return value
 * Priority is as follows:
 *   HW_ACCELERATOR_WQ_IS_BUSY                (found a workqueue that supports our descriptor, but it was busy)
 *   HW_ACCELERATOR_TRANSFER_SIZE_EXCEEDED    (found a workqueue that supports our operation, but the transfer size was too large)
 *   HW_ACCELERATOR_NOT_SUPPORTED_BY_WQ       (found an available workqueue, but it didn't support our operation)
 *   HW_ACCELERATOR_WORK_QUEUES_NOT_AVAILABLE (never found an available workqueue)
 */
static const std::unordered_map<hw_accelerator_status, uint8_t> hw_status_to_priority = {
        {HW_ACCELERATOR_WQ_IS_BUSY, 1U},
        {HW_ACCELERATOR_TRANSFER_SIZE_EXCEEDED, 2U},
        {HW_ACCELERATOR_NOT_SUPPORTED_BY_WQ, 3U},
        {HW_ACCELERATOR_WORK_QUEUES_NOT_AVAILABLE, 4U}};

extern "C"
__attribute__((target("movdir64b,clflushopt")))
hw_accelerator_status hw_enqueue_descriptor(void* desc_ptr, int32_t user_specified_numa_id,
                                                       qpl::ml::util::execution_record_ext_t* record) {
    //DEBUG: Descriptor dump
    // static int call_count = 0;
    // call_count++;
    // std::cout << "[QPL] hw_enqueue_descriptor called with numa_id: " << user_specified_numa_id << " (Call #"
    //           << call_count << ")" << std::endl;

    auto* desc = reinterpret_cast<hw_decompress_analytics_descriptor*>(desc_ptr);

    if (user_specified_numa_id == qpl::rdma::QPL_RDMA_REMOTE_NUMA_ID || 
        user_specified_numa_id == qpl::rdma::QPL_RDMA_STAGING_NUMA_ID) {
        static bool                             rdma_client_initialized = false;
        static qpl::ml::dispatcher::RdmaClient* rdma_client_instance    = nullptr;

        if (!rdma_client_initialized) {
            if (const char* env_ip = std::getenv("QPL_RDMA_SERVER_IP")) {
                rdma_client_instance = &qpl::ml::dispatcher::RdmaClient::get_instance();
                // Enable ODP MR only for ODP mode (-100), not for staging mode (-101)
                bool enable_odp = (user_specified_numa_id == qpl::rdma::QPL_RDMA_REMOTE_NUMA_ID);
                if (!rdma_client_instance->initialize(env_ip, enable_odp)) {
                    std::cerr << "[QPL] Failed to initialize RDMA client to " << env_ip << std::endl;
                    return HW_ACCELERATOR_WORK_QUEUES_NOT_AVAILABLE;
                }
                rdma_client_initialized = true;
            } else {
                std::cerr << "[QPL] RDMA NUMA ID used but QPL_RDMA_SERVER_IP not set." << std::endl;
                return HW_ACCELERATOR_WORK_QUEUES_NOT_AVAILABLE;
            }
        }

        if (!rdma_client_instance || !rdma_client_instance->is_initialized()) {
            return HW_ACCELERATOR_WORK_QUEUES_NOT_AVAILABLE;
        }

        auto& client = *rdma_client_instance;
        auto* comp_local = reinterpret_cast<hw_completion_record*>(desc->completion_record_ptr);
        auto* state_ptr = reinterpret_cast<qpl_hw_state*>(
            reinterpret_cast<char*>(comp_local) - offsetof(qpl_hw_state, comp_ptr));

        // Determine mode: staging (explicit MR) or ODP
        bool use_staging = (user_specified_numa_id == qpl::rdma::QPL_RDMA_STAGING_NUMA_ID);

        int slot_id = state_ptr->rdma_slot_id;
        bool reusing_slot = (slot_id >= 0);

        if (!reusing_slot) {
            slot_id = client.get_job_slot();
            if (slot_id < 0) {
                std::cerr << "[QPL] No free RDMA job slots available." << std::endl;
                return HW_ACCELERATOR_WQ_IS_BUSY;
            }
            state_ptr->rdma_slot_id = slot_id;
            state_ptr->rdma_synced_src1 = nullptr;
            state_ptr->rdma_synced_src2 = nullptr;
            state_ptr->rdma_synced_dst = nullptr;
        }

        if (use_staging) {
            // --- STAGING MODE: Copy to staging buffers, use explicit lkeys ---
            void* data_stg = client.get_data_staging(slot_id);
            void* desc_stg = client.get_desc_staging(slot_id);
            void* comp_stg = client.get_comp_staging(slot_id);
            
            if (!data_stg || !desc_stg || !comp_stg) {
                if (!reusing_slot) { client.release_job_slot(slot_id); state_ptr->rdma_slot_id = -1; }
                return HW_ACCELERATOR_WQ_IS_BUSY;
            }

            // Copy source data to staging (always send - content can change even with same pointer)
            if (desc->src1_ptr && desc->src1_size > 0) {
                std::memcpy(data_stg, desc->src1_ptr, desc->src1_size);
                uint64_t remote_src1 = client.get_remote_data_block_addr(slot_id, 0);
                client.prepare_write_with_lkey(data_stg, desc->src1_size, remote_src1, 
                                                client.get_remote_data_block_rkey(), 
                                                client.get_data_staging_lkey(), false);
            }

            // Copy src2 (AECS/Huffman tables for compression) to staging
            if (desc->src2_ptr && desc->src2_size > 0) {
                // src2 goes to block 1 in remote data area
                uint8_t* src2_stg = static_cast<uint8_t*>(data_stg) + qpl::rdma::BLOCK_SIZE;
                std::memcpy(src2_stg, desc->src2_ptr, desc->src2_size);
                uint64_t remote_src2 = client.get_remote_data_block_addr(slot_id, 1);
                client.prepare_write_with_lkey(src2_stg, desc->src2_size, remote_src2, 
                                                client.get_remote_data_block_rkey(), 
                                                client.get_data_staging_lkey(), false);
            }

            // Copy descriptor to staging
            std::memcpy(desc_stg, desc_ptr, 64);
            auto* remote_desc = reinterpret_cast<hw_decompress_analytics_descriptor*>(desc_stg);
            if (desc->src1_ptr)
                remote_desc->src1_ptr = reinterpret_cast<uint8_t*>(client.get_remote_data_block_addr(slot_id, 0));
            if (desc->src2_ptr)
                remote_desc->src2_ptr = reinterpret_cast<uint8_t*>(client.get_remote_data_block_addr(slot_id, 1));
            if (desc->dst_ptr)
                remote_desc->dst_ptr = reinterpret_cast<uint8_t*>(client.get_remote_data_block_addr(slot_id, 2));
            remote_desc->completion_record_ptr = reinterpret_cast<uint8_t*>(client.get_remote_comp_addr(slot_id));

            // Zero out staging completion record before sending
            std::memset(comp_stg, 0, 64);
            client.prepare_write_with_lkey(comp_stg, 64, client.get_remote_comp_addr(slot_id),
                                            client.get_remote_comp_rkey(),
                                            client.get_comp_staging_lkey(), false);
            
            // Send descriptor to portal (round-robin across WQs)
            uint32_t wq_idx = client.get_next_wq_index();
            client.prepare_write_with_lkey(desc_stg, 64, client.get_remote_portal_addr(wq_idx),
                                            client.get_remote_portal_rkey(wq_idx),
                                            client.get_desc_staging_lkey(), true);
        } else {
            // --- ODP MODE: Use original buffers with ODP lkey ---
            if (desc->src1_ptr && desc->src1_size > 0) {
                uint64_t remote_src1 = client.get_remote_data_block_addr(slot_id, 0);
                client.prepare_write(desc->src1_ptr, desc->src1_size, remote_src1, client.get_remote_data_block_rkey(), false);
            }

            if (desc->src2_ptr && desc->src2_size > 0) {
                uint64_t remote_src2 = client.get_remote_data_block_addr(slot_id, 1);
                client.prepare_write(desc->src2_ptr, desc->src2_size, remote_src2, client.get_remote_data_block_rkey(), false);
            }

            uint8_t* persistent_desc_buf = client.get_local_desc_buffer(slot_id);
            if (!persistent_desc_buf) {
                if (!reusing_slot) { client.release_job_slot(slot_id); state_ptr->rdma_slot_id = -1; }
                return HW_ACCELERATOR_WQ_IS_BUSY;
            }
            std::memcpy(persistent_desc_buf, desc_ptr, 64);

            auto* remote_desc = reinterpret_cast<hw_decompress_analytics_descriptor*>(persistent_desc_buf);
            if (desc->src1_ptr)
                remote_desc->src1_ptr = reinterpret_cast<uint8_t*>(client.get_remote_data_block_addr(slot_id, 0));
            if (desc->src2_ptr)
                remote_desc->src2_ptr = reinterpret_cast<uint8_t*>(client.get_remote_data_block_addr(slot_id, 1));
            if (desc->dst_ptr)
                remote_desc->dst_ptr = reinterpret_cast<uint8_t*>(client.get_remote_data_block_addr(slot_id, 2));
            remote_desc->completion_record_ptr = reinterpret_cast<uint8_t*>(client.get_remote_comp_addr(slot_id));

            static uint8_t zero_comp[64] = {0};
            client.prepare_write(zero_comp, 64, client.get_remote_comp_addr(slot_id), client.get_remote_comp_rkey(), false);
            
            // Send descriptor to portal (round-robin across WQs)
            uint32_t wq_idx = client.get_next_wq_index();
            client.prepare_write(persistent_desc_buf, 64, client.get_remote_portal_addr(wq_idx), client.get_remote_portal_rkey(wq_idx), true);
        }

        if (!client.commit_batch()) {
            std::cerr << "[QPL] Failed to commit RDMA batch." << std::endl;
            if (!reusing_slot) { client.release_job_slot(slot_id); state_ptr->rdma_slot_id = -1; }
            return HW_ACCELERATOR_WQ_IS_BUSY;
        }

        return HW_ACCELERATOR_STATUS_OK;
    }
    // --- END RDMA SUBMISSION LOGIC ---

    // --- CXL PROXY SUBMISSION LOGIC ---
    if (user_specified_numa_id == qpl::cxl::QPL_LOCAL_PROXY_NUMA_ID || 
        user_specified_numa_id == qpl::cxl::QPL_LOCAL_PROXY_UMWAIT_NUMA_ID ||
        user_specified_numa_id == qpl::cxl::QPL_CXL_PROXY_NUMA_ID || 
        user_specified_numa_id == qpl::cxl::QPL_CXL_PROXY_UMWAIT_NUMA_ID ||
        user_specified_numa_id == qpl::cxl::QPL_CPU_PROXY_NUMA_ID ||
        user_specified_numa_id == qpl::cxl::QPL_RDMA_PROXY_NUMA_ID ||
        user_specified_numa_id == qpl::cxl::QPL_COMBINED_CXL_NUMA_ID ||
        user_specified_numa_id == qpl::cxl::QPL_COMBINED_CXL_UMWAIT_NUMA_ID) {
        
        auto& cxl_client = qpl::ml::dispatcher::CxlClient::get_instance();
        if (!cxl_client.is_initialized()) {
            std::cerr << "[QPL CXL] Client not initialized in submission" << std::endl;
            return HW_ACCELERATOR_WORK_QUEUES_NOT_AVAILABLE;
        }

        // Determine if this specific submission should go to the remote or local IAA.
        bool is_remote = true;
        if (user_specified_numa_id == qpl::cxl::QPL_LOCAL_PROXY_NUMA_ID || 
            user_specified_numa_id == qpl::cxl::QPL_LOCAL_PROXY_UMWAIT_NUMA_ID) {
            is_remote = false;
        } else if (user_specified_numa_id == qpl::cxl::QPL_COMBINED_CXL_NUMA_ID ||
                   user_specified_numa_id == qpl::cxl::QPL_COMBINED_CXL_UMWAIT_NUMA_ID) {
            static thread_local uint32_t combined_rr_counter = 0;
            is_remote = (combined_rr_counter++ % 2 != 0);
        }
        // CPU, RDMA, and CXL modes are always is_remote = true.

        auto* state_ptr = reinterpret_cast<qpl_hw_state*>(desc_ptr);

        // Get a completion slot from the appropriate page (0-63 local, 64-127 remote)
        int slot = cxl_client.get_comp_slot(is_remote);
        if (slot < 0) {
            std::cerr << "[QPL CXL] No free completion slots for " << (is_remote ? "remote" : "local") << " path" << std::endl;
            return HW_ACCELERATOR_WQ_IS_BUSY;
        }
        state_ptr->rdma_slot_id = slot;

        uint64_t comp_iova = cxl_client.get_comp_iova(slot);
        desc->completion_record_ptr = reinterpret_cast<uint8_t*>(comp_iova);

        // Helper: look up IOVA for the target context
        auto resolve_iova = [&](uint8_t* ptr, uint32_t size) -> uint64_t {
            if (!ptr || size == 0) return 0;
            uint64_t iova = is_remote ? cxl_client.get_remote_iova(ptr) : cxl_client.get_local_iova(ptr);
            if (!iova) {
                std::cerr << "[QPL CXL] FATAL WARNING: Buffer bypassed global mempool and hit hot-path registration! ptr=" << (void*)ptr << " size=" << size << std::endl;
                if (!cxl_client.register_buffer(ptr, size, nullptr)) return 0;
                iova = is_remote ? cxl_client.get_remote_iova(ptr) : cxl_client.get_local_iova(ptr);
            }
            return iova;
        };

        uint8_t* orig_src1 = desc->src1_ptr;
        uint8_t* orig_src2 = desc->src2_ptr;
        uint8_t* orig_dst  = desc->dst_ptr;
        uint8_t* orig_comp = desc->completion_record_ptr;

        // Patch descriptor pointers to the correct IOVAs
        if (desc->src1_ptr && desc->src1_size > 0) {
            uint64_t iova = resolve_iova(desc->src1_ptr, desc->src1_size);
            if (!iova) {
                cxl_client.release_comp_slot(slot); return HW_ACCELERATOR_WORK_QUEUES_NOT_AVAILABLE;
            }
            desc->src1_ptr = reinterpret_cast<uint8_t*>(iova);
        }

        if (desc->src2_ptr && desc->src2_size > 0) {
            uint64_t iova = resolve_iova(desc->src2_ptr, desc->src2_size);
            if (!iova) {
                desc->src1_ptr = orig_src1;
                cxl_client.release_comp_slot(slot); return HW_ACCELERATOR_WORK_QUEUES_NOT_AVAILABLE;
            }
            desc->src2_ptr = reinterpret_cast<uint8_t*>(iova);
        }

        if (desc->dst_ptr && desc->max_dst_size > 0) {
            uint64_t iova = resolve_iova(desc->dst_ptr, desc->max_dst_size);
            if (!iova) {
                desc->src1_ptr = orig_src1; desc->src2_ptr = orig_src2;
                cxl_client.release_comp_slot(slot); return HW_ACCELERATOR_WORK_QUEUES_NOT_AVAILABLE;
            }
            desc->dst_ptr = reinterpret_cast<uint8_t*>(iova);
        }

        // --- CACHE FLUSHING FOR REMOTE PATHS ---
        // We must flush/invalidate caches for all remote-destined buffers 
        // to ensure the remote IAA hardware sees the latest data and 
        // that our CPU doesn't see stale data in the destination buffer.
        if (is_remote) {
            // printf("[QPL CXL] Flushing buffers: src1=%p size=%u, src2=%p size=%u, dst=%p size=%u\n", 
            auto flush_buffer = [](uint8_t* ptr, uint32_t size) {
                if (!ptr || size == 0) return;
                uintptr_t start = reinterpret_cast<uintptr_t>(ptr) & ~63ULL;
                uintptr_t end = (reinterpret_cast<uintptr_t>(ptr) + size + 63) & ~63ULL;
                for (uintptr_t addr = start; addr < end; addr += 64) {
                    _mm_clflushopt(reinterpret_cast<void*>(addr));
                }
            };

            flush_buffer(orig_src1, desc->src1_size);
            // If src2 is the compression AECS (ccfg), flush both elements of the ccfg array (11328 bytes)
            // since the hardware toggle can select ccfg[1] at offset 5664, beyond the single active size.
            uint32_t src2_flush_size = desc->src2_size;
            if (orig_src2 && (desc->src2_size == 0x620 || desc->src2_size == 0x620 + 4096)) {
                src2_flush_size = 2 * (0x620 + 4096);
            }
            flush_buffer(orig_src2, src2_flush_size);
            flush_buffer(orig_dst, desc->max_dst_size);
            void* comp_slot_ptr = cxl_client.get_comp_ptr(slot);
            if (comp_slot_ptr) {
                std::memset(comp_slot_ptr, 0, 64);
                _mm_clflushopt(comp_slot_ptr);
            }
            _mm_mfence();
        }

        // --- Actual Submission ---
        if (user_specified_numa_id == qpl::cxl::QPL_RDMA_PROXY_NUMA_ID) {
            if (!cxl_client.setup_rdma_proxy(18516) || cxl_client.rdma_clear_completion(slot) != 0 ||
                cxl_client.rdma_write_descriptor(desc_ptr) < 0) {
                desc->src1_ptr = orig_src1; desc->src2_ptr = orig_src2; desc->dst_ptr = orig_dst;
                cxl_client.release_comp_slot(slot); return HW_ACCELERATOR_WORK_QUEUES_NOT_AVAILABLE;
            }
        } else if (user_specified_numa_id == qpl::cxl::QPL_CPU_PROXY_NUMA_ID) {
            if (!cxl_client.setup_cpu_proxy() || cxl_client.submit_to_cpud(desc_ptr) != 0) {
                desc->src1_ptr = orig_src1; desc->src2_ptr = orig_src2; desc->dst_ptr = orig_dst;
                cxl_client.release_comp_slot(slot); return HW_ACCELERATOR_WORK_QUEUES_NOT_AVAILABLE;
            }
        } else {
            if (is_remote) {
                if (!cxl_client.setup_cxl_proxy(true)) {
                    std::cerr << "[QPL CXL] Failed to setup REMOTE CXL proxy" << std::endl;
                }
            } else {
                if (!cxl_client.setup_cxl_proxy(false)) {
                    std::cerr << "[QPL CXL] Failed to setup LOCAL CXL proxy" << std::endl;
                }
                if (!cxl_client.get_local_portal()) {
                    cxl_client.map_local_portal(1, 0);
                }
            }
            void* portal = is_remote ? cxl_client.get_proxy_portal() : cxl_client.get_local_portal();

            if (!portal) {
                std::cerr << "[QPL CXL] Portal is null for " << (is_remote ? "remote" : "local") << " path" << std::endl;
                desc->src1_ptr = orig_src1; desc->src2_ptr = orig_src2; desc->dst_ptr = orig_dst;
                cxl_client.release_comp_slot(slot); return HW_ACCELERATOR_WORK_QUEUES_NOT_AVAILABLE;
            }
            _movdir64b(portal, desc_ptr);
        }

        // Restore original VA pointers for job state consistency
        desc->src1_ptr = orig_src1; 
        desc->src2_ptr = orig_src2; 
        desc->dst_ptr = orig_dst;
        desc->completion_record_ptr = orig_comp;
        return HW_ACCELERATOR_STATUS_OK;
    }
    // --- END CXL PROXY SUBMISSION LOGIC ---

    hw_accelerator_status result = HW_ACCELERATOR_WORK_QUEUES_NOT_AVAILABLE;

#if defined(__linux__)
    static auto&                      dispatcher   = qpl::ml::dispatcher::hw_dispatcher::get_instance();
    static const auto                 device_count = dispatcher.device_count();
    static thread_local std::uint32_t device_idx   = 0;

    if (device_count == 0) { return HW_ACCELERATOR_WORK_QUEUES_NOT_AVAILABLE; }

    for (uint64_t try_count = 0U; try_count < device_count; ++try_count) {
        const auto& device = dispatcher.device(device_idx);
        device_idx         = (device_idx + 1) % device_count;

        if (!device.is_matching_user_numa_policy(user_specified_numa_id)) { continue; }

        hw_iaa_descriptor_hint_cpu_cache_as_destination((hw_descriptor*)desc_ptr, device.get_cache_write_available());

        const hw_accelerator_status enqueue_result = device.enqueue_descriptor(desc_ptr, record);
        // if we successfully submitted return OK immediately
        if (enqueue_result == HW_ACCELERATOR_STATUS_OK) {
#if QPL_EXPERIMENTAL_LOG_IAA
            qpl::ml::util::record_device_idx(record, device_idx);
#endif
            return HW_ACCELERATOR_STATUS_OK;
        }
        if (hw_status_to_priority.at(enqueue_result) < hw_status_to_priority.at(result)) { result = enqueue_result; }
    }
#else
    // Not supported on Windows yet
    return HW_ACCELERATOR_SUPPORT_ERR;
#endif

    return result;
}
