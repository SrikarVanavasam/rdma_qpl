
#include <iostream>
#include <vector>
#include <memory>
#include <string>
#include <chrono>
#include <thread>
#include <numeric>
#include <cmath>

#include "qpl/qpl.h"

// Constants
#define QPL_RDMA_REMOTE_NUMA_ID (-100)
static bool use_rdma_path = false;
static int rdma_numa_id = QPL_RDMA_REMOTE_NUMA_ID;

// Configuration
const int WARMUP_ITERATIONS = 5;
const int LATENCY_ITERATIONS = 1000;
const int BW_ITERATIONS = 50;
const int BW_QUEUE_DEPTH = 32; // Saturate the engine
const size_t LATENCY_JOB_SIZE = 1024; // 1KB for latency
const size_t BW_JOB_SIZE = 2 * 1024 * 1024; // 2MB for BW

// Helper to check status
void check(qpl_status status, const std::string& msg) {
    if (status != QPL_STS_OK) {
        std::cerr << "Error: " << msg << " (" << status << ")" << std::endl;
        exit(1);
    }
}

// Measure Remote Latency (Sequential Small Jobs)
double measure_remote_latency(qpl_path_t path) {
    if (path != qpl_path_hardware) return 0.0;
    
    std::cout << "Measuring Remote Latency..." << std::endl;

    // init job
    uint32_t job_size = 0;
    check(qpl_get_job_size(path, &job_size), "get_job_size");
    std::vector<uint8_t> job_buffer(job_size);
    qpl_job* job = reinterpret_cast<qpl_job*>(job_buffer.data());
    check(qpl_init_job(path, job), "init_job");

    // Remote settings
    job->numa_id = QPL_RDMA_REMOTE_NUMA_ID; 

    // Data
    std::vector<uint8_t> data(LATENCY_JOB_SIZE, 0xAA);
    qpl_rdma_register_buffer(data.data(), data.size());

    // Loop
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < LATENCY_ITERATIONS; ++i) {
        job->op = qpl_op_crc64;
        job->next_in_ptr = data.data();
        job->available_in = LATENCY_JOB_SIZE;
        job->crc64_poly = 0x42F0E1EBA9EA3693ULL;
        
        check(qpl_execute_job(job), "execute_job");
    }
    auto end = std::chrono::high_resolution_clock::now();
    
    qpl_rdma_unregister_buffer(data.data());
    qpl_fini_job(job);

    std::chrono::duration<double, std::micro> duration = end - start;
    double avg_latency_us = duration.count() / LATENCY_ITERATIONS;
    
    std::cout << "  Iterations: " << LATENCY_ITERATIONS << std::endl;
    std::cout << "  Avg Latency: " << avg_latency_us << " us" << std::endl;
    return avg_latency_us;
}

// Measure Local Bandwidth (Parallel Large Jobs)
double measure_local_bw(qpl_path_t path) {
    std::cout << "Measuring Local Bandwidth..." << std::endl;
    
    // Alloc jobs
    uint32_t job_size = 0;
    check(qpl_get_job_size(path, &job_size), "get_job_size");
    
    std::vector<std::vector<uint8_t>> job_buffers(BW_QUEUE_DEPTH);
    std::vector<qpl_job*> jobs(BW_QUEUE_DEPTH);
    
    for(int i=0; i<BW_QUEUE_DEPTH; ++i) {
         job_buffers[i].resize(job_size);
         jobs[i] = reinterpret_cast<qpl_job*>(job_buffers[i].data());
         check(qpl_init_job(path, jobs[i]), "init_job");
         jobs[i]->numa_id = -1; // Local
    }

    // Data (Shared source for read, separate dests)
    std::vector<uint8_t> src(BW_JOB_SIZE, 0xBB);
    std::vector<std::vector<uint8_t>> dsts(BW_QUEUE_DEPTH);
    for(int i=0; i<BW_QUEUE_DEPTH; ++i) dsts[i].resize(BW_JOB_SIZE);

    // Warmup
    for(int k=0; k<WARMUP_ITERATIONS; ++k) {
         for(int i=0; i<BW_QUEUE_DEPTH; ++i) {
              jobs[i]->op = qpl_op_scan_eq;
              jobs[i]->next_in_ptr = src.data();
              jobs[i]->available_in = BW_JOB_SIZE;
              jobs[i]->next_out_ptr = dsts[i].data();
              jobs[i]->available_out = BW_JOB_SIZE;
              // ... params
              qpl_submit_job(jobs[i]);
         }
         for(int i=0; i<BW_QUEUE_DEPTH; ++i) qpl_wait_job(jobs[i]);
    }

    // Measure
    double total_bytes = (double)BW_ITERATIONS * BW_QUEUE_DEPTH * BW_JOB_SIZE;
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int k = 0; k < BW_ITERATIONS; ++k) {
         for(int i=0; i<BW_QUEUE_DEPTH; ++i) {
              jobs[i]->op = qpl_op_scan_eq; // Scan is memory bound typically
              jobs[i]->next_in_ptr = src.data();
              jobs[i]->available_in = BW_JOB_SIZE;
              jobs[i]->next_out_ptr = dsts[i].data();
              jobs[i]->available_out = BW_JOB_SIZE;
              jobs[i]->num_input_elements = BW_JOB_SIZE;
              jobs[i]->src1_bit_width = 8;
              jobs[i]->out_bit_width = qpl_ow_32;
              jobs[i]->param_low = 0x10;
              jobs[i]->param_high = 0x20;
              
              qpl_submit_job(jobs[i]);
         }
         for(int i=0; i<BW_QUEUE_DEPTH; ++i) {
              check(qpl_wait_job(jobs[i]), "wait_job");
         }
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    
    for(int i=0; i<BW_QUEUE_DEPTH; ++i) qpl_fini_job(jobs[i]);

    std::chrono::duration<double> duration = end - start;
    double bw_mbps = (total_bytes / 1024.0 / 1024.0) / duration.count();

    std::cout << "  Total Data: " << total_bytes / 1024 / 1024 << " MB" << std::endl;
    std::cout << "  Time: " << duration.count() << " s" << std::endl;
    std::cout << "  Throughput: " << bw_mbps << " MB/s" << std::endl;
    
    return bw_mbps;
}

int main(int argc, char** argv) {
    if (argc < 2) {
         // Auto-detect mode if no args, but we need 'rdma_path' implied?
         // Just assume we want to test both if env vars set?
         // Or simplified: usage ./mb_sys_eval_test <path> is redundant
    }
    
    std::cout << "System Evaluation Test" << std::endl;
    std::cout << "----------------------" << std::endl;

    // 1. Measure Remote Latency (Requires RDMA setup)
    // Check if IP set
    if (!std::getenv("QPL_RDMA_SERVER_IP")) {
        std::cerr << "QPL_RDMA_SERVER_IP not set. Cannot measure remote latency." << std::endl;
        return 1;
    }

    double lat = measure_remote_latency(qpl_path_hardware);
    double bw = measure_local_bw(qpl_path_hardware);

    std::cout << std::endl;
    std::cout << "RESULTS:" << std::endl;
    std::cout << "export QPL_RDMA_LATENCY_US=" << (int)lat << std::endl;
    std::cout << "export QPL_RDMA_BW_MBPS=" << (int)bw << std::endl;

    return 0;
}
