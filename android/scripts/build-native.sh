#!/usr/bin/env bash
# Cross-compiles Eclipse and its dependencies for Android arm64 (NVIDIA Shield) and copies the
# resulting shared libraries into android/app/src/main/jniLibs/arm64-v8a.
#
# Needs: ANDROID_NDK_HOME (NDK r26+), cmake, ninja, pkg-config, curl, tar, git, python3.
# STATUS: first draft, written without access to an NDK. Expect to fix version pins and flags the
# first time CI runs it. Every step is a plain function so a failure points at one library.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
: "${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME to the NDK directory}"
ABI="${ABI:-arm64-v8a}"
API="${API:-28}"
BUILD_TYPE="${BUILD_TYPE:-Release}"

WORK="${ROOT}/android/build"
SRC="${WORK}/src"
DEPS="${WORK}/deps"
OUT="${ROOT}/android/app/src/main/jniLibs/${ABI}"
JOBS="$(nproc 2>/dev/null || echo 4)"
TOOLCHAIN="${ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake"

mkdir -p "${SRC}" "${DEPS}" "${OUT}"

export PKG_CONFIG_LIBDIR="${DEPS}/lib/pkgconfig:${DEPS}/share/pkgconfig"
export PKG_CONFIG_PATH=""

COMMON_CMAKE_ARGS=(
    -G Ninja
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN}"
    -DANDROID_ABI="${ABI}"
    -DANDROID_PLATFORM="android-${API}"
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
    -DCMAKE_INSTALL_PREFIX="${DEPS}"
    -DCMAKE_PREFIX_PATH="${DEPS}"
    -DCMAKE_FIND_ROOT_PATH="${DEPS}"
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
)

fetch() { # name url -> extracts to ${SRC}/<name>
    local name=$1 url=$2
    if [[ -d "${SRC}/${name}" ]]; then return; fi
    echo "==> fetching ${name}"
    # Download to a file first: tar cannot auto-detect gzip/xz compression on a pipe.
    local archive="${WORK}/${name}.archive"
    curl -fsSL --retry 3 -o "${archive}" "${url}"
    mkdir -p "${SRC}/${name}"
    tar -xf "${archive}" --strip-components=1 -C "${SRC}/${name}"
    rm -f "${archive}"
}

cmake_dep() { # name extra-cmake-args...
    local name=$1; shift
    echo "==> building ${name}"
    cmake -S "${SRC}/${name}" -B "${WORK}/b-${name}" "${COMMON_CMAKE_ARGS[@]}" "$@"
    cmake --build "${WORK}/b-${name}" -j"${JOBS}" -- -k 0
    cmake --install "${WORK}/b-${name}"
}

# ---------------------------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------------------------
build_sdl2() {
    fetch SDL2 "https://github.com/libsdl-org/SDL/releases/download/release-2.30.8/SDL2-2.30.8.tar.gz"
    cmake_dep SDL2 -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF
}

build_freetype() {
    fetch freetype "https://github.com/freetype/freetype/archive/refs/tags/VER-2-13-2.tar.gz"
    cmake_dep freetype -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_PNG=ON \
        -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON -DBUILD_SHARED_LIBS=OFF
}

build_sdl2_image() {
    fetch SDL2_image "https://github.com/libsdl-org/SDL_image/releases/download/release-2.8.2/SDL2_image-2.8.2.tar.gz"
    cmake_dep SDL2_image -DBUILD_SHARED_LIBS=OFF -DSDL2IMAGE_VENDORED=OFF -DSDL2IMAGE_DEPS_SHARED=OFF \
        -DSDL2IMAGE_SAMPLES=OFF -DSDL2IMAGE_TESTS=OFF \
        -DSDL2IMAGE_BACKEND_STB=ON -DSDL2IMAGE_PNG=ON -DSDL2IMAGE_JPG=ON \
        -DSDL2IMAGE_AVIF=OFF -DSDL2IMAGE_JXL=OFF -DSDL2IMAGE_TIF=OFF -DSDL2IMAGE_WEBP=OFF \
        -DSDL2_DIR="${DEPS}/lib/cmake/SDL2"
}

build_expat() {
    fetch expat "https://github.com/libexpat/libexpat/releases/download/R_2_6_2/expat-2.6.2.tar.xz"
    cmake_dep expat -DEXPAT_BUILD_TOOLS=OFF -DEXPAT_BUILD_EXAMPLES=OFF -DEXPAT_BUILD_TESTS=OFF \
        -DEXPAT_BUILD_DOCS=OFF -DEXPAT_SHARED_LIBS=OFF
}

