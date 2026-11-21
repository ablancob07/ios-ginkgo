#!/bin/bash
# Build the ginkgo (Redmi Note 8) shim and wrap it in boot images.
#
# ginkgo's ABL is from the Android 9/10 era and the stock boot.img is header v0
# (Image.gz-dtb: DTBs appended to the kernel). We do not know yet which format its
# ABL accepts for a custom kernel, so we build both:
#   out/ginkgo-v0.img  header v0, uncompressed shim Image + all stock DTBs appended
#   out/ginkgo-v2.img  header v2, stock DTB in the header (like ios-sweet)
# Boot them with `fastboot boot` (RAM only, nothing is flashed).
#
# usage: build-ginkgo.sh [payload.bin]
#   env: DTBDIR (default ../../dtb/stock)   MKBOOTIMG (default AOSP mkbootimg.py)
set -euo pipefail
cd "$(dirname "$0")"
CC="clang --target=aarch64-none-elf"
LD=aarch64-linux-gnu-gcc
OBJCOPY=aarch64-linux-gnu-objcopy
CFLAGS="${EXTRA_CFLAGS:-} -O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -fno-jump-tables -Wall -Wextra -Wno-unused-command-line-argument"
DTBDIR=${DTBDIR:-../../../dtb/stock}
MKBOOTIMG=${MKBOOTIMG:-python3 /home/usuario/ios-port-ginkgo/dl/mkbootimg/src/mkbootimg.py}
SRCS="start.S shim.c console.c mmu.c fdt.c loader.c emmc.c touch.c"
mkdir -p out
OBJS=""
for f in $SRCS; do
	$CC $CFLAGS -c $f -o out/${f%.*}.o
	OBJS="$OBJS out/${f%.*}.o"
done
abs=$(aarch64-linux-gnu-readelf -r $(echo $OBJS | sed 's#out/start.o##') | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
$LD -O2 -ffreestanding -nostdlib -static -T shim.ld -Wl,--build-id=none -Wl,-N $OBJS -o out/shim.elf
$OBJCOPY -O binary out/shim.elf out/Image
size=$((16#$(aarch64-linux-gnu-nm out/shim.elf | awk '$3=="_end"{print $1}')))
truncate -s "$size" out/Image
if [ -n "${1:-}" ]; then
	truncate -s $(( (size + 0xfff) & ~0xfff )) out/Image
	cat "$1" >> out/Image
	python3 -c 'import os,struct,sys; f=open(sys.argv[1],"r+b"); f.seek(16); f.write(struct.pack("<Q", os.path.getsize(sys.argv[1])))' out/Image
fi
(cd out && rm -rf rd && mkdir rd && cd rd && find . | cpio -o -H newc --quiet | gzip -9 > ../ramdisk.bin)

COMMON="--base 0 --kernel_offset 0x8000 --ramdisk_offset 0x1000000 --tags_offset 0x100 --pagesize 4096 --cmdline console=null"
# v2: first stock DTB in the header
$MKBOOTIMG --header_version 2 --kernel out/Image --ramdisk out/ramdisk.bin \
	--dtb "$DTBDIR/stock_0.dtb" --dtb_offset 0x1f00000 $COMMON -o out/ginkgo-v2.img
# v0: DTBs appended to the (uncompressed) kernel, as the stock Image.gz-dtb does
cat out/Image "$DTBDIR/all.dtb" > out/Image-dtb
$MKBOOTIMG --header_version 0 --kernel out/Image-dtb --ramdisk out/ramdisk.bin $COMMON -o out/ginkgo-v0.img
ls -l out/Image out/ginkgo-v0.img out/ginkgo-v2.img
