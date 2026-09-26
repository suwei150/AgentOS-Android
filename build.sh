#!/bin/bash
# AgentCore v0.3 编译脚本
# 
# 用法:
#   ./build.sh              # 编译 debug 版
#   ./build.sh release      # 编译 release 版 (优化+strip)
# 
# 需求:
#   - Android NDK (r27c+) 或
#   - aarch64-linux-musl-gcc (静态 musl)
# 
# 输出:
#   ../bin/agentcore (静态 ELF, 可直接在 Android 运行)

set -e

BUILD_TYPE="${1:-debug}"
SRC_DIR="$(cd "$(dirname "$0")" && pwd)/src"
BIN_DIR="$(cd "$(dirname "$0")" && pwd)/bin"
mkdir -p "$BIN_DIR"

# ---- 编译方式选择 ----
# 方式 1: Android NDK (推荐, 使用 bionic libc)
# 方式 2: musl 静态编译 (零依赖, 稍大)

find_ndk() {
    if [ -n "$ANDROID_NDK_HOME" ] && [ -d "$ANDROID_NDK_HOME" ]; then
        echo "$ANDROID_NDK_HOME"
        return
    fi
    if [ -d "/opt/android-ndk" ]; then
        echo "/opt/android-ndk"
        return
    fi
    if [ -d "/usr/local/android-ndk" ]; then
        echo "/usr/local/android-ndk"
        return
    fi
    if [ -d "$HOME/Android/Sdk/ndk" ]; then
        ls -1 "$HOME/Android/Sdk/ndk" | sort -V | tail -1 | xargs -I{} echo "$HOME/Android/Sdk/ndk/{}"
        return
    fi
    if [ -d "/data/data/com.termux/files/home/android-ndk" ]; then
        echo "/data/data/com.termux/files/home/android-ndk"
        return
    fi
    return 1
}

if command -v aarch64-linux-musl-gcc >/dev/null 2>&1; then
    CC="aarch64-linux-musl-gcc"
    COMPILER="musl"
elif NDK=$(find_ndk); then
    # NDK 里找 toolchain
    for tcc in "$NDK"/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android*; do
        if [ -x "$tcc" ]; then
            CC="$tcc"
            COMPILER="ndk"
            break
        fi
    done
    if [ -z "$CC" ]; then
        echo "[ERROR] NDK found but no compiler in toolchains"
        ls "$NDK"/toolchains/llvm/prebuilt/ 2>&1
        exit 1
    fi
else
    echo "[ERROR] Neither NDK nor aarch64-linux-musl-gcc found"
    echo "Install Android NDK or musl toolchain"
    exit 1
fi

echo "[INFO] Compiler: $CC ($COMPILER)"

# ---- 编译参数 ----
CFLAGS="-O2 -Wall -Wextra -D_GNU_SOURCE"
LDFLAGS=""

if [ "$COMPILER" = "ndk" ]; then
    # NDK 需要指定 API level (Android 4.4 = 19, Android 12 = 31)
    CFLAGS="$CFLAGS -D__ANDROID_API__=29"
    LDFLAGS="$LDFLAGS -pie"  # 位置无关可执行文件 (Android 要求)
elif [ "$COMPILER" = "musl" ]; then
    LDFLAGS="$LDFLAGS -static"
fi

if [ "$BUILD_TYPE" = "release" ]; then
    CFLAGS="$CFLAGS -Os -DNDEBUG"
    STRIP="strip"
else
    CFLAGS="$CFLAGS -g"
    STRIP=""
fi

# ---- 编译 ----
echo "[INFO] Building AgentCore v0.3 ($BUILD_TYPE)..."
$CC $CFLAGS -o "$BIN_DIR/agentcore" "$SRC_DIR/agentcore.c" $LDFLAGS

# ---- 可选 strip ----
if [ "$BUILD_TYPE" = "release" ]; then
    # 用对应架构的 strip
    case "$COMPILER" in
        ndk)
            # NDK toolchain 里找 strip
            NDK_STRIP=$(dirname "$CC")/llvm-strip
            if [ -x "$NDK_STRIP" ]; then
                $NDK_STRIP "$BIN_DIR/agentcore"
            fi
            ;;
        musl)
            strip "$BIN_DIR/agentcore"
            ;;
    esac
fi

# ---- 输出信息 ----
echo "[INFO] Build complete:"
ls -lh "$BIN_DIR/agentcore"
file "$BIN_DIR/agentcore" 2>/dev/null || true

# ---- 部署到手机 (需要 adb) ----
if command -v adb >/dev/null 2>&1 && adb get-state 2>/dev/null | grep -q device; then
    echo "[INFO] Deploying to device..."
    adb push "$BIN_DIR/agentcore" /data/local/tmp/agentcore
    adb shell chmod +x /data/local/tmp/agentcore
    adb shell ls -lh /data/local/tmp/agentcore
    echo "[INFO] Deploy complete. Start with: adb shell /data/local/tmp/agentcore"
else
    echo "[INFO] ADB not available. Manually push: adb push $BIN_DIR/agentcore /data/local/tmp/"
fi