#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

kernel_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
if [[ $(uname -s) != Linux ]]; then
    echo 'Full Selendang OP7 builds require a Linux build host.' >&2
    exit 1
fi
cd "$kernel_root"
build_out=${SELENDANG_OUT:-"$kernel_root/out/selendang-op7"}
artifact_out=${SELENDANG_ARTIFACTS:-"$kernel_root/out/selendang-artifacts"}
build_jobs=${SELENDANG_JOBS:-$(nproc)}
mkdir -p "$build_out" "$artifact_out"
build_out=$(realpath "$build_out")
artifact_out=$(realpath "$artifact_out")
for tool in clang ld.lld llvm-ar llvm-nm llvm-objcopy llvm-objdump \
            llvm-readelf llvm-strip aarch64-linux-gnu-as arm-linux-gnueabi-as; do
    command -v "$tool" >/dev/null || { echo "Missing build tool: $tool" >&2; exit 1; }
done
export KBUILD_BUILD_USER=Selendang
export KBUILD_BUILD_HOST=OP7
build_args=(O="$build_out" ARCH=arm64 LLVM=1 LLVM_IAS=1
            CROSS_COMPILE=aarch64-linux-gnu-
            CROSS_COMPILE_ARM32=arm-linux-gnueabi-)
{
    echo 'Selendang OP7 Kernel 0.1.0-dev'
    echo 'ROM source target: LineageOS lineage-24.0 / guacamoleb'
    git rev-parse HEAD
    clang --version
    ld.lld --version
} > "$artifact_out/build-info.txt"
exec > >(tee "$artifact_out/build.log") 2>&1
make "${build_args[@]}" vendor/sm8150-perf_defconfig
scripts/kconfig/merge_config.sh -m -O "$build_out" \
    "$build_out/.config" arch/arm64/configs/vendor/oplus.config
make "${build_args[@]}" olddefconfig
for gov in DESPAIR ELEMENTALX INTELLIACTIVE BLU_ACTIVE; do
    grep -qx "CONFIG_CPU_FREQ_GOV_${gov}=y" "$build_out/.config"
done
cp "$build_out/.config" "$artifact_out/kernel.config"
sha256sum "$build_out/.config" >> "$artifact_out/build-info.txt"
python3 tools/testing/selftests/cpufreq/legacy_governors_test.py
make "${build_args[@]}" -j"$build_jobs" Image.gz-dtb modules
make "${build_args[@]}" -s kernelrelease >> "$artifact_out/build-info.txt"
cp "$build_out/arch/arm64/boot/Image" "$artifact_out/"
cp "$build_out/arch/arm64/boot/Image.gz-dtb" "$artifact_out/"
cp "$build_out/System.map" "$artifact_out/"
mkdir -p "$artifact_out/dtbs" "$artifact_out/modules"
while IFS= read -r -d '' item; do
    rel=${item#"$build_out/arch/arm64/boot/dts/"}
    mkdir -p "$artifact_out/dtbs/$(dirname "$rel")"
    cp "$item" "$artifact_out/dtbs/$rel"
done < <(find "$build_out/arch/arm64/boot/dts" -name '*.dtb' -print0)
while IFS= read -r -d '' item; do
    rel=${item#"$build_out/"}
    mkdir -p "$artifact_out/modules/$(dirname "$rel")"
    cp "$item" "$artifact_out/modules/$rel"
done < <(find "$build_out" -name '*.ko' -print0)
(cd "$artifact_out" && find . -type f ! -name SHA256SUMS ! -name build.log \
    -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS)
echo 'Full kernel build completed. Device validation is pending.'
