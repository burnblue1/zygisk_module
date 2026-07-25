LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := libgamehelper
LOCAL_SRC_FILES := main.cpp inject.cpp bridge.cpp lua_hook.cpp anti_detect.cpp
LOCAL_CPPFLAGS := -std=c++17 -fPIC -fvisibility=hidden -O2
LOCAL_LDLIBS := -llog -ldl
LOCAL_CFLAGS := -DZYGISK_MODULE
include $(BUILD_SHARED_LIBRARY)