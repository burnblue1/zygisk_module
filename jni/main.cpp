#include <android/log.h>
#define TAG "TEST_ZYGISK"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)

__attribute__((constructor))
void ctor_test(void){
    LOGD("######## TEST SO SUCCESS ########");
}
