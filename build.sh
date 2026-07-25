#!/bin/bash

# ============================================
# 编译 Zygisk 模块
# 需要: Android NDK r25+
# ============================================

set -e

# NDK 路径
NDK="${ANDROID_NDK_HOME:-$HOME/Android/Sdk/ndk/25.2.9519653}"
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64"

if [ ! -d "$TOOLCHAIN" ]; then
    echo "Error: NDK not found at $NDK"
    echo "Set ANDROID_NDK_HOME or edit this script"
    exit 1
fi

# API level
API=26

# 输出目录
OUTPUT_DIR="output"
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR/arm64-v8a"
mkdir -p "$OUTPUT_DIR/armeabi-v7a"

# 源文件
SRCS="jni/main.cpp jni/inject.cpp jni/bridge.cpp jni/lua_hook.cpp jni/anti_detect.cpp"

# 编译 arm64
echo "Building arm64..."
aarch64-linux-android${API}-clang++ \
    -std=c++17 \
    -fPIC \
    -shared \
    -fvisibility=hidden \
    -fvisibility-inlines-hidden \
    -O2 \
    -flto \
    -Wl,--strip-all \
    -Wl,--exclude-libs,ALL \
    -Wl,-soname,libgamehelper.so \
    -o "$OUTPUT_DIR/arm64-v8a/libgamehelper.so" \
    $SRCS \
    -I jni \
    -llog \
    -ldl \
    -static-libstdc++

# 编译 arm32
echo "Building arm32..."
armv7a-linux-androideabi${API}-clang++ \
    -std=c++17 \
    -fPIC \
    -shared \
    -fvisibility=hidden \
    -fvisibility-inlines-hidden \
    -O2 \
    -flto \
    -Wl,--strip-all \
    -Wl,--exclude-libs,ALL \
    -Wl,-soname,libgamehelper.so \
    -o "$OUTPUT_DIR/armeabi-v7a/libgamehelper.so" \
    $SRCS \
    -I jni \
    -llog \
    -ldl \
    -static-libstdc++

# 打包为 Magisk 模块
echo "Packaging Magisk module..."
MODULE_DIR="game_helper_zygisk"
rm -rf "$MODULE_DIR"
mkdir -p "$MODULE_DIR/zygisk"

cp module.prop "$MODULE_DIR/"
cp customize.sh "$MODULE_DIR/"
cp post-fs-data.sh "$MODULE_DIR/"
cp "$OUTPUT_DIR/arm64-v8a/libgamehelper.so" "$MODULE_DIR/zygisk/arm64-v8a.so"
cp "$OUTPUT_DIR/armeabi-v7a/libgamehelper.so" "$MODULE_DIR/zygisk/armeabi-v7a.so"

# 创建 SHA1 校验
cd "$MODULE_DIR"
sha1sum zygisk/arm64-v8a.so zygisk/armeabi-v7a.so > zygisk/sha1sum.txt 2>/dev/null || true
cd ..

# 打包
ZIP_NAME="game_helper_zygisk_v1.0.zip"
rm -f "$ZIP_NAME"
cd "$MODULE_DIR"
zip -r "../$ZIP_NAME" . -x "*.DS_Store"
cd ..

echo ""
echo "========================================"
echo " Build complete!"
echo " Output: $ZIP_NAME"
echo "========================================"
echo ""
echo "Install:"
echo "  1. Push to phone: adb push $ZIP_NAME /sdcard/"
echo "  2. Install in Magisk: Modules -> Install from storage"
echo "  3. Reboot: adb reboot"
echo "  4. Open browser: http://127.0.0.1:27042"