#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_ROOT="${ROOT_DIR}/build/ios"
BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"
IOS_DEPLOYMENT_TARGET="${IOS_DEPLOYMENT_TARGET:-16.0}"

# Optional environment variables:
# - VCPKG_ROOT: path to vcpkg root (enables vcpkg toolchain)
# - VCPKG_TARGET_TRIPLET: override inferred triplet per SDK
# - CMAKE_GENERATOR: override CMake generator
# - BUILD_TYPE: override CMake build type (default: RelWithDebInfo)
# - IOS_DEPLOYMENT_TARGET: override iOS deployment target (default: 16.0)

IOS_SDK_PATH="$(xcrun --sdk iphoneos --show-sdk-path)"
SIM_SDK_PATH="$(xcrun --sdk iphonesimulator --show-sdk-path)"

find_lib() {
  local pattern="$1"
  local root="$2"
  if command -v rg >/dev/null 2>&1; then
    rg --files -g "${pattern}" "${root}" | head -n 1 || true
  else
    find "${root}" -name "${pattern}" -print -quit 2>/dev/null || true
  fi
}

combine_libs() {
  local build_dir="$1"
  local output_lib="${build_dir}/libs/ymir-core/libymir-core-combined.a"
  local inputs=()

  local core_lib="${build_dir}/libs/ymir-core/libymir-core.a"
  if [ -f "${core_lib}" ]; then
    inputs+=("${core_lib}")
  fi

  local fmt_lib
  fmt_lib="$(find_lib "libfmt.a" "${build_dir}")"
  if [ -n "${fmt_lib}" ]; then
    inputs+=("${fmt_lib}")
  fi

  local xxhash_lib
  xxhash_lib="$(find_lib "libxxHash.a" "${build_dir}")"
  if [ -n "${xxhash_lib}" ]; then
    inputs+=("${xxhash_lib}")
  fi

  local chdr_lib
  chdr_lib="$(find_lib "libchdr-static.a" "${build_dir}")"
  if [ -n "${chdr_lib}" ]; then
    inputs+=("${chdr_lib}")
  fi

  local lzma_lib
  lzma_lib="$(find_lib "liblzma.a" "${build_dir}")"
  if [ -n "${lzma_lib}" ]; then
    inputs+=("${lzma_lib}")
  fi

  if [ "${#inputs[@]}" -eq 0 ]; then
    echo "" >&2
    return 1
  fi

  libtool -static -o "${output_lib}" "${inputs[@]}"
  echo "${output_lib}"
}

build_slice() {
  local sdk_name="$1"
  local sdk_path="$2"
  local build_dir="$3"

  local toolchain_args=()
  if [ -n "${VCPKG_ROOT:-}" ]; then
    toolchain_args+=("-DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    local vcpkg_triplet="${VCPKG_TARGET_TRIPLET:-}"
    if [ -z "${vcpkg_triplet}" ]; then
      if [ "${sdk_name}" = "iphoneos" ]; then
        vcpkg_triplet="arm64-ios"
      else
        vcpkg_triplet="arm64-ios-simulator"
      fi
    fi
    toolchain_args+=("-DVCPKG_TARGET_TRIPLET=${vcpkg_triplet}")
    toolchain_args+=("-DVCPKG_OSX_DEPLOYMENT_TARGET=${IOS_DEPLOYMENT_TARGET}")
  fi

  local cmake_cmd=(
    cmake
    -S "${ROOT_DIR}"
    -B "${build_dir}"
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
    -DCMAKE_SYSTEM_NAME=iOS
    -DCMAKE_OSX_SYSROOT="${sdk_path}"
    -DCMAKE_OSX_ARCHITECTURES=arm64
    -DCMAKE_OSX_DEPLOYMENT_TARGET="${IOS_DEPLOYMENT_TARGET}"
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
    -DYmir_ENABLE_SDL3=OFF
    -DYmir_ENABLE_SANDBOX=OFF
    -DYmir_ENABLE_YMDASM=OFF
    -DYmir_ENABLE_TESTS=OFF
    -DYmir_INCLUDE_PACKAGING=OFF
    -DYmir_ENABLE_IPO=OFF
    -DYmir_ENABLE_DEVLOG=ON
    # compile_commands.json
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
  )

  if [ -n "${CMAKE_GENERATOR:-}" ]; then
    cmake_cmd=(cmake -G "${CMAKE_GENERATOR}" "${cmake_cmd[@]:1}")
  fi

  "${cmake_cmd[@]}" "${toolchain_args[@]}"
  cmake --build "${build_dir}" --target ymir-core --parallel

  local header_dir="${build_dir}/xcframework_headers"
  rm -rf "${header_dir}"
  mkdir -p "${header_dir}"
  cp -R "${ROOT_DIR}/libs/ymir-core/include/." "${header_dir}/"
  if [ -d "${build_dir}/libs/ymir-core/include" ]; then
    cp -R "${build_dir}/libs/ymir-core/include/." "${header_dir}/"
  fi
}

mkdir -p "${BUILD_ROOT}"

DEVICE_BUILD_DIR="${BUILD_ROOT}/iphoneos"
SIM_BUILD_DIR="${BUILD_ROOT}/iphonesimulator"

build_slice "iphoneos" "${IOS_SDK_PATH}" "${DEVICE_BUILD_DIR}"
build_slice "iphonesimulator" "${SIM_SDK_PATH}" "${SIM_BUILD_DIR}"

DEVICE_LIB="$(combine_libs "${DEVICE_BUILD_DIR}")"
SIM_LIB="$(combine_libs "${SIM_BUILD_DIR}")"

if [ ! -f "${DEVICE_LIB}" ]; then
  echo "Missing device library: ${DEVICE_LIB}" >&2
  exit 1
fi

if [ ! -f "${SIM_LIB}" ]; then
  echo "Missing simulator library: ${SIM_LIB}" >&2
  exit 1
fi

XCFRAMEWORK_PATH="${BUILD_ROOT}/ymir-core.xcframework"
rm -rf "${XCFRAMEWORK_PATH}"

xcodebuild -create-xcframework \
  -library "${DEVICE_LIB}" -headers "${DEVICE_BUILD_DIR}/xcframework_headers" \
  -library "${SIM_LIB}" -headers "${SIM_BUILD_DIR}/xcframework_headers" \
  -output "${XCFRAMEWORK_PATH}"

echo "XCFramework written to ${XCFRAMEWORK_PATH}"
