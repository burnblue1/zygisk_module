#include <jni.h>
#include <dlfcn.h>
#include <android/log.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#include <string>
#include <mutex>
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include "bridge.h"


#define TAG "GameHelper"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

using android_dlopen_ext_t = void* (*)(const char*, int, const void*, const char*);
static android_dlopen_ext_t orig_android_dlopen_ext = nullptr;

extern void install_anti_detect();
extern void hook_lua_functions(void* tolua_handle);
extern void start_bridge_server(int port);
extern void register_all_commands();
extern void* wait_for_library(const char* name, int timeout_ms);

static bool g_injected = false;
static std::mutex g_inject_mutex;

static void write_file_log(const char* msg) {
    FILE* fp = fopen("/data/local/tmp/game_helper.log", "a");
    if (!fp) return;
    time_t now = time(nullptr);
    struct tm* tm_info = localtime(&now);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y‑%m‑%d %H:%M:%S", tm_info);
    fprintf(fp, "[%s] %s\n", buf, msg);
    fclose(fp);
}

static std::string get_process_name() {
    FILE* fp = fopen("/proc/self/cmdline", "r");
    if (!fp) return "";
    char buf[256] = {0};
    size_t n = fread(buf, 1, sizeof(buf)-1, fp);
    fclose(fp);
    if (n == 0) return "";
    return std::string(buf);
}

// ======================
// 复刻 Frida android_dlopen_ext hook
// ======================
static void* hooked_android_dlopen_ext(const char* pathname, int flags, const void* extinfo, const char* caller)
{
    bool block_so = false;
    bool trigger_lua_hook = false;
    const char* real_path = pathname;

    if (pathname != nullptr)
    {
        std::string path(pathname);
        if (path.find("libxgVipSecurity.so") != std::string::npos ||
            path.find("libmsaoaidsec.so") != std::string::npos)
        {
            write_file_log("BLOCK security so: " + path);
            block_so = true;
        }
        if (path.find("libtolua.so") != std::string::npos)
        {
            trigger_lua_hook = true;
        }
    }

    if (block_so)
    {
        real_path = ""; // 拦截，传空字符串，复刻JS args[0] = allocUtf8String("")
    }

    void* handle = orig_android_dlopen_ext(real_path, flags, extinfo, caller);

    // onLeave：libtolua.so加载完成，立刻hook lua
    if (trigger_lua_hook && handle != nullptr)
    {
        write_file_log("libtolua.so loaded, run hook_lua_functions");
        hook_lua_functions(handle);
    }
    return handle;
}

static void* injection_thread(void*)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::string proc_name = get_process_name();
    if (proc_name.find("com.gof.china") == std::string::npos) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(g_inject_mutex);
    if (g_injected) return nullptr;
    g_injected = true;

    write_file_log("INJECT: thread start");

    // 1.执行基础反检测（不含dlopen hook）
    install_anti_detect();
    write_file_log("INJECT: install_anti_detect done");

    // 2. Hook android_dlopen_ext 复刻你的frida JS逻辑
    void* libc_handle = dlopen("libc.so", RTLD_LAZY);
    if (libc_handle)
    {
        void* sym = dlsym(libc_handle, "android_dlopen_ext");
        if (sym)
        {
            int ret = inline_hook(sym, (void*)hooked_android_dlopen_ext, (void**)&orig_android_dlopen_ext);
            if (ret == 0)
            {
                write_file_log("INJECT: hook android_dlopen_ext success");
            }
            else
            {
                write_file_log("INJECT ERROR: hook android_dlopen_ext failed");
            }
        }
        dlclose(libc_handle);
    }

    // 3.Bridge延后初始化
    auto& bridge = BridgeServer::getInstance();
    start_bridge_server(27042);
    register_all_commands();
    write_file_log("INJECT: bridge server start 27042");

    // 4.继续等待其它游戏so（libil2cpp等）
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

    write_file_log("INJECT: injection complete");
    bridge.sendLog("INFO", "Injection complete. Waiting for commands...");
    return nullptr;
}

// pthread_atfork child回调，fork完成后app进程创建线程
static void atfork_child() {
    pthread_t tid;
    int ret = pthread_create(&tid, nullptr, injection_thread, nullptr);
    if (ret != 0) {
        LOGE("pthread_create ret=%d", ret);
        write_file_log("INJECT ERROR: pthread_create failed");
        return;
    }
    pthread_detach(tid);
}

__attribute__((constructor))
static void init_module() {
    int ret = pthread_atfork(nullptr, nullptr, atfork_child);
    if(ret != 0) {
        LOGE("pthread_atfork register fail %d", ret);
    }
    LOGI("constructor: pthread_atfork registered");
}
