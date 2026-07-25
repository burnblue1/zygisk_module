#include "anti_detect.h"
#include <dlfcn.h>
#include <string>
#include <vector>
#include <android/log.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <signal.h>

#define TAG "AntiDetect"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

// 要拦截的安全库
static const std::vector<std::string> BLOCK_LIBS = {
    "libxgVipSecurity.so",
    "libmsaoaidsec.so",
    "libmsaoaid.so",
    "libbugly.so",
    "libtprt.so",
    "libtersafe.so",
    "libshell.so",
    "libSecShell.so",
    "libDexHelper.so",
    "libcms.so",
};

// ============================================
// 原始函数指针
// ============================================
static void* (*original_dlopen)(const char*, int) = nullptr;
static int (*original_open)(const char*, int, ...) = nullptr;

// ============================================
// Hook dlopen
// ============================================
static void* hooked_dlopen(const char* path, int mode) {
    if (path) {
        for (const auto& lib : BLOCK_LIBS) {
            if (strstr(path, lib.c_str())) {
                LOGI("Blocked: %s", path);
                return nullptr; // 返回失败
            }
        }
    }
    return original_dlopen(path, mode);
}

// ============================================
// 阻止进程退出
// ============================================
static void block_exit() {
    // 阻止 SIGABRT
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = [](int sig) {
        LOGI("Caught signal %d, ignoring...", sig);
        // 不退出
    };
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGQUIT, &sa, nullptr);
    sigaction(SIGPIPE, &sa, nullptr);
    
    // SIGKILL (9) 无法拦截
}

// ============================================
// 隐藏 /proc/self/maps 中的特征
// ============================================
static void hide_maps() {
    // 通过 hook open/read 隐藏特定行
    // 需要 inline hook，这里仅示意
}

// ============================================
// 安装反检测
// ============================================
void install_anti_detect() {
    LOGI("Installing anti-detection...");
    
    // 1. 阻止退出
    block_exit();
    
    // 2. 保存原始 dlopen
    original_dlopen = (void*(*)(const char*, int))dlopen;
    
    // 注意：完整的 Hook 需要 Dobby/substrate 等框架
    // 这里仅阻止信号，PLT Hook 需要额外库支持
    
    LOGI("Anti-detection installed (basic)");
}