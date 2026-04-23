#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_ROOT="${ROOT_DIR}/build/ios"
BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"
IOS_DEPLOYMENT_TARGET="${IOS_DEPLOYMENT_TARGET:-16.0}"

# VCPKG_ROOT discovery and validation
LOCAL_VCPKG="${ROOT_DIR}/vcpkg"

# If a local submodule exists and is functional, prioritize it for project consistency
if [ -d "${LOCAL_VCPKG}" ] && [ -f "${LOCAL_VCPKG}/scripts/buildsystems/vcpkg.cmake" ]; then
  if [ -n "${VCPKG_ROOT:-}" ] && [ "${VCPKG_ROOT}" != "${LOCAL_VCPKG}" ]; then
    echo "Note: Overriding environment VCPKG_ROOT with local submodule for consistency."
  fi
  export VCPKG_ROOT="${LOCAL_VCPKG}"
  echo "Using local vcpkg: ${VCPKG_ROOT}"
fi

# Validate VCPKG_ROOT if it was provided by environment and we didn't use the submodule
if [ -n "${VCPKG_ROOT:-}" ]; then
  if [ ! -f "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" ]; then
    echo "Warning: VCPKG_ROOT is set to '${VCPKG_ROOT}' but it is not a valid vcpkg installation (missing toolchain)."
    unset VCPKG_ROOT
  fi
fi

# Optional environment variables:
# - VCPKG_ROOT: path to vcpkg root (enables vcpkg toolchain)
# - VCPKG_TARGET_TRIPLET: override inferred triplet per SDK
# - CMAKE_GENERATOR: override CMake generator
# - BUILD_TYPE: override CMake build type (default: RelWithDebInfo)
# - IOS_DEPLOYMENT_TARGET: override iOS deployment target (default: 16.0)

# Argument parsing
NO_BINARY_CACHE=0

show_help() {
  echo "Usage: $0 [options]"
  echo "Options:"
  echo "  -nbc, --no-binary-cache    Disable vcpkg binary caching"
  echo "  -h, --help                Show this help message"
}

while [[ $# -gt 0 ]]; do
  case $1 in
    -nbc|--no-binary-cache)
      NO_BINARY_CACHE=1
      shift
      ;;
    -h|--help)
      show_help
      exit 0
      ;;
    *)
      echo "Unknown option: $1"
      show_help
      exit 1
      ;;
  esac
done

IOS_SDK_PATH="$(xcrun --sdk iphoneos --show-sdk-path)"
SIM_SDK_PATH="$(xcrun --sdk iphonesimulator --show-sdk-path)"

