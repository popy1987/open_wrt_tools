#!/bin/sh
# test_all_channel.sh — Sweep 2.4 / 5 / 6 GHz channels via cwc (lab / shield room).
# All user-facing messages are in English.
#
# Intended for OpenWrt (ash/busybox). Run as root on the router.
# Country codes are fixed per band so the driver/regdb will allow TX:
#   2.4 GHz -> CN (ch 1-13)
#   5 / 6 GHz -> US (widest coverage in cwc rules)

# Self-heal Windows CRLF if launched via: sh test_all_channel.sh ...
case "$(sed -n '1p' "$0" 2>/dev/null)" in
*$'\r')
	tmp="${0}.$$.lf"
	tr -d '\r' <"$0" >"$tmp" && chmod +x "$tmp" && exec sh "$tmp" "$@"
	;;
esac

set -eu

CWC="${CWC:-cwc}"
RADIO_2G="${RADIO_2G:-radio0}"
RADIO_5G="${RADIO_5G:-radio1}"
RADIO_6G="${RADIO_6G:-radio2}"
CC_2G="${CC_2G:-CN}"
CC_5G="${CC_5G:-US}"
CC_6G="${CC_6G:-US}"

DO_2G=1
DO_5G=1
DO_6G=1
SKIP_DFS=0
ONLY_DFS=0
DRY_RUN=0
DWELL_SEC=3
DFS_DWELL_SEC=90
ASSUME_YES=0
LOG_FILE=""

PASS=0
FAIL=0
SKIP=0

usage() {
	cat <<'EOF'
Usage: test_all_channel.sh [options]

Sweep Wi-Fi channels with cwc (Change Wi-Fi Channel). Designed for
shielded-chamber / lab use. A real ISO country code is still required
by the kernel; 00 (world domain) is NOT used (too restrictive for AP TX).

Options:
  -h, --help           Show this help
  -y, --yes            Do not prompt before starting
  --dry-run            Print commands only; do not call cwc
  --2g-only            Only sweep 2.4 GHz
  --5g-only            Only sweep 5 GHz
  --6g-only            Only sweep 6 GHz
  --skip-dfs           Skip 5 GHz DFS channels (faster smoke test)
  --only-dfs           Only sweep 5 GHz DFS channels
  --dwell SEC          Pause after each channel (default: 3)
  --dfs-dwell SEC      Extra pause after DFS channels (default: 90)
  --cwc PATH           Path to cwc binary (default: cwc on PATH)
  --radio-2g NAME      UCI radio for 2.4 GHz (default: radio0)
  --radio-5g NAME      UCI radio for 5 GHz   (default: radio1)
  --radio-6g NAME      UCI radio for 6 GHz   (default: radio2)
  --cc-2g CODE         Country for 2.4 GHz   (default: CN)
  --cc-5g CODE         Country for 5 GHz     (default: US)
  --cc-6g CODE         Country for 6 GHz     (default: US)
  --log FILE           Append a simple result log

Environment (same names as long options without --) also works, e.g.:
  RADIO_5G=radio1 DWELL_SEC=5 ./test_all_channel.sh -y

Examples:
  ./test_all_channel.sh -y --skip-dfs
  ./test_all_channel.sh -y --5g-only --only-dfs --dfs-dwell 120
  ./test_all_channel.sh --dry-run --2g-only
EOF
}

die() {
	printf 'ERROR: %s\n' "$*" >&2
	exit 1
}

log() {
	printf '%s\n' "$*"
	if [ -n "$LOG_FILE" ]; then
		printf '%s\n' "$*" >>"$LOG_FILE"
	fi
}

confirm_start() {
	if [ "$ASSUME_YES" -eq 1 ]; then
		return 0
	fi
	printf 'Start channel sweep on this router? [y/N]: '
	read -r ans || ans=""
	case "$ans" in
	y|Y|yes|YES) return 0 ;;
	*)
		log "Cancelled by user."
		exit 0
		;;
	esac
}

set_channel() {
	_cc=$1
	_radio=$2
	_ch=$3
	_tag=${4:-}

	_msg="SET country=${_cc} radio=${_radio} channel=${_ch}"
	[ -n "$_tag" ] && _msg="${_msg} [${_tag}]"

	if [ "$DRY_RUN" -eq 1 ]; then
		log "DRY-RUN: ${CWC} -c ${_cc} -i ${_radio} -n ${_ch} -y --no-reboot"
		PASS=$((PASS + 1))
		return 0
	fi

	log ">>> ${_msg}"
	if "${CWC}" -c "${_cc}" -i "${_radio}" -n "${_ch}" -y --no-reboot; then
		log "OK: ${_msg}"
		PASS=$((PASS + 1))
		return 0
	fi
	log "FAIL: ${_msg}"
	FAIL=$((FAIL + 1))
	return 0
}

pause_after() {
	_extra=$1
	_sec=$DWELL_SEC
	if [ "$_extra" -gt 0 ]; then
		_sec=$((DWELL_SEC + _extra))
	fi
	if [ "$_sec" -gt 0 ] && [ "$DRY_RUN" -eq 0 ]; then
		log "Waiting ${_sec}s before next channel..."
		sleep "$_sec"
	fi
}

sweep_2g() {
	log "======== 2.4 GHz (${RADIO_2G}, country ${CC_2G}, ch 1-13) ========"
	_ch=1
	while [ "$_ch" -le 13 ]; do
		set_channel "$CC_2G" "$RADIO_2G" "$_ch" "2.4G"
		pause_after 0
		_ch=$((_ch + 1))
	done
}

