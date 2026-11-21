#!/bin/bash
# Build the ginkgo boot image: ADT + kernel payload + shim.
#   tools/ginkgo-boot.sh [extra boot-args]
# Output: shim/out/ginkgo-v2.img (boot with `fastboot boot`, RAM only).
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=shim/out
mkdir -p $OUT
# RAM: 3 GiB at 0x80000000, all above the Qualcomm carve-outs (everything fixed is < 0x62000000)
MIDBASE=0x80000000 MIDSIZE=0xc0000000 python3 -I tools/mkadt.py $OUT/ginkgo.adt --midmem
# Boot-args. Everything topaz (SM6225) hardware is switched off: touch/keys/battery/charger
# poke other MMIO, direct MDP scanout programs another display block.
ARGS="debug=0xc serial=3 cpus=1 keepsyms=1 -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 \
wdt_test=no_user_checkin launchd_unsecure_cache=1 wdt=-1 sweetfb_hz=30 sweetfb_direct=0 \
sweettouch=0 sweetkeys=0 sweetbattery=0 sweetchg=0 sweetusb=0 sweethw=0 sweetbl=0 ufsrw=0 rootrw=0 ${*:-}"
python3 -I tools/mkpayload.py xnu/out/kernel.development.sweet $OUT/ginkgo.adt $OUT/payload.bin --cmdline "$ARGS"
shim/build-ginkgo.sh "$PWD/$OUT/payload.bin"
