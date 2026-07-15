#!/bin/sh
# Build json-c + libubox + libuci into OpenWrt SDK staging_dir (cross, aarch64).
#
# Usage:
#   ./build_libuci_staging.sh /path/to/openwrt-sdk-25.12.5-...

set -eu

SDK="${1:-}"
if [ -z "${SDK}" ] || [ ! -d "${SDK}/staging_dir" ]; then
	echo "usage: $0 /path/to/openwrt-sdk-..." >&2
	exit 1
fi

SDK="$(CDPATH= cd -- "${SDK}" && pwd)"
STAGING_SUFFIX="target-aarch64_cortex-a53"
STAGING="${SDK}/staging_dir/${STAGING_SUFFIX}"
TOOLCHAIN="$(ls -d "${SDK}"/staging_dir/toolchain-* 2>/dev/null | head -1 || true)"
HOST_STAGING="${SDK}/staging_dir/host"
BUILD="${SDK}/.cwc-libuci-build"
JOBS="$(nproc 2>/dev/null || echo 1)"

die() {
	echo "ERROR: $*" >&2
	exit 1
}

need_cmd() {
	command -v "$1" >/dev/null 2>&1 || die "missing command: $1 (sudo apt install -y $1)"
}

[ -n "${TOOLCHAIN}" ] || die "no toolchain under ${SDK}/staging_dir/"
need_cmd git
need_cmd cmake
need_cmd make
need_cmd pkg-config