# Non-DFS 5 GHz (US table used by cwc)
CH_5G_NON_DFS="36 40 44 48 149 153 157 161 165 169 173 177"
# DFS 5 GHz
CH_5G_DFS="52 56 60 64 100 104 108 112 116 120 124 128 132 136 140 144"

sweep_5g() {
	log "======== 5 GHz (${RADIO_5G}, country ${CC_5G}) ========"

	if [ "$ONLY_DFS" -eq 0 ]; then
		log "--- 5 GHz non-DFS ---"
		for _ch in $CH_5G_NON_DFS; do
			set_channel "$CC_5G" "$RADIO_5G" "$_ch" "5G-non-DFS"
			pause_after 0
		done
	else
		log "Skipping non-DFS (--only-dfs)."
		SKIP=$((SKIP + 1))
	fi

	if [ "$SKIP_DFS" -eq 1 ]; then
		log "Skipping DFS channels (--skip-dfs)."
		SKIP=$((SKIP + 1))
		return 0
	fi

	log "--- 5 GHz DFS (CAC may take 1-10 min; dwell=${DFS_DWELL_SEC}s) ---"
	for _ch in $CH_5G_DFS; do
		set_channel "$CC_5G" "$RADIO_5G" "$_ch" "5G-DFS"
		pause_after "$DFS_DWELL_SEC"
	done
}

sweep_6g() {
	log "======== 6 GHz (${RADIO_6G}, country ${CC_6G}, ch 1..233 step 4) ========"
	_ch=1
	while [ "$_ch" -le 233 ]; do
		set_channel "$CC_6G" "$RADIO_6G" "$_ch" "6G"
		pause_after 0
		_ch=$((_ch + 4))
	done
}

# --- argv ---
while [ $# -gt 0 ]; do
	case "$1" in
	-h|--help)
		usage
		exit 0
		;;
	-y|--yes)
		ASSUME_YES=1
		;;
	--dry-run)
		DRY_RUN=1
		;;
	--2g-only)
		DO_2G=1
		DO_5G=0
		DO_6G=0
		;;
	--5g-only)
		DO_2G=0
		DO_5G=1
		DO_6G=0
		;;
	--6g-only)
		DO_2G=0
		DO_5G=0
		DO_6G=1
		;;
	--skip-dfs)
		SKIP_DFS=1
		;;
	--only-dfs)
		ONLY_DFS=1
		DO_2G=0
		DO_5G=1
		DO_6G=0
		;;
	--dwell)
		[ $# -ge 2 ] || die "--dwell requires SEC"
		DWELL_SEC=$2
		shift
		;;
	--dfs-dwell)
		[ $# -ge 2 ] || die "--dfs-dwell requires SEC"
		DFS_DWELL_SEC=$2
		shift
		;;
	--cwc)
		[ $# -ge 2 ] || die "--cwc requires PATH"
		CWC=$2
		shift
		;;
	--radio-2g)
		[ $# -ge 2 ] || die "--radio-2g requires NAME"
		RADIO_2G=$2
		shift
		;;
	--radio-5g)
		[ $# -ge 2 ] || die "--radio-5g requires NAME"
		RADIO_5G=$2
		shift
		;;
	--radio-6g)
		[ $# -ge 2 ] || die "--radio-6g requires NAME"
		RADIO_6G=$2
		shift
		;;
	--cc-2g)
		[ $# -ge 2 ] || die "--cc-2g requires CODE"
		CC_2G=$2
		shift
		;;
	--cc-5g)
		[ $# -ge 2 ] || die "--cc-5g requires CODE"
		CC_5G=$2
		shift
		;;
	--cc-6g)
		[ $# -ge 2 ] || die "--cc-6g requires CODE"
		CC_6G=$2
		shift
		;;
	--log)
		[ $# -ge 2 ] || die "--log requires FILE"
		LOG_FILE=$2
		shift
		;;
	*)
		die "unknown option: $1 (use -h)"
		;;
	esac
	shift
done

if [ "$(id -u)" -ne 0 ]; then
	die "run as root on the OpenWrt router"
fi

if [ "$DRY_RUN" -eq 0 ]; then
	command -v "$CWC" >/dev/null 2>&1 || die "cwc not found: ${CWC} (install binary or set --cwc)"
fi

if [ -n "$LOG_FILE" ]; then
	: >"$LOG_FILE" || die "cannot write log: ${LOG_FILE}"
fi

log "============================================================"
log "  cwc channel sweep (lab / shield room)"
log "============================================================"
log "cwc        : ${CWC}"
log "radios     : 2G=${RADIO_2G} 5G=${RADIO_5G} 6G=${RADIO_6G}"
log "countries  : 2G=${CC_2G} 5G=${CC_5G} 6G=${CC_6G}"
log "bands      : 2G=${DO_2G} 5G=${DO_5G} 6G=${DO_6G}"
log "DFS        : skip=${SKIP_DFS} only=${ONLY_DFS}"
log "dwell      : ${DWELL_SEC}s (+${DFS_DWELL_SEC}s extra on DFS)"
log "dry-run    : ${DRY_RUN}"
log "NOTE: Verify radio↔band mapping with: ${CWC} -p -s"
log "NOTE: Country 00 (world) is intentionally not used."
log "============================================================"

confirm_start

[ "$DO_2G" -eq 1 ] && sweep_2g
[ "$DO_5G" -eq 1 ] && sweep_5g
[ "$DO_6G" -eq 1 ] && sweep_6g

log "============================================================"
log "Summary: PASS=${PASS} FAIL=${FAIL} (skip-notes=${SKIP})"
log "Done. Suggested verify: ${CWC} -p -s"
log "============================================================"

if [ "$FAIL" -gt 0 ]; then
	exit 1
fi
exit 0
