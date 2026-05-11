# ==========================================================================
# Copyright (C) 2025 Intel Corporation
#
# SPDX-License-Identifier: MIT
# ==========================================================================

# Intel® Query Processing Library (Intel® QPL)

# Build system

include_guard(GLOBAL)

set(QPL_LANGUAGES C CXX)
set(QPL_DEFAULT_C_STANDARD 17)
set(QPL_DEFAULT_CXX_STANDARD 17)

# Sets the language standard of the given language ${language} for the given target ${target_name} to the given version ${version}.
function (qpl_set_language_standard target_name language version)
    if (NOT language IN_LIST QPL_LANGUAGES)
        message(
            FATAL_ERROR
                "Intel(R) QPL: Unsupported language '${language}' specified for target '${target_name}'. Supported languages are: ${QPL_LANGUAGES}.")
    endif ()
    if (language STREQUAL "C")
        set(language_prefix "c")
    elseif (language STREQUAL "CXX")
        set(language_prefix "c++")
    endif ()
    set(language_standard "${language_prefix}${version}")
    # Set the language standard using `<LANG>_STANDARD` target property if it is supported by CMake (otherwise, use the compile option
    # `-std=<standard>`)
    set(language_version_is_supported_by_cmake FALSE)
    if (language STREQUAL "C")
        # https://cmake.org/cmake/help/latest/prop_tgt/C_STANDARD.html
        if (((version GREATER_EQUAL 90 OR version VERSION_LESS 17) AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.1")
            OR (version VERSION_LESS_EQUAL 23 AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.21"))
            set(language_version_is_supported_by_cmake TRUE)
        endif ()
        if (version VERSION_GREATER 23 AND version VERSION_LESS 90)
            message(
                FATAL_ERROR
                    "Intel(R) QPL: C standard version '${version}' is not supported by Intel(R) QPL build system. Please, fix this CMake file to add support for this version."
            )
        endif ()
    elseif (language STREQUAL "CXX")
        # https://cmake.org/cmake/help/latest/prop_tgt/CXX_STANDARD.html
        if (((version GREATER_EQUAL 98 OR version VERSION_LESS 17) AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.1")
            OR (version VERSION_EQUAL 17 AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.8")
            OR (version VERSION_EQUAL 20 AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.12")
            OR (version VERSION_EQUAL 23 AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.20")
            OR (version VERSION_EQUAL 26 AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.25"))
            set(language_version_is_supported_by_cmake TRUE)
        endif ()
        if (version VERSION_GREATER 26 AND version VERSION_LESS 98)
            message(
                FATAL_ERROR
                    "Intel(R) QPL: C++ standard version '${version}' is not supported by Intel(R) QPL build system. Please, fix this CMake file to add support for this version."
            )
        endif ()
    endif ()
    if (language_version_is_supported_by_cmake)
        set_target_properties(
            ${target_name}
            PROPERTIES ${language}_STANDARD ${version}
                       ${language}_EXTENSIONS OFF
                       ${language}_STANDARD_REQUIRED ON)
    else ()
        target_compile_options(
            ${target_name} PRIVATE $<$<AND:$<COMPILE_LANGUAGE:${language}>,$<${language}_COMPILER_ID:GNU,Clang>>:-std=${language_standard}>
                                   $<$<AND:$<COMPILE_LANGUAGE:${language}>,$<${language}_COMPILER_ID:MSVC>>:/std:${language_standard}>)
    endif ()
    # Additionally, set the language standard using `<lang>_std_<version>` compile feature if it is supported by CMake
    set(language_feature_is_supported_by_cmake FALSE)
    if (language STREQUAL "C")
        # https://cmake.org/cmake/help/latest/prop_gbl/CMAKE_C_KNOWN_FEATURES.html
        if (((version GREATER_EQUAL 90 OR version VERSION_LESS 17) AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.8")
            OR (version VERSION_LESS_EQUAL 23 AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.21"))
            set(language_feature_is_supported_by_cmake TRUE)
        endif ()
        if (version VERSION_GREATER 23 AND version VERSION_LESS 90)
            message(FATAL_ERROR "Intel(R) QPL: C standard version feature 'c_std_${version}' is not supported by Intel(R) QPL build system. \
Please, fix this CMake file to add support for this version.")
        endif ()
    elseif (language STREQUAL "CXX")
        # https://cmake.org/cmake/help/latest/prop_gbl/CMAKE_CXX_KNOWN_FEATURES.html
        if (((version GREATER_EQUAL 98 OR version VERSION_LESS 20) AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.8")
            OR (version VERSION_EQUAL 20 AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.12")
            OR (version VERSION_EQUAL 23 AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.20")
            OR (version VERSION_EQUAL 26 AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.30"))
            set(language_feature_is_supported_by_cmake TRUE)
        endif ()
        if (version VERSION_GREATER 26 AND version VERSION_LESS 98)
            message(FATAL_ERROR "Intel(R) QPL: C++ standard version feature 'cxx_std_${version}' is not supported by Intel(R) QPL build system. \
Please, fix this CMake file to add support for this version.")
        endif ()
    endif ()
    if (language_feature_is_supported_by_cmake)
        set(language_feature "${language}_std_${version}")
        string(TOLOWER "${language_feature}" language_feature)
        target_compile_features(${target_name} PRIVATE ${language_feature})
    endif ()
endfunction ()

# Sets C and C++ language standards for the given target ${target_name}.
#
# Defaults are C17 and C++17.
function (qpl_set_language_standards target_name)
    cmake_parse_arguments(
        "ARGUMENT" # prefix
        "" # options
        "${QPL_LANGUAGES}" # one-value arguments
        "" # multi-value arguments
        ${ARGN})
    foreach (language IN LISTS QPL_LANGUAGES)
        if (NOT DEFINED ARGUMENT_${language})
            set(ARGUMENT_${language} ${QPL_DEFAULT_${language}_STANDARD})
        endif ()
        qpl_set_language_standard(${target_name} ${language} ${ARGUMENT_${language}})
    endforeach ()
endfunction ()

option(QPL_TREAT_WARNINGS_AS_ERRORS "Treat all compiler warnings as errors" ON)

# Enables all reasonable warnings for a target with the given name ${target_name}.
#
# Treats them as errors if $CACHE{QPL_TREAT_WARNINGS_AS_ERRORS} option is set to ON.
function (qpl_enable_warnings target_name)
    # TODO: enable warnings here, and not in CMakeLists.txt files

    # Enable C/C++ standard conformance warnings
    if (MSVC)
        target_compile_options(${target_name} PRIVATE /permissive- /volatile:iso)
    else ()
        if (NOT DEFINED ACC_POOL_PATH)
            if ($CACHE{QPL_TREAT_WARNINGS_AS_ERRORS})
                target_compile_options(${target_name} PRIVATE -pedantic-errors)
            else ()
                target_compile_options(${target_name} PRIVATE -pedantic)
            endif ()
        endif ()
    endif ()
    # Treat warnings as errors
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
    qpl_set_language_standards(${target_name} ${ARGN})
    qpl_enable_warnings(${target_name})
endfunction ()
