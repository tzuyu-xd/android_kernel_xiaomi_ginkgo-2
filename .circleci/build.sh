#!/usr/bin/env bash

#
#
# Copyright (c) 2021 tzuyu 
# 
#

# Specify compiler.
# 'evagcc' or 'gcc'
if [[ "$1" = "--aosp" ]]; then
COMPILER=aosp
elif [[ "$1" = "--azure" ]]; then
COMPILER=azure
elif [[ "$1" = "--proton" ]]; then
COMPILER=proton
elif [[ "$1" = "--sdc" ]]; then
COMPILER=azure
elif [[ "$1" = "--sdc" ]]; then
COMPILER=gcc
fi
echo "Cloning dependencies"
if [[ $COMPILER = "aosp" ]]
then
  echo "|| Cloning AOSP-13 ||"
  git clone https://gitlab.com/crdroidandroid/android_prebuilts_clang_host_linux-x86_clang-r433403 -b 11.0 --depth=1 clang
  git clone https://github.com/sohamxda7/llvm-stable -b gcc64 --depth=1 gcc
  git clone https://github.com/sohamxda7/llvm-stable -b gcc32 --depth=1 gcc32
  PATH="${KERNEL_DIR}/clang/bin:${KERNEL_DIR}/gcc/bin:${KERNEL_DIR}/gcc32/bin:${PATH}"
  export KBUILD_COMPILER_STRING="$(${KERNEL_DIR}/clang/bin/clang --version | head -n 1 | perl -pe 's/\(http.*?\)//gs' | sed -e 's/  */ /g')"
elif [[ $COMPILER = "azure" ]]
then
  echo  "|| Cloning AZURE-14 ||"
  git clone --depth=1 https://github.com/kdrag0n/proton-clang.git clang
  PATH="${KERNEL_DIR}/clang/bin:$PATH"
  export KBUILD_COMPILER_STRING="$(${KERNEL_DIR}/clang/bin/clang --version | head -n 1 | perl -pe 's/\(http.*?\)//gs' | sed -e 's/  */ /g')"
elif [[ $COMPILER = "proton" ]]
then
  echo  "|| Cloning Proton-13 ||"
  git clone --depth=1 https://github.com/kdrag0n/proton-clang.git clang
  PATH="${KERNEL_DIR}/clang/bin:$PATH"
  export KBUILD_COMPILER_STRING="$(${KERNEL_DIR}/clang/bin/clang --version | head -n 1 | perl -pe 's/\((?:http|git).*?\)//gs' | sed -e 's/  */ /g' -e 's/[[:space:]]*$//' -e 's/^.*clang/clang/')"
elif [[ $COMPILER = "proton" ]]
then
  echo  "|| Cloning Snapdragon-14 ||"
  git clone --depth=1 --depth=1 https://github.com/ThankYouMario/proprietary_vendor_qcom_sdclang -b 14 clang
  PATH="${KERNEL_DIR}/clang/bin:$PATH"
  export KBUILD_COMPILER_STRING="$(${KERNEL_DIR}/clang/bin/clang --version | head -n 1 | perl -pe 's/\(http.*?\)//gs' | sed -e 's/  */ /g')"
elif [[ $COMPILER = "gcc" ]]
then
  echo  "|| Cloning EVA-GCC ||"
  git clone --depth=1 https://github.com/mvaisakh/gcc-arm64.git gcc64
  PATH=$KERNEL_DIR/gcc64/bin/:/usr/bin:$PATH
  export KBUILD_COMPILER_STRING="$(${KERNEL_DIR}/gcc64/bin/aarch64-elf-gcc --version | head -n 1)"
