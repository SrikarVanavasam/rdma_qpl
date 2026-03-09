#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>
#include <unistd.h>

#include "qpl/qpl.h"

// Magic Hybrid ID
#define QPL_RDMA_HYBRID_NUMA_ID (-200)

constexpr const uint32_t source_size = 1000;

int main(int argc, char** argv) {
     if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <server_ip>\n";
        return 1;
    }

    setenv("QPL_RDMA_SERVER_IP", argv[1], 1);

    std::cout << "Testing Hybrid Mode (" << QPL_RDMA_HYBRID_NUMA_ID << ")\n";

    // Setup Job
    uint32_t size = 0;
    qpl_get_job_size(qpl_path_hardware, &size);
    std::unique_ptr<uint8_t[]> job_buffer = std::make_unique<uint8_t[]>(size);
    qpl_job* job = reinterpret_cast<qpl_job*>(job_buffer.get());
    qpl_init_job(qpl_path_hardware, job);

    std::vector<uint8_t> source(source_size, 5);
    std::vector<uint8_t> dest(source_size * 2, 0);

    // Register buffers (Needed for Remote path)
    if (qpl_rdma_register_buffer(source.data(), source_size) != QPL_STS_OK) {
        std::cerr << "Failed to register source buffer.\n";
        return 1;
    }
    if (qpl_rdma_register_buffer(dest.data(), dest.size()) != QPL_STS_OK) {
        std::cerr << "Failed to register dest buffer.\n";
        return 1;
    }

    std::cout << "Buffers registered. Starting 4 jobs (Expect: Local, Remote, Local, Remote)...\n";

    // Loop to test Round Robin
    for (int i = 0; i < 4; ++i) {
        job->op = qpl_op_compress;
        job->level = qpl_default_level;
        job->next_in_ptr = source.data();
        job->available_in = source_size;
        job->next_out_ptr = dest.data();
        job->available_out = dest.size();
        job->flags = QPL_FLAG_FIRST | QPL_FLAG_LAST | QPL_FLAG_DYNAMIC_HUFFMAN | QPL_FLAG_OMIT_VERIFY;
        
        // SET HYBRID ID
        job->numa_id = QPL_RDMA_HYBRID_NUMA_ID;

        std::cout << "Submitting Job #" << i << "...\n";
        qpl_status status = qpl_submit_job(job);
        if (status != QPL_STS_OK) {
             std::cerr << "Submit failed: " << status << "\n";
             return 1;
        }
        
        status = qpl_wait_job(job);
        if (status != QPL_STS_OK) {
             std::cerr << "Wait failed: " << status << "\n";
        } else {
             std::cout << "Job #" << i << " completed. Output size: " << job->total_out << "\n";
        }
    }

    qpl_rdma_unregister_buffer(source.data());
    qpl_rdma_unregister_buffer(dest.data());
    qpl_fini_job(job);

    return 0;
}