find_cross_cc() {
	_cc=""
	for _cc in "${TOOLCHAIN}"/bin/*-openwrt-linux*-gcc; do
		[ -x "${_cc}" ] || continue
		case "${_cc}" in
			*-gcc-ar|*-gcc-ranlib|*-gcc-nm|*-gcc-gcov*) continue ;;
		esac
		echo "${_cc}"
		return 0
	done
	for _cc in "${TOOLCHAIN}"/bin/*-gcc; do
		[ -x "${_cc}" ] || continue
		case "${_cc}" in
			*-gcc-ar|*-gcc-ranlib|*-gcc-nm|*-gcc-gcov*) continue ;;
		esac
		echo "${_cc}"
		return 0
	done
	return 1
}

# NEVER use staging_dir/host/bin/pkg-config (shell wrapper -> /bin/pkg-config.real).
setup_pkg_config() {
	PKG_CONFIG=""

	if [ -x "${HOST_STAGING}/bin/pkg-config.real" ] && \
	   "${HOST_STAGING}/bin/pkg-config.real" --version >/dev/null 2>&1; then
		PKG_CONFIG="${HOST_STAGING}/bin/pkg-config.real"
	fi

	if [ -z "${PKG_CONFIG}" ]; then
		PKG_CONFIG="$(command -v pkg-config)"
	fi

	export PKG_CONFIG
	export PKG_CONFIG_SYSROOT_DIR="${STAGING}"
	export PKG_CONFIG_LIBDIR="${STAGING}/usr/lib/pkgconfig"
	export PKG_CONFIG_PATH="${STAGING}/usr/lib/pkgconfig"

	"${PKG_CONFIG}" --version >/dev/null 2>&1 || \
		die "pkg-config not working: ${PKG_CONFIG}"
}

CC="$(find_cross_cc || true)"
[ -n "${CC}" ] || die "cross gcc not found in ${TOOLCHAIN}/bin"

CC_PREFIX="$(basename "${CC}")"
CC_PREFIX="${CC_PREFIX%-gcc}"
export PATH="${TOOLCHAIN}/bin:${PATH}"
export CC
export AR="${TOOLCHAIN}/bin/${CC_PREFIX}-gcc-ar"
export RANLIB="${TOOLCHAIN}/bin/${CC_PREFIX}-gcc-ranlib"
[ -x "${AR}" ] || AR="${TOOLCHAIN}/bin/${CC_PREFIX}-ar"
[ -x "${RANLIB}" ] || RANLIB="${TOOLCHAIN}/bin/${CC_PREFIX}-ranlib"
export AR RANLIB

export CFLAGS="-Os -pipe -I${STAGING}/usr/include -I${STAGING}/usr/include/json-c"
export LDFLAGS="-L${STAGING}/usr/lib"
export STAGING_DIR="${STAGING}"

setup_pkg_config

echo "==> SDK=${SDK}"
echo "==> STAGING=${STAGING}"
echo "==> CC=${CC}"
echo "==> PKG_CONFIG=${PKG_CONFIG}"

rm -rf "${BUILD}"
mkdir -p "${BUILD}"
cd "${BUILD}"

_cmake() {
	if [ -x "${HOST_STAGING}/bin/cmake" ]; then
		"${HOST_STAGING}/bin/cmake" "$@"
	else
		cmake "$@"
	fi
}

cmake_cross() {
	_cmake \
		-DCMAKE_INSTALL_PREFIX=/usr \
		-DCMAKE_SYSTEM_NAME=Linux \
		-DCMAKE_SYSROOT="${STAGING}" \
		-DCMAKE_FIND_ROOT_PATH="${STAGING}" \
		-DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
		-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
		-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
		-DCMAKE_C_COMPILER="${CC}" \
		-DCMAKE_AR="${AR}" \
		-DCMAKE_RANLIB="${RANLIB}" \
		-DCMAKE_C_FLAGS="${CFLAGS}" \
		-DCMAKE_EXE_LINKER_FLAGS="${LDFLAGS}" \
		-DCMAKE_SHARED_LINKER_FLAGS="${LDFLAGS}" \
		"$@"
}

json_c_ready() {
	[ -f "${STAGING}/usr/lib/pkgconfig/json-c.pc" ] && \
		{ [ -f "${STAGING}/usr/lib/libjson-c.so" ] || \
		  ls "${STAGING}/usr/lib/libjson-c.so"* >/dev/null 2>&1; }
}

# libubox: drop FindPkgConfig/json-c/lua/examples (BUILD_LUA option is ignored by upstream).
patch_libubox_cmake() {
	_f="libubox/CMakeLists.txt"
	[ -f "${_f}" ] || die "missing ${_f}"

	cp "${_f}" "${_f}.orig"
	sed -i \
		-e 's/INCLUDE(FindPkgConfig)/# INCLUDE(FindPkgConfig)/' \
		-e 's/PKG_SEARCH_MODULE(JSONC json-c REQUIRED)/# PKG_SEARCH_MODULE(JSONC json-c REQUIRED)/' \
		-e "s|INCLUDE_DIRECTORIES(\${JSONC_INCLUDE_DIRS})|INCLUDE_DIRECTORIES(${STAGING}/usr/include ${STAGING}/usr/include/json-c)|" \
		-e 's/ADD_SUBDIRECTORY(lua)/# ADD_SUBDIRECTORY(lua)/' \
		-e 's/ADD_SUBDIRECTORY(examples)/# ADD_SUBDIRECTORY(examples)/' \
		-e 's/^find_library(json NAMES json-c)$/# find_library(json NAMES json-c)/' \
		"${_f}"

	grep -q '# PKG_SEARCH_MODULE(JSONC json-c REQUIRED)' "${_f}" || \
		die "failed to patch libubox CMakeLists.txt"
	grep -q '# find_library(json NAMES json-c)' "${_f}" || \
		die "failed to patch libubox json targets"
	echo "==> patched libubox/CMakeLists.txt (ubox only; no blobmsg_json/lua)"
}

build_json_c() {
	if json_c_ready; then
		echo "==> json-c already in staging"
		return 0
	fi
	echo "==> clone json-c (libubox dependency) ..."
	git clone --depth 1 https://github.com/json-c/json-c.git
	mkdir -p json-c/build
	(
		cd json-c/build
		cmake_cross \
			-DBUILD_APPS=OFF \
			-DBUILD_SHARED_LIBS=ON \
			..
		make -j"${JOBS}"
		make install DESTDIR="${STAGING}"
	)
	json_c_ready || die "json-c install failed"
}

build_libubox() {
	if [ -f "${STAGING}/usr/lib/libubox.so" ] || \
	   ls "${STAGING}/usr/lib/libubox.so"* >/dev/null 2>&1; then
		echo "==> libubox already in staging"
		return 0
	fi
	json_c_ready || die "json-c required before libubox"

	echo "==> clone libubox ..."
	git clone --depth 1 https://git.openwrt.org/project/libubox.git
	patch_libubox_cmake

	mkdir -p libubox/build
	(
		cd libubox/build
		cmake_cross -DBUILD_LUA=off -DBUILD_EXAMPLES=off ..
		make -j"${JOBS}"
		make install DESTDIR="${STAGING}"
	)
}

build_uci() {
	echo "==> clone uci ..."
	git clone --depth 1 https://git.openwrt.org/project/uci.git
	mkdir -p uci/build
	(
		cd uci/build
		cmake_cross \
			-DBUILD_LUA=off \
			-Dubox_include_dir="${STAGING}/usr/include" \
			..
		make -j"${JOBS}"
		make install DESTDIR="${STAGING}"
	)
}

build_json_c
build_libubox
build_uci

[ -f "${STAGING}/usr/include/uci.h" ] || die "uci.h still missing after cmake build"
echo "==> OK: ${STAGING}/usr/include/uci.h"
ls -l "${STAGING}/usr/lib/libuci.so"* 2>/dev/null || true