fi
echo "|| Cloning Anykernel ||"
git clone --depth=1 https://github.com/tzuyu-xd/AnyKernel3 AnyKernel
echo "Done"
KERNEL_DIR=$(pwd)
IMAGE=$(pwd)/out/arch/arm64/boot/Image.gz-dtb
SHA=$(date +"%F-%S")
START1=$(date +"%s")
export token="5330590089:AAE3gFWPQBVJuQfFln8sQkDXJrW_fHBvxc0"
export chat_id="-1001559491005"
export ARCH=arm64
export KBUILD_BUILD_USER=tzuyu-xd
export KBUILD_BUILD_HOST=circleci
# Send info plox
function sendinfo() {
        curl -s -X POST "https://api.telegram.org/bot$token/sendMessage" \
                        -d chat_id="$chat_id" \
                        -d "disable_web_page_preview=true" \
                        -d "parse_mode=html" \
                        -d text="<b>• Ginkgo Kernel •</b> ${TYPE} build started%0AStarted on <code>Circle CI</code>%0AFor device Redmi Note 8/8T%0AOn branch <code>${CIRCLE_BRANCH}</code>%0AUnder commit <code>$(git log --pretty=format:'"%h : %s"' -1)</code>%0AStarted on <code>$(TZ=Asia/Kolkata date)</code>%0A<b>CI Workflow information:</b> <a href='https://circleci.com/workflow-run/${CIRCLE_WORKFLOW_ID}'>here</a>"
}
# Push kernel to channel
function push() {
    cd AnyKernel
    curl -F document=@"$(echo *.zip)" "https://api.telegram.org/bot$token/sendDocument" \
        -F chat_id="$chat_id" \
        -F "disable_web_page_preview=true" \
        -F "parse_mode=html" \
        -F caption="Build took $(($DIFF1 / 60)) minute(s) and $(($DIFF1 % 60)) second(s). | <b>$KBUILD_COMPILER_STRING</b>"
}
# Fin Error
function finerr() {
    curl -s -X POST "https://api.telegram.org/bot$token/sendMessage" \
        -d chat_id="$chat_id" \
        -d "disable_web_page_preview=true" \
        -d "parse_mode=markdown" \
        -d text="Build throw an error(s)"
    exit 1
}
# Compile plox
function compile() {
    make -s -j$(nproc --all) O=out ARCH=arm64 vendor/ginkgo-perf_defconfig
    	if [[ $COMPILER = "azure" ]]
    	then
        make -j$(nproc --all) O=out \
    				ARCH=arm64 \
    				CC=clang \
    				AR=llvm-ar \
    				NM=llvm-nm \
    				LD=ld.lld \
    				OBJCOPY=llvm-objcopy \
    				OBJDUMP=llvm-objdump \
    				STRIP=llvm-strip \
    				CLANG_TRIPLE=aarch64-linux-gnu- \
    				CROSS_COMPILE_ARM32=arm-linux-gnueabi- \
    				CROSS_COMPILE=aarch64-linux-gnu-
        elif [[ $COMPILER = "proton" ]]
        then
        make -j$(nproc --all) O=out \
    				ARCH=arm64 \
    				CC=clang \
    				AR=llvm-ar \
    				NM=llvm-nm \
    				OBJCOPY=llvm-objcopy \
    				OBJDUMP=llvm-objdump \
    				STRIP=llvm-strip \
    				CROSS_COMPILE_ARM32=arm-linux-gnueabi- \
    				CROSS_COMPILE=aarch64-linux-gnu-
    	elif [[ $COMPILER = "aosp" ]]
    	then
        make -j$(nproc --all) O=out \
    				ARCH=arm64 \
    				CLANG_TRIPLE=aarch64-linux-gnu- \
    				CROSS_COMPILE=aarch64-linux-android- \
    				CROSS_COMPILE_ARM32=arm-linux-androideabi- \
    				CC=clang \
    				AR=llvm-ar \
    				NM=llvm-nm \
    				OBJCOPY=llvm-objcopy \
    				OBJDUMP=llvm-objdump \
    				READELF=llvm-readelf \
    				OBJSIZE=llvm-size \
    				STRIP=llvm-strip \
    				HOSTCC=clang \
    				HOSTCXX=clang++
    	elif [[ $COMPILER = "gcc" ]]
    	then
        make -j$(nproc --all) O=out \
    				ARCH=arm64 \
    				CROSS_COMPILE=aarch64-elf- \
    				AR=aarch64-elf-ar \
    				OBJDUMP=aarch64-elf-objdump \
    				STRIP=aarch64-elf-strip \
    				CROSS_COMPILE_ARM32=arm-eabi-
    	fi
        if ! [ -a "$IMAGE" ]; then
            finerr
            exit 1
        fi
    cp ${IMAGE} AnyKernel/
}
# Zipping
function zipping() {
    cd AnyKernel || exit 1
    zip -r9 Ginkgo-R-${TYPE}-${SHA}.zip *
    cd ..
}
sendinfo
compile
zipping
END1=$(date +"%s")
DIFF1=$(($END1 - $START1))
push
