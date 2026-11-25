// Velox: android::base::GetProperty の代替実装。
#include "android-base/properties.h"

#include <sys/system_properties.h>

namespace android {
namespace base {

std::string GetProperty(const std::string& key, const std::string& default_value) {
    char value[PROP_VALUE_MAX] = {};
    int len = __system_property_get(key.c_str(), value);
    if (len <= 0) return default_value;
    return std::string(value, static_cast<size_t>(len));
}

bool GetBoolProperty(const std::string& key, bool default_value) {
    const std::string v = GetProperty(key, "");
    if (v == "1" || v == "y" || v == "yes" || v == "on" || v == "true") return true;
    if (v == "0" || v == "n" || v == "no" || v == "off" || v == "false") return false;
    return default_value;
}

}  // namespace base
}  // namespace android
