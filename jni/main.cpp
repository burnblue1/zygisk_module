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

#define TAG "GameHelper"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

extern int inline_hook(void* target, void* replace, void** original);
using android_dlopen_ext_t = void* (*)(const char*, int, const void*, const char*);
static android_dlopen_ext_t orig_android_dlopen_ext = nullptr;

extern void install_anti_detect();
extern void hook_lua_functions(void* tolua_handle);

static bool g_injected = false;
static std::mutex g_inject_mutex;

// 仅在app业务线程使用，zygote阶段禁止调用
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
            block_so = true;
        }
        if (path.find("libtolua.so") != std::string::npos)
        {
            trigger_lua_hook = true;
        }
    }

    if (block_so)
    {
        real_path = "";
    }

    void* handle = orig_android_dlopen_ext(real_path, flags, extinfo, caller);

    if (trigger_lua_hook && handle != nullptr)
    {
        LOGI("libtolua.so loaded, trigger hook_lua_functions");
        hook_lua_functions(handle);
    }
    return handle;
}

static void* injection_thread(void*)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    LOGI("injection_thread running");

    std::string proc_name = get_process_name();
    LOGI("proc name: %s", proc_name.c_str());
    if (proc_name.find("com.gof.china") == std::string::npos) {
        LOGI("skip, not target app");
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(g_inject_mutex);
    if (g_injected) return nullptr;
    g_injected = true;

    write_file_log("INJECT: thread start");
    LOGI("target app matched, start inject");


    write_file_log("INJECT: install_anti_detect done");

    void* libc_handle = dlopen("libc.so", RTLD_LAZY);
    if (libc_handle)
    {
        void* sym = dlsym(libc_handle, "android_dlopen_ext");
        if (sym)
        {
            int ret = inline_hook(sym, (void*)hooked_android_dlopen_ext, (void**)&orig_android_dlopen_ext);
            if (ret == 0)
            {
                LOGI("hook android_dlopen_ext success");
                write_file_log("INJECT: hook android_dlopen_ext success");
            }
            else
            {
                LOGE("hook android_dlopen_ext failed ret=%d", ret);
                write_file_log("INJECT ERROR: hook android_dlopen_ext failed");
            }
        }
        dlclose(libc_handle);
    }

    LOGI("injection core done");
    write_file_log("INJECT: injection core done");
    return nullptr;
}

static void atfork_child() {
    LOGI("atfork_child callback enter");
    pthread_t tid;
    int ret = pthread_create(&tid, nullptr, injection_thread, nullptr);
    if (ret != 0) {
        LOGE("pthread_create ret=%d", ret);
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
    LOGI("===== ZYGISK MODULE CONSTRUCTOR RUN, pthread_atfork registered =====");
}