ensure_vcpkg_curl_overlay() {
  if [ -z "${VCPKG_ROOT:-}" ]; then
    return 0
  fi

  local overlay_root="${BUILD_ROOT}/vcpkg-overlay-ports"
  local curl_overlay_dir="${overlay_root}/curl"
  mkdir -p "${curl_overlay_dir}"

  cp -R "${VCPKG_ROOT}/ports/curl/." "${curl_overlay_dir}/"

  cat <<EOF > "${curl_overlay_dir}/disable-pipe2-on-apple-mobile.patch"
diff --git a/CMakeLists.txt b/CMakeLists.txt
index ed0561d..655a22d 100644
--- a/CMakeLists.txt
+++ b/CMakeLists.txt
@@ -1591,7 +1591,11 @@ check_symbol_exists("getaddrinfo"     "\${CURL_INCLUDES};stdlib.h;string.h" HAVE
 check_symbol_exists("getifaddrs"      "\${CURL_INCLUDES};stdlib.h" HAVE_GETIFADDRS)  # ifaddrs.h
 check_symbol_exists("freeaddrinfo"    "\${CURL_INCLUDES}" HAVE_FREEADDRINFO)  # ws2tcpip.h sys/socket.h netdb.h
 check_function_exists("pipe"          HAVE_PIPE)
-check_function_exists("pipe2"         HAVE_PIPE2)
+if(APPLE AND (CMAKE_SYSTEM_NAME STREQUAL "iOS" OR CMAKE_SYSTEM_NAME STREQUAL "tvOS" OR CMAKE_SYSTEM_NAME STREQUAL "watchOS"))
+  set(HAVE_PIPE2 0 CACHE INTERNAL "Apple mobile SDKs do not expose pipe2" FORCE)
+else()
+  check_function_exists("pipe2"       HAVE_PIPE2)
+endif()
 check_function_exists("eventfd"       HAVE_EVENTFD)
 check_symbol_exists("ftruncate"       "unistd.h" HAVE_FTRUNCATE)
 check_symbol_exists("getpeername"     "\${CURL_INCLUDES}" HAVE_GETPEERNAME)  # winsock2.h unistd.h proto/bsdsocket.h
EOF

  cat <<EOF > "${curl_overlay_dir}/portfile.cmake"
string(REPLACE "." "_" curl_version "curl-\${VERSION}")

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO curl/curl
    REF \${curl_version}
    SHA512 1dca42354d29b9326a3e9be34c74433c3a7364318d69519e2f5b9a164e81db739d3ef1eed79e3313296fe72af73281e0fc61e57a21e9dede1ef240c8fa6af4fe
    HEAD_REF master
    PATCHES
        dependencies.patch
        disable-pipe2-on-apple-mobile.patch
        winsock.diff
)
# The on-the-fly tarballs do not carry the details of release tarballs.
vcpkg_replace_string("\${SOURCE_PATH}/include/curl/curlver.h" [[-DEV"]] [["]])
vcpkg_replace_string("\${SOURCE_PATH}/include/curl/curlver.h" [[LIBCURL_TIMESTAMP "[unreleased]"]] [[LIBCURL_TIMESTAMP "[vcpkg]"]])

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        http2       USE_NGHTTP2
        http3       USE_NGTCP2
        wolfssl     CURL_USE_WOLFSSL
        openssl     CURL_USE_OPENSSL
        openssl     CURL_CA_FALLBACK
        mbedtls     CURL_USE_MBEDTLS
        ssh         CURL_USE_LIBSSH2
        tool        BUILD_CURL_EXE
        c-ares      ENABLE_ARES
        sspi        CURL_WINDOWS_SSPI
        brotli      CURL_BROTLI
        idn2        USE_LIBIDN2
        winidn      USE_WIN32_IDN
        zstd        CURL_ZSTD
        psl         CURL_USE_LIBPSL
        gssapi      CURL_USE_GSSAPI
        gsasl       CURL_USE_GSASL
        gnutls      CURL_USE_GNUTLS
        rtmp        USE_LIBRTMP
        httpsrr     USE_HTTPSRR
        ssls-export USE_SSLS_EXPORT
    INVERTED_FEATURES
        ldap        CURL_DISABLE_LDAP
        ldap        CURL_DISABLE_LDAPS
        non-http    HTTP_ONLY
        websockets  CURL_DISABLE_WEBSOCKETS
)

if("ssl" IN_LIST FEATURES AND
    NOT "http3" IN_LIST FEATURES AND
    ((VCPKG_TARGET_IS_WINDOWS AND NOT VCPKG_TARGET_IS_UWP) OR VCPKG_TARGET_IS_MINGW))
    list(APPEND FEATURE_OPTIONS -DCURL_USE_SCHANNEL=ON)
endif()

if("http3" IN_LIST FEATURES AND
    ("wolfssl" IN_LIST FEATURES OR
     "mbedtls" IN_LIST FEATURES OR
     "gnutls" IN_LIST FEATURES))
    message(FATAL_ERROR "http3 is incompatible with curl multi-ssl, preventing combination with wolfssl, mbedtls or gnutls in vcpkg's curated registry. To use curl http3 on ngtcp2 on one of the other TLS backends, author an overlay-port which exchanges curl[ssl]'s and curl[http3]'s openssl dependencies with the backend you want.")
endif()

set(OPTIONS "")

if(VCPKG_TARGET_IS_UWP)
    list(APPEND OPTIONS
        -DCURL_DISABLE_TELNET=ON
        -DENABLE_UNIX_SOCKETS=OFF
    )
