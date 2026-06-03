#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include "qpl/qpl.h"

const std::size_t CHUNK_SIZE = 2097152; // 2MB

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cout << "Usage: " << argv[0] << " <input_file> <output_prefix>\n";
        return 1;
    }

    std::string src_path = argv[1];
    std::string dst_prefix = argv[2];

    std::ifstream src_file(src_path, std::ios::binary);
    if (!src_file) {
        std::cerr << "Failed to open input file: " << src_path << std::endl;
        return 1;
    }

    uint32_t job_size = 0;
    qpl_get_job_size(qpl_path_software, &job_size);
    auto job_buffer = std::vector<uint8_t>(job_size);
    auto job = reinterpret_cast<qpl_job*>(job_buffer.data());
    qpl_init_job(qpl_path_software, job);

    std::vector<uint8_t> in_buf(CHUNK_SIZE);
    std::vector<uint8_t> out_buf(CHUNK_SIZE * 2);

    int chunk_id = 0;
    while (src_file.read(reinterpret_cast<char*>(in_buf.data()), CHUNK_SIZE) || src_file.gcount() > 0) {
        size_t bytes_read = src_file.gcount();
        
        job->op = qpl_op_compress;
        job->level = qpl_default_level;
        job->next_in_ptr = in_buf.data();
        job->available_in = bytes_read;
        job->next_out_ptr = out_buf.data();
        job->available_out = out_buf.size();
        job->flags = QPL_FLAG_FIRST | QPL_FLAG_LAST | QPL_FLAG_OMIT_VERIFY;

        // Use software path for compression to avoid needing HW for prep
        qpl_status status = qpl_execute_job(job);
        if (status != QPL_STS_OK) {
            std::cerr << "Compression failed for chunk " << chunk_id << " status: " << status << std::endl;
            return 1;
        }

        std::string out_path = dst_prefix + "." + std::to_string(chunk_id);
        std::ofstream out_file(out_path, std::ios::binary);
        out_file.write(reinterpret_cast<char*>(out_buf.data()), job->total_out);
        
        // std::cout << "Compressed chunk " << chunk_id << ": " << bytes_read << " -> " << job->total_out << " bytes\n";
        chunk_id++;
    }

    qpl_fini_job(job);
    return 0;
}
