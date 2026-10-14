# Selendang OP7 Kernel

A custom kernel for **OnePlus 7 (guacamoleb, Snapdragon 855 / SM8150)**,
maintained in deani77/lineageos_kernel_oneplus_sm8150.

- Initial development version: **0.1.0-dev**.
- ROM target: **LineageOS**, following its OnePlus 7 source branch.
- Current source baseline: **lineage-24.0**.
- Added CPU governors: despair, elementalx, intelliactive and blu_active.
- Status: development; governor object checks passed, full kernel and device
  validation tracked separately. Phone testing is pending display repair.

This repository retains the LineageOS history, original contributors' credits
and the upstream kernel licenses. "Selendang OP7 Kernel" identifies our custom
build; it does not imply an official LineageOS release.

## Builds

The Linux build script uses the same SM8150 base and Oplus configuration
fragment as the LineageOS device tree, preserving existing LTO/CFI choices.
With clang, LLVM tools, an ARM64 linker and ARM64/ARM32 binutils on PATH:

```sh
SELENDANG_DTC=/path/to/lineage-dtc/dtc bash build-selendang.sh
```

GitHub Actions provides a Linux build using LLVM 18 on Ubuntu 24.04 and a
pinned LineageOS external device-tree compiler (1.7.0). This matches the
LineageOS build approach: pass DTC_EXT rather than use the bundled 1.4.4 parser. A build
uploads logs and metadata, and uploads kernel binaries only after a successful
full build. Build artifacts include Image, Image.gz-dtb, DTBs and modules where
produced; they are raw development artifacts, not a flashable installer.

The kernel release suffix is `-Selendang-OP7-v0.1.0-dev`, followed by source
revision information when kernel version generation provides it. Build metadata
records the commit, compiler and configuration checksum.

## LineageOS integration

Build the matching LineageOS branch for `guacamoleb` and use this repository at
`kernel/oneplus/sm8150`. Keep its device tree, vendor files and firmware aligned
with that LineageOS version. The device-specific ROM build supplies boot image
and DTBO packaging; those artifacts must be validated before a public release.

Development is currently on `port-legacy-governors`, reviewed in PR #1. The
upstream baseline branch remains available for comparison. Update this baseline
when LineageOS moves the OnePlus 7 to another source branch, after checking device
configuration and driver compatibility.

See [governor port analysis](Documentation/cpu-freq/sm8150-legacy-governors.md)
for tunables, behavior differences and the completed host checks.

Before calling a release device-tested, validate booting, governor switching,
CPU hotplug, suspend/resume, touch boosting, temperature and battery behavior on
the repaired OnePlus 7. No stable or device-tested release has been published.
