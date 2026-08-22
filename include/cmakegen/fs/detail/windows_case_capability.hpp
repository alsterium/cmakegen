#pragma once

#include <optional>

namespace cmakegen::detail {

enum class WindowsVolumeCaseSensitivity {
    indeterminate,
    case_insensitive,
    case_sensitive,
};

[[nodiscard]] constexpr std::optional<bool> windows_directory_case_query_fallback(
    WindowsVolumeCaseSensitivity volume_case_sensitivity) noexcept {
    if (volume_case_sensitivity == WindowsVolumeCaseSensitivity::case_insensitive) return false;
    return std::nullopt;
}

} // namespace cmakegen::detail
