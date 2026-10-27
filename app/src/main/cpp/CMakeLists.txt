# SPDX-FileCopyrightText: Copyright (c) 2026 V-Nova International Limited
#
# Modified by V-Nova International Limited, to support integration with LCEVCdec.
# V-Nova's modifications are licensed under GPL-3.0-or-later.

cmake_minimum_required(VERSION 3.22)
project(vcat_lcevc_vvdec_jni LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

option(VN_SDK_TRACING_SYSTEM_SINK "Enable LCEVCdec Android system tracing" OFF)

if(NOT ANDROID)
  message(FATAL_ERROR "Android/NDK build only")
endif()

# Per-ABI roots (arm64-v8a only)
if(${ANDROID_ABI} STREQUAL "arm64-v8a")
  set(VVDEC_INSTALL_DIR "${VVDEC_INSTALL_ARM64_V8A}")
else()
  message(
    FATAL_ERROR
      "Unsupported ABI: ${ANDROID_ABI} (this plugin builds arm64-v8a only)")
endif()

message(STATUS "ABI='${ANDROID_ABI}'")
message(STATUS "  VVDEC_INSTALL_DIR='${VVDEC_INSTALL_DIR}'")
message(STATUS "  LCEVC_SRC_DIR='${LCEVC_SRC_DIR}'")

# vvdec (required)
set(VVDEC_INCLUDE_DIR "${VVDEC_INSTALL_DIR}/include")
set(VVDEC_LIB_DIR "${VVDEC_INSTALL_DIR}/lib")
set(VVDEC_STATIC "${VVDEC_LIB_DIR}/libvvdec.a")

if(NOT EXISTS "${VVDEC_STATIC}")
  message(FATAL_ERROR "Static vvdec not found: ${VVDEC_STATIC}")
endif()

# LCEVCdec (required). Build it as part of this CMake tree so its public targets
# carry their include paths and transitive dependencies into the plugin.
set(BUILD_SHARED_LIBS OFF)
set(VN_SDK_PIPELINE_CPU ON)
set(VN_SDK_PIPELINE_VULKAN ON)
set(VN_SDK_SIMD ON)
set(VN_SDK_EXECUTABLES OFF)
set(VN_SDK_UNIT_TESTS OFF)
set(VN_SDK_SAMPLE_SOURCE OFF)
set(VN_SDK_DOCS OFF)
set(VN_SDK_TRACING ${VN_SDK_TRACING_SYSTEM_SINK})
set(VN_SDK_TRACING_SYSTEM_SINK ${VN_SDK_TRACING_SYSTEM_SINK})

if(CONAN_DEPS_ROOT)
  set(CONAN_DEPS_DIR "${CONAN_DEPS_ROOT}/${CMAKE_BUILD_TYPE}/${ANDROID_ABI}")
  set(VN_SDK_CONAN_DEPS_DIR
      "${CONAN_DEPS_DIR}"
      CACHE PATH "" FORCE)
endif()

if(VN_SDK_TRACING_SYSTEM_SINK)
  if(NOT CONAN_DEPS_DIR)
    message(FATAL_ERROR "System tracing requires -PbuildWithConan=TRUE")
  endif()
  set(fmt_DIR "${CONAN_DEPS_DIR}")
  set(perfetto_DIR "${CONAN_DEPS_DIR}")
endif()

set(LCEVC_BUILD_DIR
    "${CMAKE_BINARY_DIR}/lcevcdec-${ANDROID_ABI}-${CMAKE_BUILD_TYPE}")

add_subdirectory("${LCEVC_SRC_DIR}" "${LCEVC_BUILD_DIR}" EXCLUDE_FROM_ALL)

# Sources
add_library(
  vcat_lcevc_vvdec_jni SHARED
  lcevcdec_vvdec_jni.cpp
  utilities/surface_presenter.cpp
  utilities/yuv10_convert_neon.cpp
  utilities/slice_thread_pool.cpp)

target_include_directories(vcat_lcevc_vvdec_jni PRIVATE "${VVDEC_INCLUDE_DIR}")

find_library(log_lib log)
find_library(android_lib android)
find_library(nativewindow_lib nativewindow)
find_library(vulkan_lib vulkan)

if(NOT vulkan_lib)
  message(
    FATAL_ERROR
      "libvulkan (required by LCEVCdec Vulkan mode) not found in NDK sysroot")
endif()

target_link_libraries(
  vcat_lcevc_vvdec_jni
  PRIVATE "${VVDEC_STATIC}"
          lcevc_dec::api
          lcevc_dec::extract
          lcevc_dec::common
          ${log_lib}
          ${android_lib}
          ${nativewindow_lib}
          ${vulkan_lib}
          c++_static)

if(VN_SDK_TRACING_SYSTEM_SINK)
  target_link_libraries(vcat_lcevc_vvdec_jni PRIVATE lcevc_dec::utility)
endif()

target_link_options(vcat_lcevc_vvdec_jni PRIVATE -Wl,--no-undefined)