build_mbedtls() {
    fetch mbedtls "https://github.com/Mbed-TLS/mbedtls/archive/refs/tags/v2.28.8.tar.gz"
    cmake_dep mbedtls -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
        -DUSE_STATIC_MBEDTLS_LIBRARY=ON
}

build_curl() {
    fetch curl "https://github.com/curl/curl/releases/download/curl-8_9_1/curl-8.9.1.tar.xz"
    cmake_dep curl -DBUILD_SHARED_LIBS=OFF -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF \
        -DCURL_USE_MBEDTLS=ON -DCURL_USE_OPENSSL=OFF -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF \
        -DUSE_NGHTTP2=OFF -DCURL_DISABLE_LDAP=ON -DCURL_DISABLE_LDAPS=ON -DCURL_ZLIB=OFF \
        -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DCURL_CA_BUNDLE=none -DCURL_CA_PATH=none
}

build_opus() {
    fetch opus "https://github.com/xiph/opus/releases/download/v1.5.2/opus-1.5.2.tar.gz"
    cmake_dep opus -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_BUILD_TESTING=OFF -DBUILD_SHARED_LIBS=OFF \
        -DOPUS_INSTALL_PKG_CONFIG_MODULE=ON -DOPUS_INSTALL_CMAKE_CONFIG_MODULE=ON
}

build_inih() {
    fetch inih "https://github.com/benhoyt/inih/archive/refs/tags/r58.tar.gz"
    if [[ -f "${DEPS}/lib/libinih.a" ]]; then return; fi
    echo "==> building inih"
    local cc="${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android${API}-clang"
    local ar="${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-ar"
    "${cc}" -O2 -fPIC -c "${SRC}/inih/ini.c" -o "${WORK}/ini.o"
    "${ar}" rcs "${DEPS}/lib/libinih.a" "${WORK}/ini.o"
    mkdir -p "${DEPS}/include" "${DEPS}/lib/pkgconfig"
    cp "${SRC}/inih/ini.h" "${DEPS}/include/"
    cat > "${DEPS}/lib/pkgconfig/inih.pc" <<EOF
prefix=${DEPS}
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: inih
Description: INI parser
Version: 58
Libs: -L\${libdir} -linih
Cflags: -I\${includedir}
EOF
}

build_uuid_shim() {
    # Android has no libuuid. Eclipse only needs uuid_generate_random() and uuid_unparse(), so
    # provide a tiny header-only replacement plus a pkg-config file for pkg_check_modules(uuid).
    echo "==> installing uuid shim"
    mkdir -p "${DEPS}/include/uuid" "${DEPS}/lib/pkgconfig"
    cat > "${DEPS}/include/uuid/uuid.h" <<'EOF'
#pragma once
#include <stdio.h>
#include <stdlib.h>

typedef unsigned char uuid_t[16];

static inline void uuid_generate_random(uuid_t out) {
    arc4random_buf(out, 16);
    out[6] = (unsigned char) ((out[6] & 0x0f) | 0x40); /* version 4 */
    out[8] = (unsigned char) ((out[8] & 0x3f) | 0x80); /* RFC 4122 variant */
}

static inline void uuid_unparse(const uuid_t in, char *out) {
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             in[0], in[1], in[2], in[3], in[4], in[5], in[6], in[7],
             in[8], in[9], in[10], in[11], in[12], in[13], in[14], in[15]);
}
EOF
    cat > "${DEPS}/lib/pkgconfig/uuid.pc" <<EOF
prefix=${DEPS}
includedir=\${prefix}/include
Name: uuid
Description: Header-only UUID shim for Android
Version: 2.38
Cflags: -I\${includedir}
EOF
}

# ---------------------------------------------------------------------------------------------
# Eclipse
# ---------------------------------------------------------------------------------------------
prepare_eclipse_sources() {
    echo "==> initialising Eclipse submodules"
    (cd "${ROOT}" && bash scripts/bootstrap_client_deps.sh)

    # ss4s dlopen()s "ss4s-<name>.so", but Android only extracts APK libraries called lib*.so.
    # Make the module loader use the lib prefix on Android. Idempotent.
    local header="${ROOT}/third_party/ss4s/src/module_impl.h"
    if ! grep -q 'libss4s-%s' "${header}"; then
        python3 - "${header}" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = '    snprintf(out, outLen, "ss4s-%s%s", name, SS4S_LIBRARY_SUFFIX);'
new = ('#ifdef __ANDROID__\n'
       '    snprintf(out, outLen, "libss4s-%s%s", name, SS4S_LIBRARY_SUFFIX);\n'
       '#else\n'
       '    snprintf(out, outLen, "ss4s-%s%s", name, SS4S_LIBRARY_SUFFIX);\n'
       '#endif')
assert old in s, "ModuleFileName not found in module_impl.h"
open(p, 'w').write(s.replace(old, new, 1))
PY
    fi
}

