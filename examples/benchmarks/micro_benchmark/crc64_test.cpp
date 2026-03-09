//* [QPL_LOW_LEVEL_CRC64_EXAMPLE] */

#include <iostream>
#include <fstream>
#include <vector>
#include <memory>
#include <string>
#include <chrono>
#include <thread>

#include "qpl/qpl.h"

// Magic NUMA IDs for Remote RDMA
#define QPL_RDMA_REMOTE_NUMA_ID (-100)  // ODP mode
#define QPL_RDMA_HYBRID_NUMA_ID (-200)  // Hybrid mode
static bool use_rdma_path = false;
static int rdma_numa_id = QPL_RDMA_REMOTE_NUMA_ID; // Default to ODP mode

/**
 * @brief This example requires a command line argument to set the execution path. Valid values are `software_path`
 * and `hardware_path`.
 * In QPL, @ref qpl_path_software (`Software Path`) means that computations will be done with CPU.
 * Accelerator can be used instead of CPU. In this case, @ref qpl_path_hardware (`Hardware Path`) must be specified.
 * If there is no difference where calculations should be done, @ref qpl_path_auto (`Auto Path`) can be used to allow
 * the library to chose the path to execute. The Auto Path usage is not demonstrated by this example.
 *
 * @warning ---! Important !---
 * `Hardware Path` doesn't support all features declared for `Software Path`
 *
 */

/**
 * NOTE : Maximum transfer size per grouped_workqueues of IAA is 2097152(2MB)
 * If you want to put data larger than 2MB, you have to split the data into 2MB chunks.
 */
std::size_t chunk_size = 2097152;
// const std::size_t chunk_size = 1048576;
// const std::size_t chunk_size = 524288;
// const std::size_t chunk_size = 262144;
// const std::size_t chunk_size = 131072;
// const std::size_t chunk_size = 65536;
// const std::size_t chunk_size = 32768;
// const std::size_t chunk_size = 16384;
// const std::size_t chunk_size = 8192;
// const std::size_t chunk_size = 4096;
// const std::size_t chunk_size = 2048;
// const std::size_t chunk_size = 1024;
constexpr const uint64_t poly = 0x04C11DB700000000;

int parse_execution_path(int argc, char **argv, qpl_path_t *path_ptr, int extra_arg = 0) {
    // Get path from input argument
    if (extra_arg == 0) {
        if (argc < 2) {
            std::cout << "Missing the execution path as the first parameter. Use either hardware_path or software_path." << std::endl;
            return 1;
        }
    } else {
        if (argc < 4) {
            std::cout << "Missing the execution path as the first parameter and/or the dataset path as the second and third parameter." << std::endl;
            return 1;
        }
    }

    std::string path = argv[1];
    if (path == "hardware_path") {
        *path_ptr = qpl_path_hardware;
        std::cout << "The test will be run on the hardware path." << std::endl;
    } else if (path == "software_path") {
        *path_ptr = qpl_path_software;
        std::cout << "The test will be run on the software path." << std::endl;
    } else if (path == "rdma_path") {
        *path_ptr = qpl_path_hardware;
        use_rdma_path = true;
        rdma_numa_id = QPL_RDMA_REMOTE_NUMA_ID;
        std::cout << "The test will be run on the RDMA remote path (ODP mode)." << std::endl;
    } else if (path == "hybrid_path") {
        *path_ptr = qpl_path_hardware;
        use_rdma_path = true;
        rdma_numa_id = QPL_RDMA_HYBRID_NUMA_ID;
        std::cout << "The test will be run on the Hybrid path." << std::endl;
    } else {
        std::cout << "Unrecognized value for parameter. Use hardware_path, software_path, rdma_path, or hybrid_path." << std::endl;
        return 1;
    }

    return 0;
}

void job_execution(qpl_job *job_ptr)
{
    qpl_status status = qpl_execute_job(job_ptr);
    if (status != QPL_STS_OK) {
        std::cout << "An error " << status << " acquired during job execution." << std::endl;
    }
}

