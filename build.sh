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
mkdir -p "$MODULE/META-INF/com/google/android" "$MODULE/zygisk"

# 官方标准 update-binary
cat > "$MODULE/META-INF/com/google/android/update-binary" << 'EOF'
#!/sbin/sh
umask 022
OUTFD=$2
ZIPFILE=$3
ui_print() { echo "$1"; }
mount /data 2>/dev/null
[ -f /data/adb/magisk/util_functions.sh ] && . /data/adb/magisk/util_functions.sh && install_module && exit 0
[ -f /data/adb/ksu/util_functions.sh ] && . /data/adb/ksu/util_functions.sh && install_module && exit 0
ui_print "- Installing to /data/adb/modules/game_helper"
mkdir -p /data/adb/modules/game_helper/zygisk
unzip -o "$ZIPFILE" 'module.prop' 'zygisk/*' -d /data/adb/modules/game_helper > /dev/null
chmod -R 755 /data/adb/modules/game_helper/zygisk
ui_print "- Done"
EOF

echo "#MAGISK" > "$MODULE/META-INF/com/google/android/updater-script"

# 复制模块文件
cp module.prop "$MODULE/"
cp post-fs-data.sh "$MODULE/"
cp "$OUTPUT/arm64-v8a/libgamehelper.so" "$MODULE/zygisk/arm64-v8a.so"
cp "$OUTPUT/armeabi-v7a/libgamehelper.so" "$MODULE/zygisk/armeabi-v7a.so"

# KernelSU 需要根目录也有
cp "$OUTPUT/arm64-v8a/libgamehelper.so" "$MODULE/arm64-v8a.so"
cp "$OUTPUT/armeabi-v7a/libgamehelper.so" "$MODULE/armeabi-v7a.so"

ZIP="game_helper_zygisk_v1.0.zip"
rm -f "$ZIP"
cd "$MODULE" && zip -r "../$ZIP" . -x "*.DS_Store" > /dev/null && cd ..

echo "Done: $ZIP ($(ls -lh $ZIP | awk '{print $5}'))"
unzip -l "$ZIP"
