# ==========================================================================
# Copyright (C) 2025 Intel Corporation
#
# SPDX-License-Identifier: MIT
# ==========================================================================

include_guard(GLOBAL)

option(QPL_TREAT_WARNINGS_AS_ERRORS "Treat all compiler warnings as errors" ON)

# Enables all reasonable warnings for a target with the given name ${target_name}.
#
# Treats them as errors if $CACHE{QPL_TREAT_WARNINGS_AS_ERRORS} option is set to ON.
function (qpl_enable_warnings target_name)
    if ($CACHE{QPL_TREAT_WARNINGS_AS_ERRORS})
        if (MSVC)
            target_compile_options(${target_name} PRIVATE /WX)
        else ()
            target_compile_options(${target_name} PRIVATE -Werror)
        endif ()
    endif ()
endfunction ()

# Sets common target settings for the target with the given name ${target_name}.
function (qpl_set_common_target_settings target_name)
    qpl_enable_warnings(${target_name})
endfunction ()
