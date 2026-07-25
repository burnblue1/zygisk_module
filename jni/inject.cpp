#include "inject.h"
#include <unistd.h>       
#include <chrono>

void* wait_for_library(const char* name, int timeout_ms) {
    auto start = std::chrono::steady_clock::now();
    void* handle = nullptr;
    
    while (!handle) {
        handle = dlopen(name, RTLD_NOLOAD);
        if (handle) break;
        
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
        if (elapsed > timeout_ms) break;
        
        usleep(100000); // 100ms
    }
    
    return handle;
}
