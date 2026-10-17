#pragma once
#include <android/log.h>

#define MW_TAG "MWMENU"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  MW_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  MW_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, MW_TAG, __VA_ARGS__)
