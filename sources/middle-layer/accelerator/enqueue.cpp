/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @date 03/23/2020
 * @brief Internal HW API functions for @ref hw_enqueue_descriptor API implementation
 */

#include <unordered_map>
#include <cstdlib> // For getenv
#include <cstring> // For memcpy
#include <new>     // For std::nothrow
#include <iostream>
#include <ostream>

#include "dispatcher/hw_dispatcher.hpp"
#include "rdma_client.hpp" // RDMA Client
#include "rdma_protocol.hpp" // For QPL_RDMA_REMOTE_NUMA_ID
#include "hw_definitions.h"
#include "hw_descriptors_api.h"
#include "util/hw_timing_util.hpp"

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
    if (user_specified_numa_id == qpl::rdma::QPL_RDMA_REMOTE_NUMA_ID) {
        static bool rdma_client_initialized = false;
        static qpl::ml::dispatcher::RdmaClient* rdma_client_instance = nullptr;

        if (!rdma_client_initialized) {
            if (const char* env_ip = std::getenv("QPL_RDMA_SERVER_IP")) {
                rdma_client_instance = &qpl::ml::dispatcher::RdmaClient::get_instance();
                if (!rdma_client_instance->initialize(env_ip)) {
                    std::cerr << "[QPL] Failed to initialize RDMA client to " << env_ip << std::endl;
                    return HW_ACCELERATOR_WORK_QUEUES_NOT_AVAILABLE;
                }
                rdma_client_initialized = true;
            } else {
                std::cerr << "[QPL] QPL_RDMA_REMOTE_NUMA_ID used but QPL_RDMA_SERVER_IP not set." << std::endl;
                return HW_ACCELERATOR_WORK_QUEUES_NOT_AVAILABLE;
            }
        }
        
        // Ensure client is initialized before proceeding with RDMA operations
        if (!rdma_client_instance || !rdma_client_instance->is_initialized()) {
            return HW_ACCELERATOR_WORK_QUEUES_NOT_AVAILABLE;
        }

        auto& client = *rdma_client_instance;
        int slot_id = client.get_job_slot();
        
        if (slot_id < 0) {
            std::cerr << "[QPL] No free RDMA job slots available." << std::endl;
            return HW_ACCELERATOR_WQ_IS_BUSY; // Indicates temporary busyness
        }

        auto* desc = reinterpret_cast<hw_decompress_analytics_descriptor*>(desc_ptr); // Common descriptor fields
        auto* comp_local = reinterpret_cast<hw_completion_record*>(desc->completion_record_ptr);

        // 1. RDMA Write Input Data (Src1)
        // Check desc->src1_ptr and desc->src1_size
        if (desc->src1_ptr && desc->src1_size > 0) {
            uint64_t remote_src1 = client.get_remote_data_block_addr(slot_id, 0);
            if (!client.rdma_write(desc->src1_ptr, desc->src1_size, remote_src1, client.get_remote_data_block_rkey())) {
                std::cerr << "[QPL] Failed RDMA write for Src1." << std::endl;
                client.release_job_slot(slot_id);
                return HW_ACCELERATOR_WQ_IS_BUSY;
            }
        }

        // 2. RDMA Write Input Data (Src2 / AECS)
        // Check desc->src2_ptr and desc->src2_size
        if (desc->src2_ptr && desc->src2_size > 0) {
             uint64_t remote_src2 = client.get_remote_data_block_addr(slot_id, 1);
             if (!client.rdma_write(desc->src2_ptr, desc->src2_size, remote_src2, client.get_remote_data_block_rkey())) {
                std::cerr << "[QPL] Failed RDMA write for Src2." << std::endl;
                client.release_job_slot(slot_id);
                return HW_ACCELERATOR_WQ_IS_BUSY;
             }
        }

        // 3. Allocate persistent buffer for descriptor
        uint8_t* persistent_desc_buf = new (std::nothrow) uint8_t[64];
        if (!persistent_desc_buf) {
            client.release_job_slot(slot_id);
            return HW_ACCELERATOR_WQ_IS_BUSY;
        }
        std::memcpy(persistent_desc_buf, desc_ptr, 64);
        
        // 4. Patch Remote Descriptor with remote addresses
        auto* remote_desc = reinterpret_cast<hw_decompress_analytics_descriptor*>(persistent_desc_buf);
        if (desc->src1_ptr) remote_desc->src1_ptr = reinterpret_cast<uint8_t*>(client.get_remote_data_block_addr(slot_id, 0));
        if (desc->src2_ptr) remote_desc->src2_ptr = reinterpret_cast<uint8_t*>(client.get_remote_data_block_addr(slot_id, 1));
        if (desc->dst_ptr)  remote_desc->dst_ptr  = reinterpret_cast<uint8_t*>(client.get_remote_data_block_addr(slot_id, 2));
        remote_desc->completion_record_ptr = reinterpret_cast<uint8_t*>(client.get_remote_comp_addr(slot_id));

        // 5. RDMA Write Descriptor to Remote Portal (non-blocking)
        if (!client.rdma_write(persistent_desc_buf, 64, client.get_remote_portal_addr(), client.get_remote_portal_rkey())) {
            std::cerr << "[QPL] Failed RDMA write for descriptor to portal." << std::endl;
            delete[] persistent_desc_buf; // Clean up on failure
            client.release_job_slot(slot_id);
            return HW_ACCELERATOR_WQ_IS_BUSY;
        }
        
        // 6. Stash State in Local Completion Record Padding
        static_assert(sizeof(void*) == 8, "Expected 64-bit system for pointer storage.");
        static_assert(sizeof(int) == 4, "Expected 32-bit int.");
        
        // Use bytes[52] for slot_id (4 bytes) and bytes[56] for ptr (8 bytes)
        *reinterpret_cast<int*>(&comp_local->bytes[52]) = slot_id;
        *reinterpret_cast<uint8_t**>(&comp_local->bytes[56]) = persistent_desc_buf;
        
        std::cout << "[QPL] RDMA Job submitted with slot: " << slot_id << std::endl;
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