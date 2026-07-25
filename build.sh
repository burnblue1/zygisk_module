# 打包为 Magisk 模块
echo "Packaging Magisk module..."
MODULE_DIR="game_helper_zygisk"
rm -rf "$MODULE_DIR"
mkdir -p "$MODULE_DIR/META-INF/com/google/android"
mkdir -p "$MODULE_DIR/zygisk"

# 创建 update-binary
cat > "$MODULE_DIR/META-INF/com/google/android/update-binary" << 'EOF'
#!/sbin/sh
############
# Magisk Module Installer
############

OUTFD=$2
ZIPFILE=$3

mkdir -p /dev/tmp
unzip -o "$ZIPFILE" 'module.prop' 'customize.sh' 'post-fs-data.sh' 'zygisk/*' -d /dev/tmp/magisk_install

. /data/adb/magisk/util_functions.sh

print_modname() {
  ui_print "*******************************"
  ui_print "  Game Helper Zygisk Module"
  ui_print "*******************************"
}

copy_files() {
  mkdir -p $MODPATH/zygisk
  cp -f /dev/tmp/magisk_install/module.prop $MODPATH/
  cp -f /dev/tmp/magisk_install/customize.sh $MODPATH/
  cp -f /dev/tmp/magisk_install/post-fs-data.sh $MODPATH/
  cp -f /dev/tmp/magisk_install/zygisk/*.so $MODPATH/zygisk/
}

set_permissions() {
  set_perm_recursive $MODPATH 0 0 0755 0644
  set_perm $MODPATH/zygisk/arm64-v8a.so 0 0 0755
  set_perm $MODPATH/zygisk/armeabi-v7a.so 0 0 0755
}

print_modname
copy_files
set_permissions

rm -rf /dev/tmp/magisk_install
EOF

chmod +x "$MODULE_DIR/META-INF/com/google/android/update-binary"

# 创建 updater-script
cat > "$MODULE_DIR/META-INF/com/google/android/updater-script" << 'EOF'
#MAGISK
EOF

# 复制模块文件
cp module.prop "$MODULE_DIR/"
cp customize.sh "$MODULE_DIR/"
cp post-fs-data.sh "$MODULE_DIR/"
cp "$OUTPUT_DIR/arm64-v8a/libgamehelper.so" "$MODULE_DIR/zygisk/arm64-v8a.so"
cp "$OUTPUT_DIR/armeabi-v7a/libgamehelper.so" "$MODULE_DIR/zygisk/armeabi-v7a.so"

# 打包
ZIP_NAME="game_helper_zygisk_v1.0.zip"
rm -f "$ZIP_NAME"
cd "$MODULE_DIR"
zip -r "../$ZIP_NAME" . -x "*.DS_Store"
cd ..

echo ""
echo "Build complete!"
echo "Zip contents:"
unzip -l "$ZIP_NAME"
