#!/system/bin/sh
# Zygisk 模块安装脚本
MODDIR=${0%/*}

# 设置权限
chmod 755 "$MODDIR/zygisk/libgamehelper.so" 2>/dev/null
chcon u:object_r:system_file:s0 "$MODDIR/zygisk/libgamehelper.so" 2>/dev/null