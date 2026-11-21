#!/bin/sh
# Read the persistent SWEET console log from a ginkgo booted into recovery (adb root).
#   tools/ginkgo-log-pull.sh OUT.txt
# eMMC layout: the log occupies the LAST (1 + 4 MiB / 512) 512-byte sectors of userdata:
# sector 0 = header ("SWEETLOG"...), then the 4 MiB ring. Read-only.
set -eu
out=${1:?usage: ginkgo-log-pull.sh OUT.txt}
dev=$(adb shell 'for d in /dev/block/by-name/userdata /dev/block/bootdevice/by-name/userdata; do [ -e $d ] && readlink -f $d && break; done' | tr -d '\r')
[ -n "$dev" ] || { echo "userdata block device not found" >&2; exit 1; }
bytes=$(adb shell blockdev --getsize64 "$dev" | tr -d '\r')
sectors=8193
skip=$((bytes / 512 - sectors))
adb exec-out "dd if=$dev bs=512 skip=$skip count=$sectors 2>/dev/null" > "$out.bin"
echo "$dev: $bytes bytes, read sectors $skip.. ($(stat -c %s "$out.bin") bytes)" >&2
python3 -I "$(dirname "$0")/sweet-log.py" "$out.bin" "$out" 512
