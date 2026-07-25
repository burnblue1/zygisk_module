cat > post-fs-data.sh << 'EOF'
#!/system/bin/sh
MODDIR=${0%/*}
export ZYGISK_MODULE_LIBRARY="$MODDIR/zygisk/arm64-v8a.so"
EOF
