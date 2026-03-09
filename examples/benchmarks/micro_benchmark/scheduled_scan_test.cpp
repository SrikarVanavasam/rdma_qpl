
//* [QPL_LOW_LEVEL_SCAN_RANGE_EXAMPLE] */

#include <iostream>
#include <fstream>
#include <vector>
#include <memory>
#include <string>
#include <chrono>
#include <thread>
#include <cmath>
#include <numeric>

#include "qpl/qpl.h"

// Magic NUMA IDs for Remote RDMA
#define QPL_RDMA_REMOTE_NUMA_ID (-100)
// Local is -1

static bool use_rdma_path = false;

// Configuration
std::size_t chunk_size = 2 * 1024 * 1024; // Default 2MB
constexpr const uint32_t input_vector_width     = 8;
constexpr const uint32_t lower_boundary         = 65; //'A'
constexpr const uint32_t upper_boundary         = 66; //'B'

int parse_execution_path(int argc, char **argv, qpl_path_t *path_ptr) {
    if (argc < 4) {
        std::cout << "Usage: " << argv[0] << " <execution_path> <dataset_path> <queue_size> [chunk_size]" << std::endl;
        return 1;
    }

    std::string path = argv[1];
    if (path == "global_hybrid_path" || path == "hybrid_path") {
        *path_ptr = qpl_path_hardware;
        use_rdma_path = true;
        std::cout << "The test will be run on the Scheduled Hybrid path." << std::endl;
    } else {
        std::cout << "This benchmark is designed for 'hybrid_path' only." << std::endl;
        return 1;
    }
    return 0;
}

// Simple CRC warmup to ensure RDMA connection is established
int do_warmup_job(qpl_path_t execution_path) {
    std::cout << "Warmup job... " << std::flush;
    
    uint32_t job_size = 0;
    qpl_get_job_size(execution_path, &job_size);
    
    std::vector<uint8_t> job_buffer(job_size);
    qpl_job* job = reinterpret_cast<qpl_job*>(job_buffer.data());
    qpl_init_job(execution_path, job);
    
    // Warmup Remote
    job->numa_id = QPL_RDMA_REMOTE_NUMA_ID;
    
    std::vector<uint8_t> warmup_data(1024, 0xAA);
    qpl_rdma_register_buffer(warmup_data.data(), warmup_data.size());
    
    job->op           = qpl_op_crc64;
    job->next_in_ptr  = warmup_data.data();
    job->available_in = static_cast<uint32_t>(warmup_data.size());
    job->crc64_poly   = 0x42F0E1EBA9EA3693ULL;
    
    qpl_status status = qpl_execute_job(job);
    qpl_fini_job(job);
    qpl_rdma_unregister_buffer(warmup_data.data());
    
    if (status != QPL_STS_OK) {
        std::cout << "Failed (" << status << ")" << std::endl;
        return status;
    }
    std::cout << "Done" << std::endl;
    return 0;
}

