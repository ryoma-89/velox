// Velox: AOSP libbase の GetProperty の代替（NDK の __system_property_get を使う）。
// ゲストの Vulkan ドライバーを NDK でビルドするとき、プラットフォームの libbase.so
// （プラットフォームの libc++ ABI）にリンクしないために使う。
#pragma once
#include <string>

namespace android {
namespace base {
std::string GetProperty(const std::string& key, const std::string& default_value);
bool GetBoolProperty(const std::string& key, bool default_value);
}  // namespace base
}  // namespace android