int iaa_crc64(std::string src_data_file_path, std::string dest_data_file_path, qpl_path_t execution_path, const uint32_t queue_size)
{
    // Source and output containers
    std::vector<uint8_t> whole_src_vector;
    std::vector<std::vector<uint8_t>> src_vector;
    double elapsed_time_sec = 0;

    std::cout << "[IAA CRC64]" << std::endl;
    
    // Opening source file
    std::cout << "Source file = " << src_data_file_path << std::endl;
    std::cout << "Loading source file... ";
    std::ifstream src_file;
    src_file.open(src_data_file_path, std::ifstream::in | std::ifstream::binary);
    if (!src_file) {
        std::cout << "File not found : " << src_data_file_path << std::endl;
        return 1;
    }
    std::cout << "Done" << std::endl;

    // Getting source file size
    src_file.seekg(0, std::ios::end);
    std::size_t src_file_size = static_cast<std::size_t>(src_file.tellg());
    src_file.seekg(0, std::ios::beg);

    // Job initialization
    std::vector<std::unique_ptr<uint8_t[]>> job_buffer;
    std::vector<qpl_job *>                  job;
    qpl_status                              status;
    uint32_t                                size = 0;

    // Allocation
    src_vector.resize(queue_size);
    job_buffer.resize(queue_size);
    job.resize(queue_size);

    status = qpl_get_job_size(execution_path, &size);
    if (status != QPL_STS_OK) {
        std::cout << "An error " << status << " acquired during job size getting." << std::endl;
        return 1;
    }

    for (int i = 0; i < queue_size; ++i) {
        job_buffer[i] = std::make_unique<uint8_t[]>(size);
        job[i] = reinterpret_cast<qpl_job *>(job_buffer[i].get());
        status = qpl_init_job(execution_path, job[i]);
        if (status != QPL_STS_OK) {
            std::cout << "An error " << status << " acquired during job initializing." << std::endl;
            return 1;
        }
        if (use_rdma_path) {
            job[i]->numa_id = rdma_numa_id;
        }
    }

    std::chrono::duration<int64_t, std::nano> elapsed_time_ns = std::chrono::nanoseconds::zero();
    std::size_t src_file_left = src_file_size;
    std::size_t vector_size = 0;

    whole_src_vector.resize(src_file_size);
    // Load memory 
    src_file.read(reinterpret_cast<char *>(&whole_src_vector.front()), src_file_size);
    // Closing source file
    src_file.close();
    
    // Force all pages to be faulted in before RDMA operations
    // This ensures the file data is actually in physical memory
    if (use_rdma_path) {
        std::cout << "Prefaulting all pages and registering..." << std::flush;
        // Register buffer
        qpl_rdma_register_buffer(whole_src_vector.data(), src_file_size);
        
        volatile uint8_t sum = 0;
        const size_t page_size = 4096;
        for (size_t i = 0; i < src_file_size; i += page_size) {
            sum += whole_src_vector[i];  // Touch each page
        }
        // Touch the last byte too
        if (src_file_size > 0) {
            sum += whole_src_vector[src_file_size - 1];
        }
        (void)sum;  // Prevent compiler from optimizing away
        std::cout << " Done" << std::endl;
    }
    
    // Warmup: Execute one job before timing to ensure RDMA connection is established
    std::cout << "Warmup job... " << std::flush;
    job[0]->op           = qpl_op_crc64;
    job[0]->next_in_ptr  = whole_src_vector.data();
    job[0]->available_in = std::min(static_cast<std::size_t>(chunk_size), src_file_size);
    job[0]->crc64_poly   = poly;
    qpl_status warmup_status = qpl_execute_job(job[0]);
    if (warmup_status != QPL_STS_OK) {
        std::cout << "Warmup failed: " << warmup_status << std::endl;
        return 1;
    }
    std::cout << "Done (CRC=" << job[0]->crc64 << ")" << std::endl;

    std::size_t current_idx = 0;

    std::chrono::duration<int64_t, std::nano> whole_elapsed_time_ns = std::chrono::nanoseconds::zero();
    auto whole_start = std::chrono::steady_clock::now();

    // CRC64
    // Pipelining Strategy:
    // 1. Fill pipeline: Submit 'queue_size' jobs
    // 2. Steady state: Wait for oldest job, submit new job
    // 3. Drain pipeline: Wait for remaining jobs

    // Circular buffer index for jobs
    int job_idx = 0; 
    int jobs_in_flight = 0;

    auto start_time = std::chrono::steady_clock::now();

    // Loop until all source data is processed
    while (src_file_left > 0 || jobs_in_flight > 0) {
        
        // --- SUBMIT PHASE ---
        // Submit jobs while we have data and pipeline capacity
        while (src_file_left > 0 && jobs_in_flight < queue_size) {
            // Calculate chunk size
            if (src_file_left <= chunk_size) {
                vector_size = src_file_left;
            } else {
                vector_size = chunk_size;
            }

            // Setup job
            job[job_idx]->op           = qpl_op_crc64;
            job[job_idx]->next_in_ptr  = whole_src_vector.data() + current_idx;
            job[job_idx]->available_in = static_cast<uint32_t>(vector_size);
            job[job_idx]->crc64_poly   = poly;

            current_idx   += vector_size;
            src_file_left -= vector_size;

            if (execution_path == qpl_path_software) {
                qpl_execute_job(job[job_idx]); // Sync execute for SW path
            } else {
                status = qpl_submit_job(job[job_idx]);
                if (status != QPL_STS_OK) {
                    std::cout << "An error " << status << " acquired during job execution." << std::endl;
                    return 1;
                }
                jobs_in_flight++;
                // Move to next slot in circular buffer
                job_idx = (job_idx + 1) % queue_size;
            }
        }

        // --- WAIT PHASE ---
        // Check for completions if pipeline is full or no more data to submit
        if (jobs_in_flight > 0 && (jobs_in_flight == queue_size || src_file_left == 0)) {
            // In the circular buffer, the "oldest" job is always at job_idx
            // because we incremented job_idx after submitting.
            // Wait for the oldest job to free up its slot
            
            if (execution_path != qpl_path_software) {
                status = qpl_wait_job(job[job_idx]);
                if (status != QPL_STS_OK) {
                    std::cout << "An error " << status << " acquired during job waiting." << std::endl;
                    return 1;
                }
                jobs_in_flight--;
                // Slot at job_idx is now free for next iteration's submit
            }
        }
    }

    auto end_time = std::chrono::steady_clock::now();
    elapsed_time_ns += end_time - start_time;

        // for (int i = 0; i < enqueue_cnt; ++i) {
        //     // Opening destination file
        //     std::ofstream dest_file;
        //     dest_file.open(dest_data_file_path + "." + std::to_string(iteration + i), std::ofstream::out | std::ofstream::binary);
        //     if (!dest_file) {
        //         std::cout << "File not found : " << dest_data_file_path << std::endl;
        //         return 1;
        //     }

        //     // Writing CRC64 data to destination file
        //     const std::string crc_value_str = std::to_string(job[i]->crc64);
        //     dest_file.write(crc_value_str.c_str(), sizeof(uint64_t));

        //     // Closing destination file
        //     dest_file.close();
        // }


        std::cout << '\r';
        std::cout << "Progress ... " << (src_file_size - src_file_left) << " / " << src_file_size << " Bytes" << std::flush;


    // Closing source file
    // src_file.close();

    if (use_rdma_path) {
        qpl_rdma_unregister_buffer(whole_src_vector.data());
    }

    auto whole_end = std::chrono::steady_clock::now();

    whole_elapsed_time_ns += whole_end - whole_start;

    // Freeing resources
    for (int i = 0; i < queue_size; ++i) {
        status = qpl_fini_job(job[i]);
        if (status != QPL_STS_OK) {
            std::cout << "An error " << status << " acquired during job finalization." << std::endl;
            return 1;
        }
    }

    std::cout << std::endl;
    std::cout << "CRC64 was performed successfully." << std::endl;
    std::cout << "Input size      = " << src_file_size << " Bytes" << std::endl;
    elapsed_time_sec = static_cast<double>(elapsed_time_ns.count()) / 1000 / 1000 / 1000;
    // std::cout << "Elapsed Time = " << elapsed_time_ns.count() << " ns (" << elapsed_time_sec << " s)" << std::endl;
    // double whole_elapsed_time_sec = static_cast<double>(whole_elapsed_time_ns.count()) / 1000 / 1000 / 1000;
    // std::cout << "Whole elapsed Time = " << whole_elapsed_time_ns.count() << " ns (" << whole_elapsed_time_sec << " s)" << std::endl;
    std::cout << "Bandwidth       = " << static_cast<double>(src_file_size) / 1024 / 1024 / elapsed_time_sec << " MB/s" << std::endl;

    return 0;
}

auto main(int argc, char** argv) -> int {
    std::cout << std::endl;
    std::cout << "Intel(R) Query Processing Library version is " << qpl_get_library_version() << ".\n";

    // Default to Software Path
    qpl_path_t execution_path = qpl_path_software;

    // Get path from input argument
    int parse_ret = parse_execution_path(argc, argv, &execution_path, 1);
    if (parse_ret != 0) {
        return 1;
    }

    // File path
    const std::string SRC_DATA_FILE_PATH    = argv[2];
    const std::string DEST_DATA_FILE_PATH   = SRC_DATA_FILE_PATH + ".iaa.crc64";

    const uint32_t queue_size = static_cast<uint32_t>(atoi(argv[3]));
    chunk_size = static_cast<size_t>(atoi(argv[4]));

    std::cout << "Queue Size = " << queue_size << std::endl;
    std::cout << std::endl;
    // CRC64
    if(iaa_crc64(SRC_DATA_FILE_PATH, DEST_DATA_FILE_PATH, execution_path, queue_size) != 0) {
        std::cout << "An error acquired during iaa_execution(crc64)" << std::endl;
        return 1;
    }

    return 0;
}

//* [QPL_LOW_LEVEL_CRC64_EXAMPLE] */
