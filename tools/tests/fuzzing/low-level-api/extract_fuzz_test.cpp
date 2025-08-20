/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include <memory>
#include <vector>

#include "qpl/qpl.h"

#include "filtering_fuzz_common.hpp"

#ifndef QPL_EXECUTION_PATH
#define QPL_EXECUTION_PATH qpl_path_software
#endif

constexpr qpl_path_t execution_path = QPL_EXECUTION_PATH;

struct extract_properties {
    uint16_t       destination_size    = 0;
    uint32_t       input_bit_width     = 0;
    size_t         number_of_elements  = 0;
    qpl_out_format output_bit_width    = qpl_ow_nom;
    uint32_t       high_index_boundary = 0;
    uint32_t       low_index_boundary  = 0;
};

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* Data, size_t Size) {
    if (0 == Size) { return 0; }

    qpl_parser parser = static_cast<qpl_parser>(Data[0] % 3);

    Data++;
    Size--;

    if (Size > sizeof(extract_properties)) {
        extract_properties properties = *reinterpret_cast<const extract_properties*>(Data);

        // Make sure the output bit width is in the valid range of enum qpl_out_format
        properties.output_bit_width =
                static_cast<qpl_out_format>(static_cast<uint8_t>(properties.output_bit_width) % 4);

        const int64_t source_size   = static_cast<int64_t>(Size) - sizeof(extract_properties);
        const bool    is_rle_parser = qpl_p_parquet_rle == parser;
        const auto    source_ptr    = Data + sizeof(extract_properties);
        bool          is_good_data  = validate_filtering_input(properties.input_bit_width, source_ptr, is_rle_parser,
                                                               properties.output_bit_width, properties.number_of_elements,
                                                               source_size, properties.destination_size);
        if (properties.high_index_boundary > properties.number_of_elements) { is_good_data = false; }
        if (is_good_data) {
            std::vector<uint8_t> source(Data + sizeof(extract_properties), Data + Size);
            std::vector<uint8_t> destination(properties.destination_size);

            qpl_status status;
            uint32_t   job_size = 0;

            // Job initialization
            status = qpl_get_job_size(execution_path, &job_size);
            if (status != QPL_STS_OK) { return 0; }

            auto     job_buffer = std::make_unique<uint8_t[]>(job_size);
            qpl_job* job_ptr    = reinterpret_cast<qpl_job*>(job_buffer.get());

            status = qpl_init_job(execution_path, job_ptr);
            if (status != QPL_STS_OK) { return 0; }

            job_ptr->next_in_ptr        = source.data();
            job_ptr->available_in       = source.size();
            job_ptr->next_out_ptr       = destination.data();
            job_ptr->available_out      = destination.size();
            job_ptr->op                 = qpl_op_extract;
            job_ptr->num_input_elements = properties.number_of_elements;
            job_ptr->src1_bit_width     = properties.input_bit_width;
            job_ptr->param_low          = properties.low_index_boundary;
            job_ptr->param_high         = properties.high_index_boundary;
            job_ptr->out_bit_width      = properties.output_bit_width;
            job_ptr->parser             = parser;

            status = qpl_execute_job(job_ptr);
        }
    }

    return 0;
}