endif()

if(VCPKG_TARGET_IS_WINDOWS)
    list(APPEND OPTIONS -DENABLE_UNICODE=ON)
endif()

vcpkg_find_acquire_program(PKGCONFIG)

vcpkg_cmake_configure(
    SOURCE_PATH "\${SOURCE_PATH}"
    OPTIONS
        "-DCMAKE_PROJECT_INCLUDE=\${CMAKE_CURRENT_LIST_DIR}/cmake-project-include.cmake"
        "-DPKG_CONFIG_EXECUTABLE=\${PKGCONFIG}"
        \${FEATURE_OPTIONS}
        \${OPTIONS}
        -DBUILD_TESTING=OFF
        -DENABLE_CURL_MANUAL=OFF
        -DIMPORT_LIB_SUFFIX=
        -DSHARE_LIB_OBJECT=OFF
        -DCURL_USE_PKGCONFIG=ON
        -DCMAKE_DISABLE_FIND_PACKAGE_Perl=ON
    MAYBE_UNUSED_VARIABLES
        PKG_CONFIG_EXECUTABLE
)
vcpkg_cmake_install()
vcpkg_copy_pdbs()

if ("tool" IN_LIST FEATURES)
    vcpkg_copy_tools(TOOL_NAMES curl AUTO_CLEAN)
endif()

vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/CURL)

vcpkg_fixup_pkgconfig()
set(namespec "curl")
if(VCPKG_TARGET_IS_WINDOWS AND NOT VCPKG_TARGET_IS_MINGW)
    set(namespec "libcurl")
    vcpkg_replace_string("\${CURRENT_PACKAGES_DIR}/lib/pkgconfig/libcurl.pc" " -lcurl" " -l\${namespec}")
endif()
if(NOT DEFINED VCPKG_BUILD_TYPE)
    vcpkg_replace_string("\${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig/libcurl.pc" " -lcurl" " -l\${namespec}-d")
endif()

vcpkg_replace_string("\${CURRENT_PACKAGES_DIR}/bin/curl-config" "\${CURRENT_PACKAGES_DIR}" "\${prefix}")
vcpkg_replace_string("\${CURRENT_PACKAGES_DIR}/bin/curl-config" "\${CURRENT_INSTALLED_DIR}" "\${prefix}" IGNORE_UNCHANGED)
vcpkg_replace_string("\${CURRENT_PACKAGES_DIR}/bin/curl-config" "\nprefix='\${prefix}'" [=[prefix=\$(CDPATH= cd -- "\$(dirname -- "\$0")"/../../.. && pwd -P)]=])
file(MAKE_DIRECTORY "\${CURRENT_PACKAGES_DIR}/tools/\${PORT}/bin")
file(RENAME "\${CURRENT_PACKAGES_DIR}/bin/curl-config" "\${CURRENT_PACKAGES_DIR}/tools/\${PORT}/bin/curl-config")
if(EXISTS "\${CURRENT_PACKAGES_DIR}/debug/bin/curl-config")
    vcpkg_replace_string("\${CURRENT_PACKAGES_DIR}/debug/bin/curl-config" "\${CURRENT_PACKAGES_DIR}" "\${prefix}")
    vcpkg_replace_string("\${CURRENT_PACKAGES_DIR}/debug/bin/curl-config" "\${CURRENT_INSTALLED_DIR}" "\${prefix}" IGNORE_UNCHANGED)
    vcpkg_replace_string("\${CURRENT_PACKAGES_DIR}/debug/bin/curl-config" "\nprefix='\${prefix}/debug'" [=[prefix=\$(CDPATH= cd -- "\$(dirname -- "\$0")"/../../../.. && pwd -P)]=])
    vcpkg_replace_string("\${CURRENT_PACKAGES_DIR}/debug/bin/curl-config" "\nexec_prefix=\"\${prefix}\"" "\nexec_prefix=\"\${prefix}/debug\"")
    vcpkg_replace_string("\${CURRENT_PACKAGES_DIR}/debug/bin/curl-config" "-lcurl" "-l\${namespec}-d")
    vcpkg_replace_string("\${CURRENT_PACKAGES_DIR}/debug/bin/curl-config" "curl." "curl-d.")
    file(MAKE_DIRECTORY "\${CURRENT_PACKAGES_DIR}/tools/\${PORT}/debug/bin")
    file(RENAME "\${CURRENT_PACKAGES_DIR}/debug/bin/curl-config" "\${CURRENT_PACKAGES_DIR}/tools/\${PORT}/debug/bin/curl-config")
