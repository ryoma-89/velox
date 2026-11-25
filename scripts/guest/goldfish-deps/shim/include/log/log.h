// Velox: AOSP liblog の log/log.h の最小代替（NDK の android/log.h を使う）。
#pragma once
#include <android/log.h>
#include <stdlib.h>

#ifndef LOG_TAG
#define LOG_TAG NULL
#endif

#define ALOGV(...) ((void)0)
#define ALOGD(...) ((void)__android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__))
#define ALOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__))
#define ALOGW(...) ((void)__android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__))
#define ALOGE(...) ((void)__android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__))
#define LOG_ALWAYS_FATAL(...) \
    do { __android_log_print(ANDROID_LOG_FATAL, LOG_TAG, __VA_ARGS__); abort(); } while (0)
#define LOG_ALWAYS_FATAL_IF(cond, ...) \
    do { if (cond) LOG_ALWAYS_FATAL(__VA_ARGS__); } while (0)
#define ALOG_ASSERT(cond, ...) ((void)0)
#define ALOGE_IF(cond, ...) do { if (cond) ALOGE(__VA_ARGS__); } while (0)
#define ALOGW_IF(cond, ...) do { if (cond) ALOGW(__VA_ARGS__); } while (0)
