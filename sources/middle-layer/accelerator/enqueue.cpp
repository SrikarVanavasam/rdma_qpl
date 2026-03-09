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

#include "dispatcher/hw_dispatcher.hpp"
#include "hw_definitions.h"
#include "hw_descriptors_api.h"
#include "rdma_client.hpp"   // RDMA Client
#include "rdma_protocol.hpp" // For QPL_RDMA_REMOTE_NUMA_ID
#include "util/hw_timing_util.hpp"
#include "hardware_state.h"  // For qpl_hw_state and rdma_slot_id

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

extern "C" hw_accelerator_status hw_enqueue_descriptor(void* desc_ptr, int32_t user_specified_numa_id,
                                                       qpl::ml::util::execution_record_ext_t* record) {
    /* DEBUG: Descriptor dump
    static int call_count = 0;
    call_count++;
    std::cout << "[QPL] hw_enqueue_descriptor called with numa_id: " << user_specified_numa_id << " (Call #"
              << call_count << ")" << std::endl;

    auto* original_desc_content = reinterpret_cast<hw_decompress_analytics_descriptor*>(desc_ptr);
    std::cout << "[QPL] Original Descriptor Content (pre-patch):" << std::endl;
    std::cout << "  Trusted Fields: 0x" << std::hex << (int)original_desc_content->trusted_fields << std::dec
              << std::endl;
    std::cout << "  Opcode: 0x" << std::hex << (int)original_desc_content->op_code_op_flags << std::dec << std::endl;
    std::cout << "  Comp Rec Ptr: 0x" << std::hex << (uintptr_t)original_desc_content->completion_record_ptr << std::dec
              << std::endl;
    std::cout << "  Src1 Ptr: 0x" << std::hex << (uintptr_t)original_desc_content->src1_ptr << std::dec << std::endl;
    std::cout << "  Src1 Size: " << original_desc_content->src1_size << std::endl;
    std::cout << "  Dst Ptr: 0x" << std::hex << (uintptr_t)original_desc_content->dst_ptr << std::dec << std::endl;
    std::cout << "  Max Dst Size: " << original_desc_content->max_dst_size << std::endl;
    if (original_desc_content->src2_ptr) {
        std::cout << "  Src2 Ptr: 0x" << std::hex << (uintptr_t)original_desc_content->src2_ptr << std::dec
                  << std::endl;
        std::cout << "  Src2 Size: " << original_desc_content->src2_size << std::endl;
    }
    if (original_desc_content->decomp_flags == 0) {
        auto* compress_desc = reinterpret_cast<hw_compress_descriptor*>(desc_ptr);
        std::cout << "  Compression Flags: 0x" << std::hex << (int)compress_desc->compression_flags << std::dec
                  << std::endl;
        std::cout << "  Compression 2 Flags: 0x" << std::hex << (int)compress_desc->compression_2_flags << std::dec
                  << std::endl;
    } else {
        std::cout << "  Decomp Flags: 0x" << std::hex << (int)original_desc_content->decomp_flags << std::dec
                  << std::endl;
    }
    if (original_desc_content->src2_ptr && original_desc_content->src2_size > 0) {
        std::cout << "  Src2 (AECS) Data Preview (First 64 bytes):" << std::endl;
        for (uint32_t i = 0; i < 64 && i < original_desc_content->src2_size; ++i) {
            std::cout << std::hex << (int)original_desc_content->src2_ptr[i] << (i % 16 == 15 ? "\n" : " ");
        }
        std::cout << std::dec << std::endl;
        bool has_nonzero = false;
        for (uint32_t i = 0; i < original_desc_content->src2_size; ++i) {
            if (original_desc_content->src2_ptr[i] != 0) {
                has_nonzero = true;
                std::cout << "  First non-zero byte at offset " << i << ": 0x" << std::hex
                          << (int)original_desc_content->src2_ptr[i] << std::dec << std::endl;
                break;
            }
        }
        if (!has_nonzero) {
            std::cout << "  WARNING: Src2 (AECS) is entirely zeros!" << std::endl;
        }
    }
    std::cout << "------------------------------------------" << std::endl;
    END DEBUG */

    bool is_remote_target = (user_specified_numa_id == qpl::rdma::QPL_RDMA_REMOTE_NUMA_ID);

    if (user_specified_numa_id == qpl::rdma::QPL_RDMA_HYBRID_NUMA_ID) {
        static thread_local uint32_t rr_counter = 0;
        // Round Robin: Odd -> Remote, Even -> Local
        if ((rr_counter++ % 2) != 0) {
            is_remote_target = true;
        } else {
            user_specified_numa_id = QPL_DEVICE_NUMA_ID_ANY;
        }
    }

    if (is_remote_target) {
        static bool                             rdma_client_initialized = false;
        static qpl::ml::dispatcher::RdmaClient* rdma_client_instance    = nullptr;

        if (!rdma_client_initialized) {
            if (const char* env_ip = std::getenv("QPL_RDMA_SERVER_IP")) {
                rdma_client_instance = &qpl::ml::dispatcher::RdmaClient::get_instance();
                if (!rdma_client_instance->initialize(env_ip)) {
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
        auto* desc   = reinterpret_cast<hw_decompress_analytics_descriptor*>(desc_ptr);
        auto* comp_local = reinterpret_cast<hw_completion_record*>(desc->completion_record_ptr);
        auto* state_ptr = reinterpret_cast<qpl_hw_state*>(
            reinterpret_cast<char*>(comp_local) - offsetof(qpl_hw_state, comp_ptr));

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

        // --- STAGING MODE (Unified) ---
        void* data_stg = client.get_data_staging(slot_id);
        // desc_stg and comp_stg no longer needed for short writes
        
        if (!data_stg) {
            if (!reusing_slot) { client.release_job_slot(slot_id); state_ptr->rdma_slot_id = -1; }
            return HW_ACCELERATOR_WQ_IS_BUSY;
        }

        // Copy source data to staging (always send - content can change even with same pointer)
        if (desc->src1_ptr && desc->src1_size > 0) {
            uint64_t remote_src1 = client.get_remote_data_block_addr(slot_id, 0);
            
            if (client.is_registered(desc->src1_ptr, desc->src1_size)) {
                // Zero-Copy Path: Send directly from User Buffer to Server Staging
                client.write(desc->src1_ptr, desc->src1_size, remote_src1, client.get_remote_data_block_rkey(), false);
            } else {
                // Staging Path: Copy to Bounce Buffer -> Send
                std::memcpy(data_stg, desc->src1_ptr, desc->src1_size);
                client.write(data_stg, desc->src1_size, remote_src1, client.get_remote_data_block_rkey(), false);
            }
        }

        // Copy src2 (AECS/Huffman tables for compression) to staging
        if (desc->src2_ptr && desc->src2_size > 0) {
            // src2 goes to block 1 in remote data area
            uint64_t remote_src2 = client.get_remote_data_block_addr(slot_id, 1);
            
            if (client.is_registered(desc->src2_ptr, desc->src2_size)) {
                 client.write(desc->src2_ptr, desc->src2_size, remote_src2, client.get_remote_data_block_rkey(), false);
            } else {
                 uint8_t* src2_stg = static_cast<uint8_t*>(data_stg) + qpl::rdma::BLOCK_SIZE;
                 std::memcpy(src2_stg, desc->src2_ptr, desc->src2_size);
                 client.write(src2_stg, desc->src2_size, remote_src2, client.get_remote_data_block_rkey(), false);
            }
        }

        // Prepare descriptor on stack for inline send
        alignas(64) uint8_t desc_buf[64];
        std::memcpy(desc_buf, desc_ptr, 64);
        auto* remote_desc = reinterpret_cast<hw_decompress_analytics_descriptor*>(desc_buf);
        
        if (desc->src1_ptr)
            remote_desc->src1_ptr = reinterpret_cast<uint8_t*>(client.get_remote_data_block_addr(slot_id, 0));
        if (desc->src2_ptr)
            remote_desc->src2_ptr = reinterpret_cast<uint8_t*>(client.get_remote_data_block_addr(slot_id, 1));
        if (desc->dst_ptr)
            remote_desc->dst_ptr = reinterpret_cast<uint8_t*>(client.get_remote_data_block_addr(slot_id, 2));
        remote_desc->completion_record_ptr = reinterpret_cast<uint8_t*>(client.get_remote_comp_addr(slot_id));

        // Zero out remote completion record using inline write
        uint8_t zero_comp[64] = {0};
        if (!client.write_short(zero_comp, 64, client.get_remote_comp_addr(slot_id), client.get_remote_comp_rkey())) {
             std::cerr << "[Enqueue] Failed to zero completion record!" << std::endl;
             return HW_ACCELERATOR_WQ_IS_BUSY;
        }
        
        // Send descriptor to portal (round-robin across WQs) using inline write
        uint32_t wq_idx = client.get_next_wq_index();
        if (!client.write_short(desc_buf, 64, client.get_remote_portal_addr(wq_idx), client.get_remote_portal_rkey(wq_idx))) {
             std::cerr << "[Enqueue] Failed to submit descriptor (write_short)!" << std::endl;
             // Should we retry or fail? Fail for now.
             return HW_ACCELERATOR_WQ_IS_BUSY;
        }

        return HW_ACCELERATOR_STATUS_OK;
    }
    // --- END RDMA SUBMISSION LOGIC ---

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
