#!/bin/sh
# Cross-build cwc for BPI-R4 (OpenWrt 25.12.5, mediatek/filogic aarch64)
#
# Layout (SDK folder must sit next to this script):
#   openwrt/bpi-r4/
#     build.sh
#     Makefile  cwc.c  ...
#     openwrt-sdk-25.12.5-mediatek-filogic_gcc-14.3.0_musl.Linux-x86_64/
#
# Usage:
#   ./build.sh
#   ./build.sh clean
#   ROUTER=192.168.1.1 ./build.sh install

set -eu

SDK_NAME="openwrt-sdk-25.12.5-mediatek-filogic_gcc-14.3.0_musl.Linux-x86_64"
STAGING_SUFFIX="target-aarch64_cortex-a53"
TARGET="cwc"

ROOT="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"

# Override: SDK_DIR=/path/to/openwrt-sdk-... ./build.sh
if [ -n "${SDK_DIR:-}" ]; then
	SDK="${SDK_DIR}"
elif [ -d "${ROOT}/${SDK_NAME}" ]; then
	SDK="${ROOT}/${SDK_NAME}"
elif [ -d "${HOME}/openwrt-sdk/${SDK_NAME}" ]; then
	SDK="${HOME}/openwrt-sdk/${SDK_NAME}"
else
	SDK="${ROOT}/${SDK_NAME}"
fi

STAGING="${SDK}/staging_dir/${STAGING_SUFFIX}"
TAR_ZST="${ROOT}/${SDK_NAME}.tar.zst"

die() {
	echo "ERROR: $*" >&2
	exit 1
}

need_cmd() {
	command -v "$1" >/dev/null 2>&1 || die "missing command: $1"
}

ensure_sdk() {
	if [ -d "${SDK}/staging_dir" ]; then
		return 0
	fi

	if [ -f "${TAR_ZST}" ]; then
		need_cmd zstd
		need_cmd tar
		echo "==> Extracting ${TAR_ZST} ..."
		tar --zstd -xf "${TAR_ZST}" -C "${ROOT}"
	fi

	[ -d "${SDK}/staging_dir" ] || die "SDK not found: ${SDK}
Place extracted folder next to build.sh, or put ${SDK_NAME}.tar.zst here."
}

verify_sdk() {
	_tc="$(ls -d "${SDK}/staging_dir/toolchain-"* 2>/dev/null | head -1 || true)"
	[ -n "${_tc}" ] || die "toolchain not found under ${SDK}/staging_dir/"
}

check_sdk_host_deps() {
	_missing=""
	if ! command -v gawk >/dev/null 2>&1; then
		_missing="${_missing} gawk"
	fi
	if ! echo '#include <ncurses.h>' | gcc -E - >/dev/null 2>&1; then
		_missing="${_missing} libncurses-dev"
	fi
	if [ -n "${_missing}" ]; then
		die "OpenWrt SDK host deps missing:${_missing}

Install on Ubuntu/Debian:
  sudo apt install -y gawk libncurses-dev rsync unzip patch python3 python3-distutils"
	fi
}

