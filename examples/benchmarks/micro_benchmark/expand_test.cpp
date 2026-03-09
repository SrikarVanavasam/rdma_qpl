//* [QPL_LOW_LEVEL_Expand_EXAMPLE] */

#include <iostream>
#include <fstream>
#include <vector>
#include <memory>
#include <string>
#include <chrono>
#include <thread>
#include <cmath>

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
constexpr const uint32_t input_vector_width = 8;
constexpr const uint32_t output_vector_width = 1;
uint32_t mask_size           = 2*1024*1024;
uint32_t mask_byte_length = mask_size / 8;

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
        std::cout << "The test will be run on the RDMA remote path (Zero-Copy)." << std::endl;
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

// Simple CRC warmup to ensure RDMA connection is established
int do_warmup_job(qpl_path_t execution_path) {
    if (execution_path == qpl_path_software) return 0;
    
    std::cout << "Warmup job... " << std::flush;
    
    uint32_t job_size = 0;
    qpl_status status = qpl_get_job_size(execution_path, &job_size);
    if (status != QPL_STS_OK) return status;
    
    std::vector<uint8_t> job_buffer(job_size);
    qpl_job* job = reinterpret_cast<qpl_job*>(job_buffer.data());
    status = qpl_init_job(execution_path, job);
    if (status != QPL_STS_OK) return status;
    
    if (use_rdma_path) {
        job->numa_id = rdma_numa_id;
    }
    
    std::vector<uint8_t> warmup_data(1024, 0xAA);
    
    if (use_rdma_path) {
        if (qpl_rdma_register_buffer(warmup_data.data(), warmup_data.size()) != QPL_STS_OK) {
             std::cout << "Warmup registration failed" << std::endl;
             return QPL_STS_LIBRARY_INTERNAL_ERR;
        }
    }
    job->op           = qpl_op_crc64;
    job->next_in_ptr  = warmup_data.data();
    job->available_in = static_cast<uint32_t>(warmup_data.size());
    job->crc64_poly   = 0x42F0E1EBA9EA3693ULL;
    
    status = qpl_execute_job(job);
    qpl_fini_job(job);
    
    if (use_rdma_path) {
        qpl_rdma_unregister_buffer(warmup_data.data());
    }
    
    if (status != QPL_STS_OK) {
        std::cout << "Failed (" << status << ")" << std::endl;
        return status;
    }
    std::cout << "Done" << std::endl;
    return 0;
}

