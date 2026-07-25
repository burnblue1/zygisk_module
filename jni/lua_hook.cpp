#include "lua_hook.h"
#include "bridge.h"
#include <dlfcn.h>
#include <cstring>
#include <android/log.h>

#define TAG "LuaHook"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// Lua 函数指针类型
typedef int (*lua_pcall_t)(void* L, int nargs, int nresults, int errfunc);
typedef int (*lua_gettop_t)(void* L);
typedef const char* (*lua_tolstring_t)(void* L, int index, size_t* len);
typedef int (*luaL_loadstring_t)(void* L, const char* s);

// 函数指针
static lua_pcall_t p_lua_pcall = nullptr;
static lua_gettop_t p_lua_gettop = nullptr;
static lua_tolstring_t p_lua_tolstring = nullptr;
static luaL_loadstring_t p_luaL_loadstring = nullptr;

// 全局 Lua State
void* g_lua_state = nullptr;
bool g_lua_ready = false;

// MyBot 基础代码
static const char* MYBOT_CODE = 
    "console = console or {}\n"
    "console.log = function(msg) print('[MyBot] ' .. tostring(msg)) end\n"
    "console.log('MyBot injected via Zygisk!')\n"
    "_G.MyBot = {\n"
    "    version = '1.0.0',\n"
    "    test = function() console.log('MyBot test OK') return 'zygisk' end\n"
    "}\n";

// ============================================
// 执行 Lua 代码
// ============================================
static bool do_lua_string(void* L, const char* code) {
    if (!L || !code || !p_luaL_loadstring || !p_lua_pcall) return false;
    
    if (p_luaL_loadstring(L, code) != 0) {
        const char* err = p_lua_tolstring ? p_lua_tolstring(L, -1, nullptr) : "unknown error";
        LOGE("Lua load error: %s", err);
        return false;
    }
    
    if (p_lua_pcall(L, 0, 0, 0) != 0) {
        const char* err = p_lua_tolstring ? p_lua_tolstring(L, -1, nullptr) : "unknown error";
        LOGE("Lua pcall error: %s", err);
        return false;
    }
    
    return true;
}

// ============================================
// 检查 lua_State 有效性
// ============================================
static bool is_valid_L(void* L) {
    if (!L || !p_lua_gettop) return false;
    int top = p_lua_gettop(L);
    return (top >= 0 && top < 100000);
}

// ============================================
// Hook lua_pcall 来捕获 lua_State
// ============================================
static void install_lua_pcall_hook() {
    // 这里需要 inline hook 框架（如 Dobby）
    // 简化版：通过轮询 + 导出函数方式
    
    // 如果拿到 lua_State，直接注入
    if (g_lua_state && is_valid_L(g_lua_state) && !g_lua_ready) {
        if (do_lua_string(g_lua_state, MYBOT_CODE)) {
            g_lua_ready = true;
            LOGI("MyBot injected successfully");
            BridgeServer::getInstance().sendLog("INFO", "MyBot Lua code injected");
            
            // 设置 Bridge 的 Lua 执行器
            BridgeServer::getInstance().setLuaExecutor([&](const std::string& code) -> bool {
                return do_lua_string(g_lua_state, code.c_str());
            });
        }
    }
}

// ============================================
// 扫描 lua_State
// ============================================
static void* scan_for_lua_state() {
    // 方法1: 从 libtolua.so 的数据段读取全局变量
    void* tolua = dlopen("libtolua.so", RTLD_NOLOAD);
    if (!tolua) return nullptr;
    
    // 方法2: Hook lua_pcall 获取第一个参数
    // 这里简化处理，实际需要 inline hook
    
    return nullptr;
}

// ============================================
// 初始化 Lua 函数
// ============================================
static bool init_lua_funcs(void* tolua_handle) {
    p_lua_pcall = (lua_pcall_t)dlsym(tolua_handle, "lua_pcall");
    p_lua_gettop = (lua_gettop_t)dlsym(tolua_handle, "lua_gettop");
    p_lua_tolstring = (lua_tolstring_t)dlsym(tolua_handle, "lua_tolstring");
    p_luaL_loadstring = (luaL_loadstring_t)dlsym(tolua_handle, "luaL_loadstring");
    
    if (!p_lua_pcall) LOGI("lua_pcall: NOT FOUND");
    if (!p_lua_gettop) LOGI("lua_gettop: NOT FOUND");
    if (!p_luaL_loadstring) LOGI("luaL_loadstring: NOT FOUND");
    
    if (p_lua_pcall && p_luaL_loadstring) {
        LOGI("Core Lua functions found");
        return true;
    }
    
    // 尝试其他库
    const char* alt_libs[] = {"libcocos2dlua.so", "libxlua.so", "libluajit.so", nullptr};
    for (int i = 0; alt_libs[i]; i++) {
        void* h = dlopen(alt_libs[i], RTLD_NOLOAD);
        if (h) {
            LOGI("Trying %s...", alt_libs[i]);
            if (!p_lua_pcall) p_lua_pcall = (lua_pcall_t)dlsym(h, "lua_pcall");
            if (!p_lua_gettop) p_lua_gettop = (lua_gettop_t)dlsym(h, "lua_gettop");
            if (!p_luaL_loadstring) p_luaL_loadstring = (luaL_loadstring_t)dlsym(h, "luaL_loadstring");
            if (p_lua_pcall && p_luaL_loadstring) {
                LOGI("Found functions in %s", alt_libs[i]);
                return true;
            }
        }
    }
    
    return (p_lua_pcall && p_luaL_loadstring);
}

// ============================================
// 主入口
// ============================================
void hook_lua_functions(void* tolua_handle) {
    LOGI("Hooking Lua functions...");
    
    if (!init_lua_funcs(tolua_handle)) {
        LOGE("Failed to find Lua functions");
        BridgeServer::getInstance().sendLog("WARN", "Lua functions not found");
        return;
    }
    
    // 尝试扫描 lua_State
    g_lua_state = scan_for_lua_state();
    
    // 启动轮询线程，等待 lua_State 出现
    std::thread([]() {
        auto& bridge = BridgeServer::getInstance();
        
        for (int i = 0; i < 300; i++) { // 最多等 5 分钟
            if (g_lua_ready) break;
            
            install_lua_pcall_hook();
            
            if (!g_lua_state) {
                g_lua_state = scan_for_lua_state();
            }
            
            if (g_lua_state && !g_lua_ready) {
                if (is_valid_L(g_lua_state)) {
                    if (do_lua_string(g_lua_state, MYBOT_CODE)) {
                        g_lua_ready = true;
                        bridge.sendLog("INFO", "MyBot injected (delayed)");
                        bridge.setLuaExecutor([&](const std::string& code) -> bool {
                            return do_lua_string(g_lua_state, code.c_str());
                        });
                    }
                }
            }
            
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
        if (!g_lua_ready) {
            bridge.sendLog("WARN", "Lua injection timeout");
        }
    }).detach();
}