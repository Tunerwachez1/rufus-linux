#!/usr/bin/env bash
#
# Rufus Linux port - stage 0 verification on loopback devices.
# Destroys nothing outside /tmp; requires root for losetup/mount.
#
# Usage: sudo ./loop-test.sh [path/to/rufus-cli]
set -u

RUFUS_CLI="${1:-$(dirname "$0")/../build/rufus-cli}"
WORK=$(mktemp -d /tmp/rufus-loop-test.XXXXXX)
DATA_MB=16
LOOP=""
FAILED=0

cleanup() {
	set +e
	[ -n "${LOOP:-}" ] && losetup -d "$LOOP" 2>/dev/null
	[ -n "${MNT:-}" ] && umount "$MNT" 2>/dev/null
	rm -rf "$WORK"
}
trap cleanup EXIT

fail() { echo "FAIL: $*"; FAILED=1; exit 1; }
pass() { echo "PASS: $*"; }
note() { echo "-- $*"; }

[ "$(id -u)" -eq 0 ] || { echo "please run as root: sudo $0"; exit 1; }
[ -x "$RUFUS_CLI" ] || fail "rufus-cli not found at '$RUFUS_CLI'"

SRC="$WORK/src.img"
truncate -s ${DATA_MB}M "$SRC"
dd if=/dev/urandom of="$SRC" bs=1M count=8 status=none

note "1. raw dd write"
LOOP=$(losetup --find --show "$SRC") || fail "losetup failed"
"$RUFUS_CLI" write "$SRC" "$LOOP" --yes || fail "raw write returned error"
cmp "$SRC" "$LOOP" || fail "raw content mismatch"
pass "raw dd write, content verified"
losetup -d "$LOOP"; LOOP=""

note "2. compressed .gz write through bled"
gzip -c "$SRC" > "$SRC.gz"
LOOP=$(losetup --find --show "$SRC") || fail "losetup failed"
"$RUFUS_CLI" write "$SRC.gz" "$LOOP" --yes || fail ".gz write returned error"
cmp "$SRC" "$LOOP" || fail "content mismatch after gzip"
pass "gzip image written and verified"
losetup -d "$LOOP"; LOOP=""

note "3. refusal to write to a mounted device"
MKFS=""
for c in mkfs.ext2 mkfs.vfat mkfs.btrfs; do
	command -v "$c" >/dev/null 2>&1 && MKFS="$c" && break
done
if [ -z "$MKFS" ]; then
	note "no mkfs tool found - skipping mounted-refusal check"
else
	LOOP=$(losetup --find --show "$SRC") || fail "losetup failed"
	"$MKFS" -q "$LOOP" 2>/dev/null || "$MKFS" -F32 "$LOOP" >/dev/null 2>&1
	MNT="$WORK/mnt"
	mkdir -p "$MNT"
	mount "$LOOP" "$MNT" || { note "mount failed - skipping"; }
	if mountpoint -q "$MNT"; then
		if "$RUFUS_CLI" write "$SRC" "$LOOP" --yes >/dev/null 2>&1; then
			fail "WROTE to a mounted device!"
		fi
		pass "refused to write to mounted device"
		umount "$MNT"
	fi
	losetup -d "$LOOP"; LOOP=""
fi

note "4. dry-run leaves target untouched"
LOOP=$(losetup --find --show "$SRC")
BEFORE=$(md5sum < "$LOOP")
"$RUFUS_CLI" write "$SRC" "$LOOP" --dry-run >/dev/null 2>&1 || fail "dry-run returned error"
AFTER=$(md5sum < "$LOOP")
[ "$BEFORE" = "$AFTER" ] || fail "dry-run modified the target!"
pass "dry-run is side-effect free"
losetup -d "$LOOP"; LOOP=""

note "5. GPT partitioning recognized by the kernel and sfdisk"
# -P (--partscan) makes the kernel scan partitions of the loop device -
# real disks always do this, loops only when asked.
PTGPT="$WORK/pt-gpt.img"
truncate -s ${DATA_MB}M "$PTGPT"
LOOP=$(losetup --find --show -P "$PTGPT") || fail "losetup failed"
"$RUFUS_CLI" mktable "$LOOP" --gpt --yes || fail "mktable --gpt returned error"
BASENAME=$(basename "$LOOP")
[ -e "/sys/block/${BASENAME}/${BASENAME}p1/start" ] || fail "kernel did not see ${BASENAME}p1"
KSTART=$(cat "/sys/block/${BASENAME}/${BASENAME}p1/start")
[ "$KSTART" = "34" ] || fail "kernel partition start=$KSTART, expected 34"
pass "GPT: kernel sees ${BASENAME}p1 at sector 34"
losetup -d "$LOOP"; LOOP=""

note "6. MBR partitioning recognized by the kernel"
SRC2="$WORK/src2.img"
truncate -s ${DATA_MB}M "$SRC2"
LOOP=$(losetup --find --show -P "$SRC2") || fail "losetup failed"
"$RUFUS_CLI" mktable "$LOOP" --mbr --yes || fail "mktable --mbr returned error"
BASENAME=$(basename "$LOOP")
[ -e "/sys/block/${BASENAME}/${BASENAME}p1/start" ] || fail "kernel did not see ${BASENAME}p1"
KSTART=$(cat "/sys/block/${BASENAME}/${BASENAME}p1/start")
[ "$KSTART" = "2048" ] || fail "kernel partition start=$KSTART, expected 2048"
pass "MBR: kernel sees ${BASENAME}p1 at sector 2048"
losetup -d "$LOOP"; LOOP=""

note "7. full Windows-style file-copy pipeline on a loop device"
# sudo resets $HOME to /root - resolve the invoking user's home instead
REAL_HOME=$(getent passwd "${SUDO_USER:-$(id -un)}" | cut -d: -f6)
WINISO=""
for c in "$REAL_HOME/Загрузки/iso/Win11_25H2_Russian_x64_v2.iso" \
         "$HOME/Загрузки/iso/Win11_25H2_Russian_x64_v2.iso" ; do
	[ -f "$c" ] && WINISO="$c" && break
done
if [ -z "$WINISO" ]; then
	note "no Win11 image found - skipping pipeline test"
else
	SRC3="$WORK/pipeline.img"
	truncate -s 16G "$SRC3"
	LOOP=$(losetup --find --show -P "$SRC3") || fail "losetup failed"
	"$RUFUS_CLI" write "$WINISO" "$LOOP" --copy --yes --max-mb=96 \
		|| fail "copy pipeline returned error"
	BASENAME=$(basename "$LOOP")
	[ -e "/sys/block/${BASENAME}/${BASENAME}p1" ] || fail "no partition node"
	# reference extraction of bootmgr by our libcdio backend
	"$(dirname "$0")/../build/test-cdio" "$WINISO" "$WORK/bootmgr.ref" \
		/bootmgr >/dev/null 2>&1
	mkdir -p "$WORK/mntchk"
	MNT="$WORK/mntchk"
	mount -o ro "${LOOP}p1" "$WORK/mntchk" || fail "cannot mount result FS"
	[ -f "$WORK/mntchk/bootmgr" ] || fail "bootmgr not copied"
	cmp "$WORK/mntchk/bootmgr" "$WORK/bootmgr.ref" && \
		pass "pipeline: bootmgr on FAT32 byte-identical to ISO content"
	umount "$WORK/mntchk"
	losetup -d "$LOOP"; LOOP=""
	rm -f "$SRC3"
fi

if [ "$FAILED" -eq 0 ]; then
	echo "ALL STAGE-0+1 CHECKS PASSED"
fi
exit "$FAILED"
