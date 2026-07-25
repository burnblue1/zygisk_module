#!/bin/bash
set -e

# NDK 路径 - GitHub Actions
NDK="${ANDROID_NDK_HOME}"
if [ ! -d "$NDK" ]; then
    echo "Error: NDK not found at $NDK"
    exit 1
fi

TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
API=26

# 设置 PATH
export PATH="$TOOLCHAIN/bin:$PATH"

echo "NDK: $NDK"
echo "Toolchain: $TOOLCHAIN"
echo "Compiler: $(which aarch64-linux-android${API}-clang++)"

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
    -O2 \
    -Wl,--strip-all \
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
    -O2 \
    -Wl,--strip-all \
    -Wl,-soname,libgamehelper.so \
    -o "$OUTPUT_DIR/armeabi-v7a/libgamehelper.so" \
    $SRCS \
    -I jni \
    -llog \
    -ldl \
    -static-libstdc++

# 打包
echo "Packaging Magisk module..."
MODULE_DIR="game_helper_zygisk"
rm -rf "$MODULE_DIR"
mkdir -p "$MODULE_DIR/zygisk"

cp module.prop "$MODULE_DIR/"
cp customize.sh "$MODULE_DIR/"
cp post-fs-data.sh "$MODULE_DIR/"
cp "$OUTPUT_DIR/arm64-v8a/libgamehelper.so" "$MODULE_DIR/zygisk/arm64-v8a.so"
cp "$OUTPUT_DIR/armeabi-v7a/libgamehelper.so" "$MODULE_DIR/zygisk/armeabi-v7a.so"

ZIP_NAME="game_helper_zygisk_v1.0.zip"
rm -f "$ZIP_NAME"
cd "$MODULE_DIR"
zip -r "../$ZIP_NAME" . -x "*.DS_Store"
cd ..

echo ""
echo "Build complete! Output: $ZIP_NAME"
