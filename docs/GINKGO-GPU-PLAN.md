# ginkgo GPU (Adreno 610) plan

Status: research only, nothing implemented yet (2026-11-21).

## What topaz (Redmi Note 12 4G) already has

Three layers, all in this tree:

1. **Kernel**: `iokit/Kernel/arm/TopazAdreno.cpp` (power, PLL, zap shader through TrustZone PAS 13,
   GPU SMMU, CP start with SQE microcode, IB submission, fences) and `RedwoodGPU.cpp` (an msm-DRM
   style user client, from HackiOS). Boot-arg `gpu=1`.
2. **Mesa freedreno for Darwin**: `userland/mesa/` (`freedreno-darwin.patch`,
   `osmesa-es-profile.patch`, `redwood-drm`, `mk-mesa-mac.sh`). Needs macOS to build, so it has to
   become a GitHub Actions job like the kernel.
3. **iOS glue**: `userland/gpu/eagl.c`, `gl_osmesa.c`, `inject.c`, `bbd_wrap.c`: injected into
   backboardd (kernel `kern.exec_inject`) so that CoreAnimation renders through OpenGL ES on the GPU.

## What is the same on ginkgo (SM6125)

| Item | topaz | ginkgo |
|---|---|---|
| GPU | Adreno 610 | Adreno 610, `qcom,chipid = 0x06010000` |
| GPU registers | 0x05900000 | 0x05900000 |
| GPU SMMU | 0x059A0000 | 0x059A0000 (`kgsl-smmu@0x59a0000`) |
| GPU clock controller | 0x05990000 | 0x05990000 |
| GDSCs | GX 0x100c, CX 0x106c (status 0x1540) | same (gpucc-sm6125.c) |
| Branches | gfx3d 0x1054, cx_gmu 0x1098, cxo 0x109c, ahb 0x1078, smmu vote 0x5000 | same |
| GCC | 0x79004 bit 15 (GPLL0 to GPU CC), 0x71154, 0x3600c, 0x36018 | same (gcc-sm6125.c) |
| OPPs | 320 ... 980 MHz | 320, 465, 600, 745, 820, 900, 950 MHz |

The Mesa side keys on the chip id, so the user space should work as built for topaz.

## What differs (kernel work)

1. **GPU PLL**: topaz programs a Zonda-style PLL (`ZP_*`, CFG 0x08200800...). SM6125 has a default
   alpha PLL (clk-alpha-pll DEFAULT layout: MODE 0, L 4, ALPHA 8, ALPHA_U 0xc, USER_CTL 0x10,
   CONFIG_CTL 0x18). PLL0 at 0x0: `config_ctl 0x4001055b`, alpha_en BIT(24), aux2 output BIT(2),
   vco 0 (1-2 GHz); 600 MHz gfx3d = PLL0 1200 MHz (L 0x3e, alpha_hi 0x80) / 2.
   gfx3d RCG 0x101c, parent PLL0_OUT_AUX2 = 2 (PLL1_OUT_AUX2 = 4 on SM6125, 3 on SM6115).
   -> rewrite `gpu_pll_start()` for the default PLL.
2. **GX power-up**: SM6125 has no `gpu_cc_gx_cxo_clk` (0x1060) and no GX clamp (0x1508) / GX BCR in
   mainline: skip those writes.
3. **Zap shader**: signed per SoC; topaz embeds its own. Use ginkgo's `/vendor/firmware/a610_zap.*`
   (pull from the phone in recovery) and hand it over in the ADT like the touch firmware; the zap
   region is the DT `gpu_region` (`reg = <0x57515000 0x2000>`), not topaz's 0x55B15000.
   SQE microcode `a630_sqe.fw` likewise from `/vendor/firmware`.
4. **Memory**: topaz puts a 1 GB GPU pool at 0x90000000, outside the RAM xnu manages there. On ginkgo
   xnu manages 0x80000000-0x140000000, so split it: xnu 0x80000000 + 2 GB, GPU pool 0x100000000 + 1 GB
   (the GPU SMMU maps 36-bit addresses) or keep it below 4 GB if scanout needs it.
5. **Rails**: check whether the GX rail needs an RPM vote (TopazRpm.cpp) on trinket.

## Order of work

1. Shim probe (like eMMC/touch): power the GPU, lock the PLL, read the chip id register.
2. Kernel: adapt TopazAdreno.cpp (PLL, GX, zap from ADT, pool) and boot with `gpu=1`, check the
   `TopazGPU:` log until the CP runs and a fence completes.
3. CI job on macos-15 to build Mesa + eagl + inject for iOS; install into the root image.
4. Enable injection into backboardd; compare frame rate (`SWEET FPS`).
