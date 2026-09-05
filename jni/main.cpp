#include <android/log.h>
#include <dlfcn.h>
#include "zygisk_next_api.h"

#define TAG "TEST_ZYGISK"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)

// ========== 功能代码 ==========
__attribute__((constructor))
void ctor_test(void) {
    LOGD("######## TEST SO SUCCESS ########");
}

// ========== Zygisk Next 入口结构 ==========
static void on_module_loaded(void *arg, const struct ZygiskNextAPI *api) {
    LOGD("Zygisk Next entry loaded");
    // 如果还需要在这里做其他初始化，可以加
}

#ifdef __cplusplus
extern "C" {
#endif
__attribute__((visibility("default"))) 
struct ZygiskNextModule zn_module = {
    .target_api_version = ZYGISK_NEXT_API_VERSION,
    .onModuleLoaded = on_module_loaded,
};
#ifdef __cplusplus
}
#endif
