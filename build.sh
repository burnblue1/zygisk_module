#!/bin/bash

# ============================================
# 编译 Zygisk 模块
# 需要: Android NDK r25+
# ============================================

set -e

# NDK 路径
NDK="${ANDROID_NDK_HOME}"
if [ ! -d "$NDK" ]; then
    echo "Error: NDK not found at $NDK"
    exit 1
fi

TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
API=26

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
INCLUDE_DIR="$PWD/jni"

# ============================================
# 编译 arm64
# ============================================
echo ""
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
    -I"$INCLUDE_DIR" \
    -llog \
    -ldl \
    -static-libstdc++

echo "arm64 done: $(ls -la $OUTPUT_DIR/arm64-v8a/libgamehelper.so)"

# ============================================
# 编译 arm32
# ============================================
echo ""
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
    -I"$INCLUDE_DIR" \
    -llog \
    -ldl \
    -static-libstdc++

echo "arm32 done: $(ls -la $OUTPUT_DIR/armeabi-v7a/libgamehelper.so)"

# ============================================
# 打包为 Magisk 模块
# ============================================
echo ""
echo "Packaging Magisk module..."

MODULE_DIR="game_helper_zygisk"
rm -rf "$MODULE_DIR"
mkdir -p "$MODULE_DIR/META-INF/com/google/android"
mkdir -p "$MODULE_DIR/zygisk"

# 创建 update-binary
cat > "$MODULE_DIR/META-INF/com/google/android/update-binary" << 'ENDOFSCRIPT'
#!/sbin/sh

OUTFD=$2
ZIPFILE=$3

ui_print() {
  echo -n -e "ui_print $1\n" >> /proc/self/fd/$OUTFD
}

ui_print "*******************************"
ui_print "  Game Helper Zygisk Module"
ui_print "*******************************"

ui_print "- Extracting files..."
unzip -o "$ZIPFILE" 'module.prop' 'customize.sh' 'post-fs-data.sh' 'zygisk/*' -d "$MODPATH" > /dev/null

if [ -f "$MODPATH/module.prop" ]; then
  ui_print "- module.prop OK"
else
  ui_print "! Failed to extract module.prop"
  exit 1
fi

ui_print "- Setting permissions..."
set_perm_recursive $MODPATH 0 0 0755 0644

if [ -f "$MODPATH/zygisk/arm64-v8a.so" ]; then
  set_perm $MODPATH/zygisk/arm64-v8a.so 0 0 0755
  ui_print "- arm64-v8a.so OK"
fi

if [ -f "$MODPATH/zygisk/armeabi-v7a.so" ]; then
  set_perm $MODPATH/zygisk/armeabi-v7a.so 0 0 0755
  ui_print "- armeabi-v7a.so OK"
fi

ui_print "- Done!"
ui_print " "
ui_print "Reboot to activate!"
ENDOFSCRIPT

chmod 755 "$MODULE_DIR/META-INF/com/google/android/update-binary"

# 创建 updater-script
cat > "$MODULE_DIR/META-INF/com/google/android/updater-script" << 'EOF'
#MAGISK
EOF

chmod 644 "$MODULE_DIR/META-INF/com/google/android/updater-script"

# 复制模块文件
cp module.prop "$MODULE_DIR/"
cp customize.sh "$MODULE_DIR/"
cp post-fs-data.sh "$MODULE_DIR/"

# 检查并复制 so 文件
if [ ! -f "$OUTPUT_DIR/arm64-v8a/libgamehelper.so" ]; then
    echo "ERROR: arm64-v8a/libgamehelper.so not found!"
    exit 1
fi

if [ ! -f "$OUTPUT_DIR/armeabi-v7a/libgamehelper.so" ]; then
    echo "ERROR: armeabi-v7a/libgamehelper.so not found!"
    exit 1
fi

cp "$OUTPUT_DIR/arm64-v8a/libgamehelper.so" "$MODULE_DIR/zygisk/arm64-v8a.so"
cp "$OUTPUT_DIR/armeabi-v7a/libgamehelper.so" "$MODULE_DIR/zygisk/armeabi-v7a.so"

echo ""
echo "Module structure:"
find "$MODULE_DIR" -type f | sort

# ============================================
# 打包 zip
# ============================================
ZIP_NAME="game_helper_zygisk_v1.0.zip"
rm -f "$ZIP_NAME"

cd "$MODULE_DIR"
zip -r "../$ZIP_NAME" . -x "*.DS_Store" > /dev/null
cd ..

if [ ! -f "$ZIP_NAME" ]; then
    echo "ERROR: Failed to create $ZIP_NAME"
    exit 1
fi

echo ""
echo "========================================"
echo " Build complete!"
echo "========================================"
echo ""
echo "Zip contents:"
unzip -l "$ZIP_NAME"
echo ""
echo "File size: $(ls -lh $ZIP_NAME | awk '{print $5}')"
echo ""
echo "Install:"
echo "  1. adb push $ZIP_NAME /sdcard/"
echo "  2. Magisk -> Modules -> Install from storage"
echo "  3. Reboot"
echo "  4. Browser: http://127.0.0.1:27042"
