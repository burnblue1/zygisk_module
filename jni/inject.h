#ifndef INJECT_H
#define INJECT_H

#include <string>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <vector>
#include <dlfcn.h>
#include <android/log.h>

void* wait_for_library(const char* name, int timeout_ms = 60000);

#endif