#include <jni.h>
#include <dlfcn.h>
#include <android/log.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#include <string>
#include <fstream>
#include <mutex>
#include <thread>
#include <chrono>
#include "bridge.h"
#define TAG "GameHelper"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// 前向声明
extern void install_anti_detect();
extern void* wait_for_library(const char* name, int timeout_ms);
extern void hook_lua_functions(void* tolua_handle);
extern void start_bridge_server(int port);
extern void register_all_commands();

static bool g_injected = false;
static std::mutex g_inject_mutex;

// ============================================
// 获取当前进程名
// ============================================
static std::string get_process_name() {
    std::ifstream cmdline("/proc/self/cmdline");
    if (!cmdline.good()) return "";
    std::string name;
    std::getline(cmdline, name, '\0');
    return name;
}

// ============================================
// 主注入线程
// ============================================
static void* injection_thread(void*) {
    LOGI("Injection thread started");
    auto& bridge = BridgeServer::getInstance();
    
    // 1. 反检测 - 尽早执行
    install_anti_detect();
    LOGI("[OK] Anti-detect installed");
    
    // 2. 启动 Bridge Server
    start_bridge_server(27042);
    register_all_commands();
    bridge.sendLog("INFO", "Bridge Server started on 127.0.0.1:27042");
    
    // 3. 等待 libtolua.so 加载
    void* tolua = wait_for_library("libtolua.so", 120000);
    if (tolua) {
        LOGI("[OK] libtolua.so found at %p", tolua);
        hook_lua_functions(tolua);
        bridge.sendLog("INFO", "Lua hook installed");
    } else {
        LOGE("libtolua.so not found after timeout");
        bridge.sendLog("WARN", "libtolua.so not found, Lua injection skipped");
    }
    
    // 4. 等待目标库加载
    const char* game_libs[] = {
        "libil2cpp.so",
        "libunity.so",
        "libcocos2dlua.so",
        "libgame.so",
        nullptr
    };
    
    for (int i = 0; game_libs[i]; i++) {
        void* lib = wait_for_library(game_libs[i], 30000);
        if (lib) {
            LOGI("[OK] %s loaded at %p", game_libs[i], lib);
            bridge.sendLog("INFO", std::string("Game lib found: ") + game_libs[i]);
        }
    }
    
    bridge.sendLog("INFO", "Injection complete. Waiting for commands...");
    LOGI("Injection thread done");
    return nullptr;
}

// ============================================
// Zygisk 入口点
// ============================================
extern "C" [[gnu::visibility("default")]] void zygisk_module_entry(void* companion_ptr) {
    // 只注入目标游戏
    std::string process = get_process_name();
    
    // 提取包名（去掉 :xxx 后缀）
    size_t pos = process.find(':');
    std::string pkg = (pos != std::string::npos) ? process.substr(0, pos) : process;
    
    if (pkg != "com.gof.china") {
        // 不输出日志，避免干扰
        return;
    }
    
    LOGI("Target game detected: %s", process.c_str());
    
    std::lock_guard<std::mutex> lock(g_inject_mutex);
    if (g_injected) return;
    g_injected = true;
    
    // 稍微延迟，确保 ART 初始化
    usleep(300000);
    
    pthread_t tid;
    pthread_create(&tid, nullptr, injection_thread, nullptr);
    pthread_detach(tid);
    
    LOGI("Zygisk entry done, injection thread spawned");
}

// ============================================
// Zygisk 卸载
// ============================================
extern "C" [[gnu::visibility("default")]] void zygisk_module_unload() {
    LOGI("Module unloaded");
    BridgeServer::getInstance().stop();
}
// 在文件末尾加上
__attribute__((constructor))
static void init_module() {
    // ZygiskNext 加载 so 时自动调用
    // 不能做重操作，只启动线程
    pthread_t tid;
    pthread_create(&tid, nullptr, injection_thread, nullptr);
    pthread_detach(tid);
}
