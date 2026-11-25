// Velox: libcutils の property_get の代替（ヘッダーのみ、__system_property_get を使う）。
#pragma once
#include <string.h>
#include <sys/system_properties.h>

#ifndef PROPERTY_KEY_MAX
#define PROPERTY_KEY_MAX PROP_NAME_MAX
#endif
#ifndef PROPERTY_VALUE_MAX
#define PROPERTY_VALUE_MAX PROP_VALUE_MAX
#endif

static inline int property_get(const char* key, char* value, const char* default_value) {
    int len = __system_property_get(key, value);
    if (len > 0) return len;
    if (default_value) {
        len = (int)strnlen(default_value, PROPERTY_VALUE_MAX - 1);
        memcpy(value, default_value, (size_t)len);
        value[len] = '\0';
        return len;
    }
    value[0] = '\0';
    return 0;
}
