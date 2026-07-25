#!/bin/bash
set -e

NDK="${ANDROID_NDK_HOME}"
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
API=26
export PATH="$TOOLCHAIN/bin:$PATH"

# 编译
OUTPUT="output"
rm -rf "$OUTPUT"
mkdir -p "$OUTPUT/arm64-v8a" "$OUTPUT/armeabi-v7a"

SRCS="jni/main.cpp jni/inject.cpp jni/bridge.cpp jni/lua_hook.cpp jni/anti_detect.cpp"

echo "Building arm64..."
aarch64-linux-android${API}-clang++ -std=c++17 -fPIC -shared -O2 -fvisibility=hidden \
  -Wl,--strip-all -o "$OUTPUT/arm64-v8a/libgamehelper.so" $SRCS -I jni -llog -ldl -static-libstdc++

echo "Building arm32..."
armv7a-linux-androideabi${API}-clang++ -std=c++17 -fPIC -shared -O2 -fvisibility=hidden \
  -Wl,--strip-all -o "$OUTPUT/armeabi-v7a/libgamehelper.so" $SRCS -I jni -llog -ldl -static-libstdc++

# 打包
MODULE="game_helper_zygisk"
rm -rf "$MODULE"

mkdir -p "$MODULE/META-INF/com/google/android"
mkdir -p "$MODULE/lib/arm64-v8a"
mkdir -p "$MODULE/lib64/arm64-v8a"
mkdir -p "$MODULE/lib/armeabi-v7a"

# update-binary
cat > "$MODULE/META-INF/com/google/android/update-binary" << 'EOF'
#!/sbin/sh
umask 022
OUTFD=$2
ZIPFILE=$3
ui_print() { echo "$1"; }
mount /data 2>/dev/null
if [ -f /data/adb/magisk/util_functions.sh ]; then
  . /data/adb/magisk/util_functions.sh && install_module && exit 0
fi
if [ -f /data/adb/ksu/util_functions.sh ]; then
  . /data/adb/ksu/util_functions.sh && install_module && exit 0
fi
MODPATH=/data/adb/modules/game_helper
mkdir -p "$MODPATH/lib/arm64-v8a" "$MODPATH/lib64/arm64-v8a" "$MODPATH/lib/armeabi-v7a"
unzip -o "$ZIPFILE" 'module.prop' 'lib/*' 'lib64/*' -d "$MODPATH" > /dev/null
chmod -R 755 "$MODPATH/lib" "$MODPATH/lib64"
ui_print "- Installed to $MODPATH"
ui_print "- Done! Reboot."
EOF

echo "#MAGISK" > "$MODULE/META-INF/com/google/android/updater-script"

# 复制文件
cp module.prop "$MODULE/"
cp "$OUTPUT/arm64-v8a/libgamehelper.so" "$MODULE/lib/arm64-v8a/libgamehelper.so"
cp "$OUTPUT/arm64-v8a/libgamehelper.so" "$MODULE/lib64/arm64-v8a/libgamehelper.so"
cp "$OUTPUT/armeabi-v7a/libgamehelper.so" "$MODULE/lib/armeabi-v7a/libgamehelper.so"

# 设置权限
chmod 755 "$MODULE/lib/arm64-v8a/libgamehelper.so"
chmod 755 "$MODULE/lib64/arm64-v8a/libgamehelper.so"
chmod 755 "$MODULE/lib/armeabi-v7a/libgamehelper.so"

# 打包
ZIP="game_helper_zygisk_v1.0.zip"
rm -f "$ZIP"
cd "$MODULE" && zip -r "../$ZIP" . -x "*.DS_Store" > /dev/null && cd ..

echo ""
echo "========================================"
echo " Build complete!"
echo "========================================"
echo "Zip: $ZIP ($(ls -lh $ZIP | awk '{print $5}'))"
echo ""
unzip -l "$ZIP"