# OpenWrt SDK ships toolchain only; libuci headers/libs are installed into
# staging_dir after building uci once (SDK feeds) or build_libuci_staging.sh.
prepare_staging_deps() {
	if [ -f "${STAGING}/usr/include/uci.h" ] && \
	   { [ -f "${STAGING}/usr/lib/libuci.so" ] || \
	     ls "${STAGING}/usr/lib/libuci.so"* >/dev/null 2>&1; }; then
		return 0
	fi

	check_sdk_host_deps

	echo "==> staging has no libuci yet (normal for fresh SDK)"
	echo "==> building libuci + libubox into staging (one-time) ..."

	if [ -x "${SDK}/scripts/feeds" ] && [ "${SKIP_SDK_FEEDS:-0}" != 1 ]; then
		echo "==> try OpenWrt SDK package (feeds update + uci compile) ..."
		cd "${SDK}"
		./scripts/feeds update -a
		_j="$(nproc 2>/dev/null || echo 1)"
		for _t in \
			package/feeds/base/uci/compile \
			package/feeds/base/libuci/compile \
			package/system/uci/compile; do
			echo "==> make ${_t}"
			if make "${_t}" -j"${_j}" V=s; then
				break
			fi
		done
		cd "${ROOT}"
	fi

	if [ ! -f "${STAGING}/usr/include/uci.h" ]; then
		echo "==> SDK package route failed or skipped; using cmake fallback ..."
		_libuci="${ROOT}/build_libuci_staging.sh"
		[ -x "${_libuci}" ] || _libuci="${0%/*}/build_libuci_staging.sh"
		[ -f "${_libuci}" ] || die "missing build_libuci_staging.sh next to build.sh"
		sh "${_libuci}" "${SDK}"
	fi

	if [ ! -f "${STAGING}/usr/include/uci.h" ]; then
		die "libuci dev files still missing.

Try manually:
  cd ${SDK}
  ./scripts/feeds update -a
  find package feeds -path '*/uci/Makefile'
  make package/feeds/base/uci/compile V=s

Or cmake fallback:
  ${ROOT}/build_libuci_staging.sh ${SDK}

Then re-run: ${0}"
	fi
	echo "==> libuci ready: ${STAGING}/usr/include/uci.h"
}

do_build() {
	echo "==> ROOT=${ROOT}"
	echo "==> SDK=${SDK}"
	echo "==> STAGING_DIR=${STAGING}"
	echo "==> Building ${TARGET} ..."
	(
		cd "${ROOT}"
		make STAGING_DIR="${STAGING}" clean all
	)
}

verify_binary() {
	[ -x "${ROOT}/${TARGET}" ] || die "build failed: ${ROOT}/${TARGET} not found"
	if command -v file >/dev/null 2>&1; then
		echo "==> file ${TARGET}:"
		file "${ROOT}/${TARGET}"
		file "${ROOT}/${TARGET}" | grep -q 'aarch64' || \
			die "wrong architecture (expected aarch64)"
	fi
	ls -lh "${ROOT}/${TARGET}"
}

do_install() {
	_r="${ROUTER:-}"
	[ -n "${_r}" ] || die "set ROUTER=ip for install (e.g. ROUTER=192.168.1.1 ./build.sh install)"
	need_cmd scp
	need_cmd ssh
	echo "==> Installing to root@${_r}:/usr/local/bin/${TARGET}"
	ssh "root@${_r}" "mkdir -p /usr/local/bin"
	scp "${ROOT}/${TARGET}" "root@${_r}:/usr/local/bin/${TARGET}"
	ssh "root@${_r}" "chmod 755 /usr/local/bin/${TARGET} && ${TARGET} -h"
}

ACTION="${1:-build}"

case "${ACTION}" in
	build|"")
		need_cmd make
		ensure_sdk
		verify_sdk
		prepare_staging_deps
		do_build
		verify_binary
		echo ""
		echo "OK: ${ROOT}/${TARGET}"
		echo "Deploy: ROUTER=192.168.1.1 ${0} install"
		echo "        scp ${ROOT}/${TARGET} root@ROUTER:/usr/local/bin/"
		;;
	clean)
		(
			cd "${ROOT}"
			if [ -d "${SDK}/staging_dir" ]; then
				make STAGING_DIR="${STAGING}" clean
			else
				make clean 2>/dev/null || rm -f cwc *.o
			fi
		)
		echo "clean done"
		;;
	install)
		need_cmd make
		ensure_sdk
		verify_sdk
		prepare_staging_deps
		[ -x "${ROOT}/${TARGET}" ] || do_build
		verify_binary
		do_install
		;;
	*)
		die "usage: $0 [build|clean|install]"
		;;
esac