endif()

file(REMOVE_RECURSE "\${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "\${CURRENT_PACKAGES_DIR}/debug/share")
if(VCPKG_LIBRARY_LINKAGE STREQUAL "static" OR NOT VCPKG_TARGET_IS_WINDOWS)
    file(REMOVE_RECURSE "\${CURRENT_PACKAGES_DIR}/bin")
    file(REMOVE_RECURSE "\${CURRENT_PACKAGES_DIR}/debug/bin")
endif()

if(VCPKG_LIBRARY_LINKAGE STREQUAL "static")
    vcpkg_replace_string("\${CURRENT_PACKAGES_DIR}/include/curl/curl.h"
        "#ifdef CURL_STATICLIB"
        "#if 1"
    )
endif()

file(INSTALL "\${CURRENT_PORT_DIR}/vcpkg-cmake-wrapper.cmake" DESTINATION "\${CURRENT_PACKAGES_DIR}/share/\${PORT}")
file(INSTALL "\${CURRENT_PORT_DIR}/usage" DESTINATION "\${CURRENT_PACKAGES_DIR}/share/\${PORT}")

file(READ "\${SOURCE_PATH}/lib/curlx/inet_ntop.c" inet_ntop_c)
string(REGEX REPLACE "#i.*" "" inet_ntop_c "\${inet_ntop_c}")
set(inet_ntop_copyright "\${CURRENT_BUILDTREES_DIR}/inet_ntop.c and inet_pton.c Notice")
file(WRITE "\${inet_ntop_copyright}" "\${inet_ntop_c}")

vcpkg_install_copyright(
    FILE_LIST
        "\${SOURCE_PATH}/COPYING"
        "\${inet_ntop_copyright}"
)
EOF

  VCPKG_CURL_OVERLAY_PORT_DIR="${overlay_root}"
}

ensure_vcpkg_triplets() {
  local triplet_dir="${ROOT_DIR}/vcpkg-triplets"
  mkdir -p "${triplet_dir}"

  # Create arm64-ios triplet if missing
  if [ ! -f "${triplet_dir}/arm64-ios.cmake" ]; then
    cat <<EOF > "${triplet_dir}/arm64-ios.cmake"
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME iOS)
EOF
  fi

  # Create/Update arm64-ios-simulator triplet with the pipe2 fix
  cat <<EOF > "${triplet_dir}/arm64-ios-simulator.cmake"
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME iOS)
set(VCPKG_OSX_SYSROOT iphonesimulator)
# Force-disable pipe2 for the simulator to avoid mis-detection on macOS hosts
set(HAVE_PIPE2 0 CACHE INTERNAL "" FORCE)
set(HAVE_PIPE2_EXITCODE "0" CACHE INTERNAL "" FORCE)
EOF

  # Keep stale curl state from bypassing updated triplet/overlay behavior.
  if [ -d "${VCPKG_ROOT}/buildtrees/curl" ]; then
    echo "Forcing clean build of curl to apply triplet changes..."
    rm -rf "${VCPKG_ROOT}/buildtrees/curl"
  fi
}

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
    if [ -n "${VCPKG_CURL_OVERLAY_PORT_DIR:-}" ]; then
      toolchain_args+=("-DVCPKG_OVERLAY_PORTS=${VCPKG_CURL_OVERLAY_PORT_DIR}")
    fi
    if [ "${NO_BINARY_CACHE}" -eq 1 ]; then
      toolchain_args+=("-DVCPKG_INSTALL_OPTIONS=--no-binarycaching")
    fi
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

ensure_vcpkg_curl_overlay
ensure_vcpkg_triplets

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
