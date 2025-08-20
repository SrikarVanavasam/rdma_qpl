/*******************************************************************************
 * Copyright (C) 2025 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#pragma once

#include "qpl/c_api/defs.h"

constexpr uint32_t qpl_out_format_to_uint(qpl_out_format output) {
    switch (output) {
        case qpl_ow_nom: return 1U;
        case qpl_ow_8: return 8U;
        case qpl_ow_16: return 16U;
        case qpl_ow_32: return 32U;
        default: return 1U;
    }
}

inline bool validate_filtering_input(uint32_t input_bit_width_1, const uint8_t* p_input_bit_width_2,
                                     bool is_bit_width_2, qpl_out_format out_format_enum, size_t number_of_elements,
                                     int64_t src_available_bytes, size_t dst_available_bytes) {
    if (src_available_bytes <= 0) { return false; }

    // Check if input and output bit widths are valid
    uint32_t input_bit_width = input_bit_width_1;
    if (is_bit_width_2) { input_bit_width = static_cast<uint32_t>(*p_input_bit_width_2); }
    const uint32_t output_bit_width = qpl_out_format_to_uint(out_format_enum);
    if (input_bit_width > output_bit_width) { return false; }

    // Check if input and output sizes are valid
    const size_t src_required_bytes = (number_of_elements * input_bit_width + 7) / 8;
    const size_t dst_required_bytes = (number_of_elements * output_bit_width + 7) / 8;
    if (src_required_bytes > src_available_bytes || dst_required_bytes > dst_available_bytes) { return false; }

    // Check if number of elements can be written with the given output bit width
    if (output_bit_width != 1U) {
        const uint32_t max_output_value = (1ULL << output_bit_width) - 1;
        if (max_output_value < number_of_elements) { return false; }
    }
    return true;
}