int iaa_expand(std::string src_data_file_path, std::string dest_data_file_path, qpl_path_t execution_path, uint32_t &iteration, const uint32_t queue_size)
{
    // Source and output containers
    std::vector<std::vector<uint8_t>> src_vector;
    std::vector<std::vector<uint8_t>> dest_vector;
    std::vector<std::vector<uint8_t>> mask_vector;
    std::vector<uint8_t> whole_src_vector;
    double elapsed_time_sec = 0;

    std::cout << "[IAA Expand]" << std::endl;
    
    // Opening source file
    std::cout << "Source file = " << src_data_file_path << std::endl;
    std::cout << "Loading source file... ";
    std::ifstream src_file;
    src_file.open(src_data_file_path, std::ifstream::in | std::ifstream::binary);
    if (!src_file) {
        std::cout << "File not found : " << src_data_file_path << std::endl;
        return 1;
    }

    // Getting source file size
    src_file.seekg(0, std::ios::end);
    std::size_t src_file_size = static_cast<std::size_t>(src_file.tellg());
    src_file.seekg(0, std::ios::beg);

    // Pre-load source file into memory
    whole_src_vector.resize(src_file_size);
    src_file.read(reinterpret_cast<char*>(whole_src_vector.data()), src_file_size);
    src_file.close();
    std::cout << "Done" << std::endl;

    // Job initialization
    std::vector<std::unique_ptr<uint8_t[]>> job_buffer;
    std::vector<qpl_job *>                  job;
    qpl_status                              status;
    uint32_t                                size = 0;

    // Allocation
    src_vector.resize(queue_size);
    dest_vector.resize(queue_size);
    mask_vector.resize(queue_size);
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

    // Initialize buffers for all jobs
    for(int i = 0; i < queue_size; i++) {
        src_vector[i].resize(chunk_size);
        dest_vector[i].resize(mask_size);
        mask_vector[i].resize(mask_byte_length, 0);
    }
    
    if (use_rdma_path) {
        qpl_rdma_register_buffer(whole_src_vector.data(), src_file_size);
        for(int i = 0; i < queue_size; i++) {
            qpl_rdma_register_buffer(src_vector[i].data(), chunk_size);
            qpl_rdma_register_buffer(dest_vector[i].data(), mask_size);
            qpl_rdma_register_buffer(mask_vector[i].data(), mask_byte_length);
        }
    }

    std::chrono::duration<int64_t, std::nano> elapsed_time_ns = std::chrono::nanoseconds::zero();
    std::size_t src_file_left = src_file_size;
    std::size_t vector_size = 0;
    iteration = 0;
    std::size_t current_idx = 0;

    auto whole_start = std::chrono::steady_clock::now();

    // Pipelined Execution
    int current_job_idx = 0;
    int jobs_in_flight = 0;
    int oldest_job_idx = 0;
    
    // Fill the pipeline and process
    while (src_file_left > 0 || jobs_in_flight > 0) {
        
        // Submit jobs while we have data and queue space
        while (src_file_left > 0 && jobs_in_flight < queue_size) {
            
            // Prepare job
            if (src_file_left <= chunk_size / 2) {
                vector_size = src_file_left;
            } else {
                vector_size = chunk_size / 2;
            }

            int full_bytes = vector_size / 8;
            int remaining_bits = vector_size % 8;
            
            // Reset mask
            std::fill(mask_vector[current_job_idx].begin(), mask_vector[current_job_idx].end(), 0);
            if (remaining_bits > 0) {
                mask_vector[current_job_idx][full_bytes] = (0xFF << (8 - remaining_bits));
            }

            // Setup job descriptor
            job[current_job_idx]->op                 = qpl_op_expand;
            job[current_job_idx]->next_in_ptr        = whole_src_vector.data() + current_idx;
            job[current_job_idx]->next_out_ptr       = dest_vector[current_job_idx].data();
            job[current_job_idx]->available_in       = static_cast<uint32_t>(vector_size);
            job[current_job_idx]->available_out      = static_cast<uint32_t>(mask_size);
            job[current_job_idx]->src1_bit_width     = input_vector_width;
            job[current_job_idx]->src2_bit_width     = output_vector_width;
            job[current_job_idx]->available_src2     = mask_byte_length/2;
            job[current_job_idx]->num_input_elements = mask_size/2;
            job[current_job_idx]->out_bit_width      = qpl_ow_8;
            job[current_job_idx]->next_src2_ptr      = mask_vector[current_job_idx].data();

            // Submit
            if(execution_path == qpl_path_software) {
                status = qpl_execute_job(job[current_job_idx]);
                // Software path is synchronous usually, but we still count it as "flight" logic for uniformity 
                // (though for software path qpl_execute_job blocks).
            } else {
                status = qpl_submit_job(job[current_job_idx]);
            }
            
            if (status != QPL_STS_OK) {
                std::cout << "An error " << status << " acquired during job submission." << std::endl;
                return 1;
            }

            // Update state
            current_idx += vector_size;
            src_file_left -= vector_size;
            jobs_in_flight++;
            current_job_idx = (current_job_idx + 1) % queue_size;
            
            // Show progress
            if (current_idx % (chunk_size * 10) == 0) {
                std::cout << '\r' << "Progress ... " << current_idx << " / " << src_file_size << " Bytes" << std::flush;
            }
        }

        // Wait for oldest job (if pipeline full or no more data)
        if (jobs_in_flight > 0) {
            if (execution_path != qpl_path_software) {
                status = qpl_wait_job(job[oldest_job_idx]);
                if (status != QPL_STS_OK) {
                     std::cout << "An error " << status << " acquired during job waiting." << std::endl;
                     return 1;
                }
            }
            
            jobs_in_flight--;
            oldest_job_idx = (oldest_job_idx + 1) % queue_size;
        }
    }

    auto whole_end = std::chrono::steady_clock::now();
    elapsed_time_ns = whole_end - whole_start;
    
    if (use_rdma_path) {
        qpl_rdma_unregister_buffer(whole_src_vector.data());
        for(int i = 0; i < queue_size; i++) {
            qpl_rdma_unregister_buffer(src_vector[i].data());
            qpl_rdma_unregister_buffer(dest_vector[i].data());
            qpl_rdma_unregister_buffer(mask_vector[i].data());
        }
    }

    // Freeing resources
    for (int i = 0; i < queue_size; ++i) {
        status = qpl_fini_job(job[i]);
        if (status != QPL_STS_OK) {
            std::cout << "An error " << status << " acquired during job finalization." << std::endl;
            return 1;
        }
    }

    std::cout << std::endl;
    std::cout << "Expand was performed successfully." << std::endl;
    std::cout << "Input size      = " << src_file_size << " Bytes" << std::endl;
    elapsed_time_sec = static_cast<double>(elapsed_time_ns.count()) / 1000 / 1000 / 1000;
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
    const std::string DEST_DATA_FILE_PATH   = SRC_DATA_FILE_PATH + ".iaa.expanded";
    
    const uint32_t queue_size = static_cast<uint32_t>(atoi(argv[3]));
    uint32_t iteration = 0;

    std::cout << "Queue Size = " << queue_size << std::endl;
    std::cout << std::endl;
    // Expand
    chunk_size = static_cast<size_t>(atoi(argv[4]));
    mask_size = chunk_size;
    
    // Warmup to establish RDMA connection before timing
    if (do_warmup_job(execution_path) != 0) {
        std::cout << "Warmup failed!" << std::endl;
        return 1;
    }
    
    if(iaa_expand(SRC_DATA_FILE_PATH, DEST_DATA_FILE_PATH, execution_path, iteration, queue_size) != 0) {
        std::cout << "An error acquired during iaa_execution(Expand)" << std::endl;
        return 1;
    }

    return 0;
}

//* [QPL_LOW_LEVEL_Expand_EXAMPLE] */