int iaa_scheduled_scan(std::string src_data_file_path, qpl_path_t execution_path, const uint32_t queue_size)
{
    // Load System Constants
    double latency_us = 0.0;
    double bw_mbps = 0.0;
    
    if (const char* env_lat = std::getenv("QPL_RDMA_LATENCY_US")) {
        latency_us = std::stof(env_lat);
    } else {
        std::cout << "Warning: QPL_RDMA_LATENCY_US not set. Using default 0." << std::endl;
    }

    if (const char* env_bw = std::getenv("QPL_RDMA_BW_MBPS")) {
        bw_mbps = std::stof(env_bw);
    } else {
        std::cout << "Warning: QPL_RDMA_BW_MBPS not set. Using default 1000." << std::endl;
        bw_mbps = 1000.0;
    }
    
    // Calculate N_remote (number of bytes to send to remote)
    // Formula: N_remote = Total / 2 - (Latency * BW) / 2
    // Product (Latency * BW) needs units adjustment:
    // Latency (us) * BW (MB/s) = (us * 10^-6 s/us) * (MB * 10^6 B/MB / s) = B
    double product_bytes = latency_us * bw_mbps; 
    
    // This calculation is per-batch or total?
    // User wants to schedule *jobs* in a batch. Each job is `chunk_size`.
    // Total Bytes in Batch = queue_size * chunk_size.
    // Optimal Remote Bytes = (Total Bytes / 2) - (product_bytes / 2)
    
    // Source File
    std::cout << "Source file = " << src_data_file_path << std::endl;
    std::ifstream src_file(src_data_file_path, std::ifstream::binary | std::ifstream::ate);
    if (!src_file) {
        std::cout << "File not found." << std::endl;
        return 1;
    }
    std::size_t src_file_size = src_file.tellg();
    src_file.seekg(0, std::ios::beg);

    std::vector<uint8_t> whole_src_vector(src_file_size);
    src_file.read(reinterpret_cast<char*>(whole_src_vector.data()), src_file_size);
    src_file.close();

    // Register Source
    qpl_rdma_register_buffer(whole_src_vector.data(), src_file_size);

    // Job Init
    uint32_t job_size = 0;
    qpl_get_job_size(execution_path, &job_size);
    
    std::vector<std::vector<uint8_t>> job_buffers(queue_size);
    std::vector<qpl_job*> jobs(queue_size);
    std::vector<std::vector<uint8_t>> dest_vectors(queue_size);

    for(int i=0; i<queue_size; ++i) {
        job_buffers[i].resize(job_size);
        jobs[i] = reinterpret_cast<qpl_job*>(job_buffers[i].data());
        qpl_init_job(execution_path, jobs[i]);
        
        dest_vectors[i].resize(chunk_size);
        qpl_rdma_register_buffer(dest_vectors[i].data(), chunk_size);
    }

    std::size_t src_offset = 0;
    std::size_t remaining_bytes = src_file_size;
    std::chrono::duration<double, std::nano> total_time_ns = std::chrono::nanoseconds::zero();

    // Loop
    while (remaining_bytes > 0) {
        int batch_count = 0;
        size_t batch_bytes = 0;
        
        // Prepare Batch
        for (int i=0; i<queue_size && remaining_bytes > 0; ++i) {
            size_t size = std::min(remaining_bytes, chunk_size);
            
            jobs[i]->op = qpl_op_scan_range;
            jobs[i]->next_in_ptr = whole_src_vector.data() + src_offset;
            jobs[i]->available_in = size;
            jobs[i]->next_out_ptr = dest_vectors[i].data();
            jobs[i]->available_out = chunk_size;
            jobs[i]->num_input_elements = size;
            jobs[i]->src1_bit_width = 8;
            jobs[i]->out_bit_width = qpl_ow_32;
            jobs[i]->param_low = lower_boundary;
            jobs[i]->param_high = upper_boundary;

            src_offset += size;
            remaining_bytes -= size;
            batch_bytes += size;
            batch_count++;
        }
        
        // Schedule
        // Target Remote Bytes per batch
        double optimal_remote_bytes = (double)batch_bytes / 2.0 - product_bytes / 2.0;
        if (optimal_remote_bytes < 0) optimal_remote_bytes = 0;
        
        int remote_jobs_count = (int)(optimal_remote_bytes / chunk_size); 
        // Note: integer division floor. It's safer to round down remote jobs.
        
        // Limit to batch count
        if (remote_jobs_count > batch_count) remote_jobs_count = batch_count;
        
        // Print schedule once
        static bool first_print = true;
        if (first_print) {
             std::cout << "[Schedule] Batch Bytes: " << batch_bytes 
                       << ", Product(LxBW): " << product_bytes
                       << ", Opt Remote Bytes: " << optimal_remote_bytes
                       << ", Remote Jobs: " << remote_jobs_count 
                       << " / " << batch_count << std::endl;
             first_print = false;
        }

        // Assign NUMA IDs - SCHEDULE REMOTE FIRST
        for (int i=0; i<batch_count; ++i) {
            if (i < remote_jobs_count) {
                jobs[i]->numa_id = QPL_RDMA_REMOTE_NUMA_ID;
            } else {
                jobs[i]->numa_id = -1; // Local
            }
        }
        
        // Submit
        auto start = std::chrono::steady_clock::now();
        for(int i=0; i<batch_count; ++i) {
            if (qpl_submit_job(jobs[i]) != QPL_STS_OK) {
                std::cerr << "Submit failed job " << i << std::endl;
                return 1;
            }
        }
        
        // Wait
        for(int i=0; i<batch_count; ++i) {
             if (qpl_wait_job(jobs[i]) != QPL_STS_OK) {
                 std::cerr << "Wait failed job " << i << std::endl;
                 return 1;
             }
        }
        auto end = std::chrono::steady_clock::now();
        total_time_ns += (end - start);
        
        std::cout << "\rRemaining: " << remaining_bytes << "      " << std::flush;
    }

    std::cout << std::endl;

    // Report
    double seconds = total_time_ns.count() / 1e9;
    double mbps = (src_file_size / 1024.0 / 1024.0) / seconds;
    
    std::cout << "Total Time: " << seconds << " s" << std::endl;
    std::cout << "Throughput: " << mbps << " MB/s" << std::endl;

    // Cleanup
    qpl_rdma_unregister_buffer(whole_src_vector.data());
    for(int i=0; i<queue_size; ++i) {
        qpl_rdma_unregister_buffer(dest_vectors[i].data());
        qpl_fini_job(jobs[i]);
    }
    
    return 0;
}

int main(int argc, char** argv) {
     qpl_path_t path;
     if (parse_execution_path(argc, argv, &path) != 0) return 1;
     
     std::string src_path = argv[2];
     uint32_t queue_size = std::stoi(argv[3]);
     if (argc > 4) chunk_size = std::stoi(argv[4]);
     
     if (do_warmup_job(path) != 0) return 1;
     
     return iaa_scheduled_scan(src_path, path, queue_size);
}