build_eclipse() {
    echo "==> building Eclipse (libmain.so + decoder module)"
    # bionic has pthread built into libc; provide empty stubs so a plain -lpthread / -lrt still links.
    local ar="${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-ar"
    for stub in pthread rt; do
        rm -f "${DEPS}/lib/lib${stub}.a"
        "${ar}" rcs "${DEPS}/lib/lib${stub}.a"
    done
    cmake -S "${ROOT}" -B "${WORK}/b-eclipse" "${COMMON_CMAKE_ARGS[@]}" \
        -DCMAKE_INSTALL_PREFIX="${WORK}/eclipse-install" \
        -DCMAKE_SHARED_LINKER_FLAGS="-L${DEPS}/lib" \
        -DCMAKE_MODULE_LINKER_FLAGS="-L${DEPS}/lib" \
        -DCMAKE_EXE_LINKER_FLAGS="-L${DEPS}/lib" \
        -DBUILD_TESTS=OFF -DUSE_WERROR=OFF \
        -DCMAKE_C_FLAGS="-Wno-error=format-security -Wno-format-security"
    cmake --build "${WORK}/b-eclipse" -j"${JOBS}" -- -k 0
}

collect_libs() {
    echo "==> collecting shared libraries into ${OUT}"
    rm -f "${OUT}"/*.so
    local main sdl mod
    echo "-- shared objects found in the Eclipse build tree:"
    find "${WORK}/b-eclipse" \( -name '*.so' -o -name 'main*' \) -type f | head -40
    main="$(find "${WORK}/b-eclipse" -name 'libmain.so' | head -n1)"
    sdl="$(find "${DEPS}" -name 'libSDL2.so' | head -n1)"
    mod="$(find "${WORK}/b-eclipse" -name 'ss4s-mediacodec.so' | head -n1)"
    [[ -n "${main}" ]] || { echo "libmain.so was not produced" >&2; exit 1; }
    [[ -n "${sdl}" ]] || { echo "libSDL2.so was not produced" >&2; exit 1; }
    [[ -n "${mod}" ]] || { echo "ss4s-mediacodec.so was not produced" >&2; exit 1; }
    cp "${main}" "${OUT}/libmain.so"
    # Ship every library libmain.so links against that we built ourselves (e.g. libmicrodns.so).
    local readelf="${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf"
    local need found
    for need in $("${readelf}" -d "${main}" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
        echo "   libmain.so needs ${need}"
        case "${need}" in
            libc.so|libm.so|libdl.so|liblog.so|libandroid.so|libz.so|libSDL2.so|libhidapi.so|libGLESv1_CM.so|libGLESv2.so|libEGL.so|libOpenSLES.so|libmediandk.so) continue ;;
        esac
        found="$(find "${WORK}/b-eclipse" "${DEPS}" -name "${need}" | head -n1)"
        if [[ -n "${found}" ]]; then cp "${found}" "${OUT}/${need}"; else echo "WARNING: ${need} not found" >&2; fi
    done
    cp "${sdl}" "${OUT}/libSDL2.so"
    cp "${mod}" "${OUT}/libss4s-mediacodec.so"
    # Optional: SDL's separate HID library, if this SDL build produced one.
    local hid
    hid="$(find "${DEPS}" -name 'libhidapi.so' | head -n1 || true)"
    if [[ -n "${hid}" ]]; then cp "${hid}" "${OUT}/libhidapi.so"; fi
    # The SS4S "sdl" audio module is required for sound.
    local audio
    audio="$(find "${WORK}/b-eclipse" -name 'ss4s-sdl.so' | head -n1 || true)"
    if [[ -n "${audio}" ]]; then
        cp "${audio}" "${OUT}/libss4s-sdl.so"
    else
        echo "WARNING: ss4s-sdl.so (audio) was not built; the stream will be silent" >&2
    fi
    ls -l "${OUT}"
}

fetch_sdl_java() {
    # SDLActivity and friends are Java sources shipped inside the SDL release tarball.
    local dest="${ROOT}/android/app/src/main/java/org/libsdl/app"
    mkdir -p "${dest}"
    cp "${SRC}/SDL2/android-project/app/src/main/java/org/libsdl/app/"*.java "${dest}/"
}

build_sdl2
build_freetype
build_sdl2_image
build_expat
build_mbedtls
build_curl
build_opus
build_inih
build_uuid_shim
fetch_sdl_java
prepare_eclipse_sources
build_eclipse
collect_libs
echo "==> native build finished"
