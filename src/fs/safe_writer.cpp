#include "cmakegen/fs/safe_writer.hpp"
#include "cmakegen/fs/detail/windows_case_capability.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <climits>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cctype>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <system_error>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <sys/attr.h>
#include <sys/mount.h>
#endif

namespace cmakegen {
namespace {

using Hash = std::array<std::uint8_t, 32>;
constexpr std::string_view manifest_relative_path = ".cmakegen/manifest.json";

struct Manifest {
    std::map<std::string, std::string, std::less<>> files;
};

[[nodiscard]] DiagnosticSet error(std::string message, std::string path = {}) {
    DiagnosticSet diagnostics;
    diagnostics.add({std::string{diagnostic_codes::filesystem_error}, DiagnosticSeverity::error,
                     std::move(message), std::move(path), std::nullopt});
    return diagnostics;
}

// Generated trees must be portable to the default case-insensitive file
// systems used by macOS and Windows.  Keep this deliberately ASCII-only.  In
// particular, do not use the process locale or std::tolower for this identity:
// manifests and collision decisions must be stable on every host.
[[nodiscard]] std::string ascii_case_fold(std::string_view path) {
    std::string result{path};
    for (auto& character : result) {
        const auto value = static_cast<unsigned char>(character);
        if (value >= static_cast<unsigned char>('A') && value <= static_cast<unsigned char>('Z')) {
            character = static_cast<char>(value + ('a' - 'A'));
        }
    }
    return result;
}

// Ordinary Win32 paths disregard a component's trailing dots and spaces.  Use
// that identity everywhere we reason about portable output ownership, even on
// hosts whose native filesystem permits those names.
[[nodiscard]] std::string portable_equivalence_key(std::string_view path) {
    std::string result;
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto end = path.find('/', start);
        auto component = path.substr(start, end == std::string_view::npos ? path.size() - start : end - start);
        while (!component.empty() && (component.back() == '.' || component.back() == ' ')) component.remove_suffix(1);
        if (!result.empty()) result += '/';
        result += ascii_case_fold(component);
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return result;
}

[[nodiscard]] bool is_reserved_metadata_path(std::string_view path) {
    const auto key = portable_equivalence_key(path);
    return key == ".cmakegen" || key.starts_with(".cmakegen/");
}

[[nodiscard]] std::vector<std::string_view> path_components(std::string_view path);

[[nodiscard]] bool has_non_ascii(std::string_view path) {
    return std::ranges::any_of(path, [](char character) {
        return static_cast<unsigned char>(character) >= 0x80U;
    });
}

// std::filesystem::path::string()/generic_string() use the active narrow
// code page on Windows.  Path identities, manifests, and the Win32 Unicode
// API instead use UTF-8 throughout, so never route a native Windows path
// through that lossy representation.
[[nodiscard]] std::string generic_utf8_string(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

[[nodiscard]] std::filesystem::path path_from_utf8(std::string_view value) {
    std::u8string utf8;
    utf8.reserve(value.size());
    for (const auto byte : value) utf8.push_back(static_cast<char8_t>(static_cast<unsigned char>(byte)));
    return std::filesystem::path{utf8};
}

[[nodiscard]] std::string trim_win32_components(std::string_view path) {
    std::string result;
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto end = path.find('/', start);
        auto component = path.substr(start, end == std::string_view::npos ? path.size() - start : end - start);
        while (!component.empty() && (component.back() == '.' || component.back() == ' ')) component.remove_suffix(1);
        if (!result.empty()) result += '/';
        result.append(component);
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return result;
}

enum class UnicodeFilesystemMode {
    unsupported,
    macos_case_sensitive,
    macos_case_insensitive,
    windows_case_sensitive,
    windows_case_insensitive,
};

struct FilesystemCapabilities {
    std::filesystem::path project_root;
    UnicodeFilesystemMode unicode_mode = UnicodeFilesystemMode::unsupported;
#ifdef _WIN32
    detail::WindowsVolumeCaseSensitivity windows_volume_case_sensitivity =
        detail::WindowsVolumeCaseSensitivity::indeterminate;
#endif

    [[nodiscard]] bool supports_unicode() const noexcept {
        return unicode_mode != UnicodeFilesystemMode::unsupported;
    }

    [[nodiscard]] Result<void> validate_path(std::string_view path) const;
    [[nodiscard]] Result<bool> equivalent(std::string_view left, std::string_view right) const;
    [[nodiscard]] Result<bool> equivalent_in_directory(const std::filesystem::path& parent,
                                                        std::string_view left,
                                                        std::string_view right) const;
    [[nodiscard]] Result<bool> equivalent_path(std::string_view left, std::string_view right) const;
};

#ifdef _WIN32
[[nodiscard]] Result<bool> query_windows_directory_case_sensitive(
    const FilesystemCapabilities& capabilities, const std::filesystem::path& directory);
#endif

#ifdef __APPLE__
[[nodiscard]] Result<UnicodeFilesystemMode> detect_macos_unicode_mode(const std::filesystem::path& directory) {
    struct statfs volume{};
    if (::statfs(directory.c_str(), &volume) != 0) return UnicodeFilesystemMode::unsupported;

    struct attrlist attributes{};
    attributes.bitmapcount = ATTR_BIT_MAP_COUNT;
    attributes.volattr = ATTR_VOL_CAPABILITIES;
    struct CapabilitiesBuffer {
        std::uint32_t length;
        vol_capabilities_attr_t capabilities;
    } buffer{};
    if (::getattrlist(directory.c_str(), &attributes, &buffer, sizeof(buffer), 0) != 0 ||
        buffer.length < sizeof(buffer)) {
        return UnicodeFilesystemMode::unsupported;
    }

    constexpr auto format = VOL_CAPABILITIES_FORMAT;
    constexpr std::uint32_t case_sensitive = VOL_CAP_FMT_CASE_SENSITIVE;
    if ((buffer.capabilities.valid[format] & case_sensitive) == 0) return UnicodeFilesystemMode::unsupported;

    const std::string filesystem_type{volume.f_fstypename};
    if (filesystem_type != "apfs" && filesystem_type != "hfs") return UnicodeFilesystemMode::unsupported;
    return (buffer.capabilities.capabilities[format] & case_sensitive) != 0
               ? UnicodeFilesystemMode::macos_case_sensitive
               : UnicodeFilesystemMode::macos_case_insensitive;
}

[[nodiscard]] Result<std::string> macos_unicode_key(std::string_view path, bool case_insensitive) {
    std::string result;
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto end = path.find('/', start);
        auto component = path.substr(start, end == std::string_view::npos ? path.size() - start : end - start);
        while (!component.empty() && (component.back() == '.' || component.back() == ' ')) component.remove_suffix(1);

        auto* input = CFStringCreateWithBytes(kCFAllocatorDefault,
                                               reinterpret_cast<const UInt8*>(component.data()),
                                               static_cast<CFIndex>(component.size()), kCFStringEncodingUTF8,
                                               false);
        if (input == nullptr) {
            return std::unexpected{error("cannot decode a generated path as UTF-8", std::string{path})};
        }
        auto* normalized = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, input);
        CFRelease(input);
        if (normalized == nullptr) {
            return std::unexpected{error("cannot allocate a Unicode path comparison key", std::string{path})};
        }

        // APFS and HFS compare normalization-insensitively.  Form D is used
        // only as a comparison representation; the requested spelling is
        // retained for output and manifest rendering.
        CFStringNormalize(normalized, kCFStringNormalizationFormD);
        if (case_insensitive) {
            // A null locale requests Core Foundation's canonical case-folding
            // behavior rather than the process or user locale.
            CFStringFold(normalized, kCFCompareCaseInsensitive, nullptr);
        }

        const auto maximum = CFStringGetMaximumSizeForEncoding(CFStringGetLength(normalized),
                                                                kCFStringEncodingUTF8) + 1;
        std::string encoded(static_cast<std::size_t>(maximum), '\0');
        const auto converted = CFStringGetCString(normalized, encoded.data(), static_cast<CFIndex>(encoded.size()),
                                                  kCFStringEncodingUTF8);
        CFRelease(normalized);
        if (!converted) {
            return std::unexpected{error("cannot encode a Unicode path comparison key as UTF-8", std::string{path})};
        }
        encoded.resize(std::strlen(encoded.c_str()));

        if (!result.empty()) result += '/';
        result += encoded;
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return result;
}

[[nodiscard]] Result<bool> macos_unicode_equivalent_in_directories(
    const std::filesystem::path& left_parent, const std::filesystem::path& right_parent,
    std::string_view left, std::string_view right) {
    auto left_mode = detect_macos_unicode_mode(left_parent);
    if (!left_mode) return std::unexpected{left_mode.error()};
    auto right_mode = detect_macos_unicode_mode(right_parent);
    if (!right_mode) return std::unexpected{right_mode.error()};
    if (*left_mode == UnicodeFilesystemMode::unsupported || *right_mode == UnicodeFilesystemMode::unsupported) {
        return std::unexpected{error("cannot determine Unicode filesystem equivalence safely; target filesystem capability is unsupported",
                                     std::string{left})};
    }

    const bool case_insensitive = *left_mode == UnicodeFilesystemMode::macos_case_insensitive ||
                                  *right_mode == UnicodeFilesystemMode::macos_case_insensitive;
    auto left_key = macos_unicode_key(left, case_insensitive);
    if (!left_key) return std::unexpected{left_key.error()};
    auto right_key = macos_unicode_key(right, case_insensitive);
    if (!right_key) return std::unexpected{right_key.error()};
    return *left_key == *right_key;
}
#endif

#ifdef _WIN32
[[nodiscard]] Result<std::wstring> utf8_to_wide(std::string_view value) {
    if (value.size() > static_cast<std::size_t>(INT_MAX)) {
        return std::unexpected{error("generated path is too long for the Windows Unicode API", std::string{value})};
    }
    const auto source_length = static_cast<int>(value.size());
    const auto required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), source_length, nullptr, 0);
    if (required <= 0) {
        return std::unexpected{error("cannot decode a generated path as UTF-8", std::string{value})};
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), source_length, result.data(), required) <= 0) {
        return std::unexpected{error("cannot decode a generated path as UTF-8", std::string{value})};
    }
    return result;
}

[[nodiscard]] Result<bool> windows_unicode_equal(std::string_view left, std::string_view right,
                                                 bool ignore_case) {
    if (portable_equivalence_key(left) == portable_equivalence_key(right)) return true;
    auto left_wide = utf8_to_wide(trim_win32_components(left));
    if (!left_wide) return std::unexpected{left_wide.error()};
    auto right_wide = utf8_to_wide(trim_win32_components(right));
    if (!right_wide) return std::unexpected{right_wide.error()};
    const auto comparison = CompareStringOrdinal(left_wide->data(), static_cast<int>(left_wide->size()),
                                                  right_wide->data(), static_cast<int>(right_wide->size()),
                                                  ignore_case ? TRUE : FALSE);
    if (comparison == 0) {
        return std::unexpected{error("Windows Unicode path comparison failed", std::string{left})};
    }
    return comparison == CSTR_EQUAL;
}
#endif

Result<void> FilesystemCapabilities::validate_path(std::string_view path) const {
    if (!has_non_ascii(path)) return {};
    if (!supports_unicode()) {
        return std::unexpected{error("cannot determine Unicode filesystem equivalence safely; target filesystem capability is unsupported",
                                     std::string{path})};
    }
#ifdef __APPLE__
    if (unicode_mode == UnicodeFilesystemMode::macos_case_sensitive ||
        unicode_mode == UnicodeFilesystemMode::macos_case_insensitive) {
        auto key = macos_unicode_key(path, unicode_mode == UnicodeFilesystemMode::macos_case_insensitive);
        if (!key) return std::unexpected{key.error()};
        return {};
    }
#endif
#ifdef _WIN32
    if (unicode_mode == UnicodeFilesystemMode::windows_case_sensitive ||
        unicode_mode == UnicodeFilesystemMode::windows_case_insensitive) {
        auto wide = utf8_to_wide(trim_win32_components(path));
        if (!wide) return std::unexpected{wide.error()};
        return {};
    }
#endif
    return std::unexpected{error("cannot determine Unicode filesystem equivalence safely; target filesystem capability is unsupported",
                                 std::string{path})};
}

[[nodiscard]] Result<FilesystemCapabilities> detect_filesystem_capabilities(const std::filesystem::path& root) {
    FilesystemCapabilities result;
    result.project_root = std::filesystem::absolute(root).lexically_normal();
#ifdef __APPLE__
    auto mode = detect_macos_unicode_mode(result.project_root);
    if (!mode) return std::unexpected{mode.error()};
    result.unicode_mode = *mode;
#elif defined(_WIN32)
    const auto native_root = result.project_root.native();
    std::wstring volume_path(32768, L'\0');
    if (!GetVolumePathNameW(native_root.c_str(), volume_path.data(), static_cast<DWORD>(volume_path.size()))) {
        return result;
    }
    volume_path.resize(std::wcslen(volume_path.c_str()));

    DWORD filesystem_flags = 0;
    if (!GetVolumeInformationW(volume_path.c_str(), nullptr, 0, nullptr, nullptr, &filesystem_flags, nullptr, 0)) {
        return result;
    }
    const bool volume_case_sensitive = (filesystem_flags & FILE_CASE_SENSITIVE_SEARCH) != 0;
    bool case_sensitive = volume_case_sensitive;
    result.windows_volume_case_sensitivity =
        volume_case_sensitive ? detail::WindowsVolumeCaseSensitivity::case_sensitive
                              : detail::WindowsVolumeCaseSensitivity::case_insensitive;

    // FileCaseSensitiveInfo was added for Windows 10 per-directory case
    // sensitivity.  Keep the numeric enum value local so older SDK headers
    // can still build the generator; a failed query is indeterminate.
    constexpr auto file_case_sensitive_info = static_cast<FILE_INFO_BY_HANDLE_CLASS>(23);
    struct CaseSensitiveInformation {
        ULONG flags;
    } information{};
    const auto handle = CreateFileW(native_root.c_str(), FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return result;
    const auto queried = GetFileInformationByHandleEx(handle, file_case_sensitive_info, &information,
                                                      sizeof(information));
    CloseHandle(handle);
    if (queried) {
        case_sensitive = (information.flags & 0x1U) != 0;
    } else if (volume_case_sensitive) {
        return result;
    }
    result.unicode_mode = case_sensitive ? UnicodeFilesystemMode::windows_case_sensitive
                                         : UnicodeFilesystemMode::windows_case_insensitive;
#else
    (void)root;
#endif
    return result;
}

#ifdef _WIN32
[[nodiscard]] Result<bool> query_windows_directory_case_sensitive(
    const FilesystemCapabilities& capabilities, const std::filesystem::path& directory) {
    const auto handle = CreateFileW(directory.c_str(), FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        if (const auto fallback = detail::windows_directory_case_query_fallback(
                capabilities.windows_volume_case_sensitivity)) {
            return *fallback;
        }
        return std::unexpected{error("cannot determine Windows directory case sensitivity",
                                     generic_utf8_string(directory))};
    }

    constexpr auto file_case_sensitive_info = static_cast<FILE_INFO_BY_HANDLE_CLASS>(23);
    struct CaseSensitiveInformation {
        ULONG flags;
    } information{};
    const auto queried = GetFileInformationByHandleEx(handle, file_case_sensitive_info, &information,
                                                      sizeof(information));
    CloseHandle(handle);
    if (!queried) {
        if (const auto fallback = detail::windows_directory_case_query_fallback(
                capabilities.windows_volume_case_sensitivity)) {
            return *fallback;
        }
        return std::unexpected{error("cannot determine Windows directory case sensitivity",
                                     generic_utf8_string(directory))};
    }
    return (information.flags & 0x1U) != 0;
}

[[nodiscard]] Result<std::filesystem::path> resolve_windows_existing_directory(
    const std::filesystem::path& parent, std::string_view component, bool parent_case_sensitive) {
    std::error_code ec;
    const auto exact = parent / path_from_utf8(component);
    (void)std::filesystem::symlink_status(exact, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
        return std::unexpected{error("cannot inspect destination directory: " + ec.message(), exact.generic_string())};
    }

    ec.clear();
    std::filesystem::directory_iterator entries(parent, ec);
    if (ec == std::errc::no_such_file_or_directory) return exact;
    if (ec) {
        return std::unexpected{error("cannot inspect destination directory: " + ec.message(), parent.generic_string())};
    }

    std::optional<std::filesystem::path> matched;
    for (const auto& entry : entries) {
        auto equivalent = windows_unicode_equal(generic_utf8_string(entry.path().filename()), component,
                                                !parent_case_sensitive);
        if (!equivalent) return std::unexpected{equivalent.error()};
        if (*equivalent) {
            if (matched) {
                return std::unexpected{error("destination has ambiguous equivalent path aliases",
                                             parent.generic_string())};
            }
            matched = entry.path();
        }
    }
    return matched ? *matched : exact;
}

struct WindowsPathCursor {
    std::filesystem::path directory;
    bool exists = true;
    bool case_sensitive = false;
};

[[nodiscard]] Result<WindowsPathCursor> advance_windows_path_cursor(
    const FilesystemCapabilities& capabilities, WindowsPathCursor cursor, std::string_view component) {
    if (!cursor.exists) {
        return WindowsPathCursor{cursor.directory / path_from_utf8(component), false,
                                 cursor.case_sensitive};
    }

    auto next = resolve_windows_existing_directory(cursor.directory, component, cursor.case_sensitive);
    if (!next) return std::unexpected{next.error()};

    std::error_code ec;
    const auto status = std::filesystem::symlink_status(*next, ec);
    if (ec == std::errc::no_such_file_or_directory) {
        return WindowsPathCursor{*next, false, cursor.case_sensitive};
    }
    if (ec) {
        return std::unexpected{error("cannot inspect destination directory: " + ec.message(), next->generic_string())};
    }
    if (!std::filesystem::is_directory(status)) {
        return WindowsPathCursor{*next, false, cursor.case_sensitive};
    }

    auto case_sensitive = query_windows_directory_case_sensitive(capabilities, *next);
    if (!case_sensitive) return std::unexpected{case_sensitive.error()};
    return WindowsPathCursor{*next, true, *case_sensitive};
}

[[nodiscard]] Result<WindowsPathCursor> windows_parent_for_relative(
    const FilesystemCapabilities& capabilities, std::string_view relative_parent) {
    auto case_sensitive = query_windows_directory_case_sensitive(capabilities, capabilities.project_root);
    if (!case_sensitive) return std::unexpected{case_sensitive.error()};
    WindowsPathCursor cursor{capabilities.project_root, true, *case_sensitive};
    if (relative_parent.empty()) return cursor;
    for (const auto component : path_components(relative_parent)) {
        auto advanced = advance_windows_path_cursor(capabilities, cursor, component);
        if (!advanced) return std::unexpected{advanced.error()};
        cursor = *advanced;
    }
    return cursor;
}

[[nodiscard]] Result<bool> windows_equivalent_in_directory(
    const FilesystemCapabilities& capabilities, const std::filesystem::path& parent,
    std::string_view left, std::string_view right) {
    auto case_sensitive = query_windows_directory_case_sensitive(capabilities, parent);
    if (!case_sensitive) return std::unexpected{case_sensitive.error()};
    return windows_unicode_equal(left, right, !*case_sensitive);
}

#endif

#ifdef __APPLE__
struct MacOSPathCursor {
    std::filesystem::path directory;
    std::filesystem::path capability_directory;
    bool exists = true;
};

[[nodiscard]] Result<MacOSPathCursor> advance_macos_path_cursor(
    const FilesystemCapabilities& capabilities, MacOSPathCursor cursor, std::string_view component) {
    if (!cursor.exists) {
        return MacOSPathCursor{cursor.directory / path_from_utf8(component),
                               cursor.capability_directory, false};
    }

    std::error_code ec;
    const auto exact = cursor.directory / path_from_utf8(component);
    const auto exact_status = std::filesystem::symlink_status(exact, ec);
    if (!ec && std::filesystem::is_directory(exact_status)) return MacOSPathCursor{exact, exact, true};
    if (ec && ec != std::errc::no_such_file_or_directory) {
        return std::unexpected{error("cannot inspect destination directory: " + ec.message(), generic_utf8_string(exact))};
    }

    ec.clear();
    std::filesystem::directory_iterator entries(cursor.directory, ec);
    if (ec == std::errc::no_such_file_or_directory) return MacOSPathCursor{exact, cursor.capability_directory, false};
    if (ec) {
        return std::unexpected{error("cannot inspect destination directory: " + ec.message(),
                                     generic_utf8_string(cursor.directory))};
    }
    for (const auto& entry : entries) {
        auto equivalent = capabilities.equivalent_in_directory(
            cursor.directory, generic_utf8_string(entry.path().filename()), component);
        if (!equivalent) return std::unexpected{equivalent.error()};
        if (!*equivalent) continue;
        const auto status = entry.symlink_status(ec);
        if (ec) return std::unexpected{error("cannot inspect destination directory: " + ec.message(),
                                              generic_utf8_string(entry.path()))};
        if (std::filesystem::is_directory(status)) return MacOSPathCursor{entry.path(), entry.path(), true};
        break;
    }
    return MacOSPathCursor{exact, cursor.capability_directory, false};
}

[[nodiscard]] Result<MacOSPathCursor> macos_parent_for_relative(
    const FilesystemCapabilities& capabilities, std::string_view relative_parent) {
    MacOSPathCursor cursor{capabilities.project_root, capabilities.project_root, true};
    for (const auto component : path_components(relative_parent)) {
        auto advanced = advance_macos_path_cursor(capabilities, cursor, component);
        if (!advanced) return std::unexpected{advanced.error()};
        cursor = *advanced;
    }
    return cursor;
}
#endif

// Validate a non-ASCII destination against the capability of the physical
// directory that will contain it.  Pairwise equivalence checks alone do not
// visit an empty target directory, so they cannot establish this safety
// boundary for a single output beneath a nested mount.
[[nodiscard]] Result<void> validate_target_unicode_capability(
    const FilesystemCapabilities& capabilities, std::string_view relative) {
    if (!has_non_ascii(relative)) return {};
#ifdef __APPLE__
    // Validate the UTF-8 spelling without deciding its filesystem capability
    // from the project root: an ASCII-named nested mount can provide the
    // supported target parent for this output.
    auto encoding = macos_unicode_key(relative, false);
    if (!encoding) return std::unexpected{encoding.error()};
    const auto parent = generic_utf8_string(path_from_utf8(relative).parent_path());
    auto cursor = macos_parent_for_relative(capabilities, parent);
    if (!cursor) return std::unexpected{cursor.error()};
    const auto& capability_directory = cursor->exists ? cursor->directory : cursor->capability_directory;
    auto mode = detect_macos_unicode_mode(capability_directory);
    if (!mode) return std::unexpected{mode.error()};
    if (*mode == UnicodeFilesystemMode::unsupported) {
        return std::unexpected{error("cannot determine Unicode filesystem equivalence safely; target filesystem capability is unsupported",
                                     std::string{relative})};
    }
    auto key = macos_unicode_key(relative, *mode == UnicodeFilesystemMode::macos_case_insensitive);
    if (!key) return std::unexpected{key.error()};
#endif
#ifdef _WIN32
    auto wide = utf8_to_wide(trim_win32_components(relative));
    if (!wide) return std::unexpected{wide.error()};
    const auto parent = generic_utf8_string(path_from_utf8(relative).parent_path());
    auto cursor = windows_parent_for_relative(capabilities, parent);
    if (!cursor) return std::unexpected{cursor.error()};
#endif
#if !defined(__APPLE__) && !defined(_WIN32)
    auto valid = capabilities.validate_path(relative);
    if (!valid) return std::unexpected{valid.error()};
#endif
    return {};
}

[[nodiscard]] Result<std::string> normalize_relative_path(const std::filesystem::path& path) {
    if (path.native().find('\\') != std::filesystem::path::string_type::npos) {
        return std::unexpected{error("generated path must use forward slashes", path.generic_string())};
    }
    if (path.native().find('\0') != std::filesystem::path::string_type::npos) {
        return std::unexpected{error("generated path must not contain NUL bytes", path.generic_string())};
    }
    if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
        return std::unexpected{error("generated path must be a non-empty relative path", path.generic_string())};
    }
    const auto normalized = path.lexically_normal();
    // A generated entry always denotes a file.  Preserve this distinction
    // before the write phase: paths ending in a separator (including
    // `directory/.`) have no filename and would otherwise reach filesystem
    // operations as directory destinations.
    if (normalized == "." || normalized.filename().empty()) {
        return std::unexpected{error("generated path must name a file", path.generic_string())};
    }
    for (const auto& component : normalized) {
        auto portable_component = generic_utf8_string(component);
        while (!portable_component.empty() &&
               (portable_component.back() == '.' || portable_component.back() == ' ')) {
            portable_component.pop_back();
        }
        if (portable_component.empty()) {
            return std::unexpected{error("generated path contains a component empty under Win32 rules",
                                         path.generic_string())};
        }
        if (component == "..") {
            return std::unexpected{error("generated path cannot escape the project root", path.generic_string())};
        }
    }
    const auto result = generic_utf8_string(normalized);
    if (is_reserved_metadata_path(result)) {
        return std::unexpected{error("generated path is reserved for cmakegen metadata", result)};
    }
    return result;
}

[[nodiscard]] std::vector<std::string_view> path_components(std::string_view path) {
    std::vector<std::string_view> result;
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto end = path.find('/', start);
        result.emplace_back(path.substr(start, end == std::string_view::npos ? path.size() - start : end - start));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return result;
}

Result<bool> FilesystemCapabilities::equivalent_in_directory(const std::filesystem::path& parent,
                                                              std::string_view left,
                                                              std::string_view right) const {
    if (portable_equivalence_key(left) == portable_equivalence_key(right)) return true;
    // Canonical normalization and case folding cannot make a non-ASCII name
    // equivalent to a distinct ASCII name.  Avoid probing an unrelated
    // Unicode entry when an ASCII request is being resolved on a host that
    // has no Unicode filesystem capability.
    if (!has_non_ascii(left) || !has_non_ascii(right)) return false;

#ifdef __APPLE__
    return macos_unicode_equivalent_in_directories(parent, parent, left, right);
#else
    auto left_valid = validate_path(left);
    if (!left_valid) return std::unexpected{left_valid.error()};
    auto right_valid = validate_path(right);
    if (!right_valid) return std::unexpected{right_valid.error()};
#ifdef _WIN32
    if (unicode_mode == UnicodeFilesystemMode::windows_case_sensitive ||
        unicode_mode == UnicodeFilesystemMode::windows_case_insensitive) {
        return windows_equivalent_in_directory(*this, parent, left, right);
    }
#else
    (void)parent;
#endif
    return std::unexpected{error("cannot determine Unicode filesystem equivalence safely; target filesystem capability is unsupported",
                                 std::string{left})};
#endif
}

Result<bool> FilesystemCapabilities::equivalent_path(std::string_view left, std::string_view right) const {
    if (portable_equivalence_key(left) == portable_equivalence_key(right)) return true;
    if (!has_non_ascii(left) && !has_non_ascii(right)) return false;

#ifdef _WIN32
    const auto left_components = path_components(left);
    const auto right_components = path_components(right);
    if (left_components.size() != right_components.size()) return false;

    std::string left_parent;
    std::string right_parent;
    for (std::size_t index = 0; index < left_components.size(); ++index) {
        auto left_cursor = windows_parent_for_relative(*this, left_parent);
        if (!left_cursor) return std::unexpected{left_cursor.error()};
        auto right_cursor = windows_parent_for_relative(*this, right_parent);
        if (!right_cursor) return std::unexpected{right_cursor.error()};
        auto left_valid = validate_path(left_components[index]);
        if (!left_valid) return std::unexpected{left_valid.error()};
        auto right_valid = validate_path(right_components[index]);
        if (!right_valid) return std::unexpected{right_valid.error()};
        const bool ignore_case = !(left_cursor->case_sensitive && right_cursor->case_sensitive);
        auto component_equivalent = windows_unicode_equal(left_components[index], right_components[index], ignore_case);
        if (!component_equivalent) return std::unexpected{component_equivalent.error()};
        if (!*component_equivalent) return false;
        if (!left_parent.empty()) left_parent += '/';
        left_parent += left_components[index];
        if (!right_parent.empty()) right_parent += '/';
        right_parent += right_components[index];
    }
    return true;
#elif defined(__APPLE__)
    const auto left_components = path_components(left);
    const auto right_components = path_components(right);
    if (left_components.size() != right_components.size()) return false;

    std::string left_parent;
    std::string right_parent;
    for (std::size_t index = 0; index < left_components.size(); ++index) {
        const auto left_key = portable_equivalence_key(left_components[index]);
        const auto right_key = portable_equivalence_key(right_components[index]);
        if (left_key != right_key) {
            if (!has_non_ascii(left_components[index]) && !has_non_ascii(right_components[index])) return false;
            auto left_cursor = macos_parent_for_relative(*this, left_parent);
            if (!left_cursor) return std::unexpected{left_cursor.error()};
            auto right_cursor = macos_parent_for_relative(*this, right_parent);
            if (!right_cursor) return std::unexpected{right_cursor.error()};
            const auto left_capability_directory = left_cursor->exists ? left_cursor->directory
                                                                        : left_cursor->capability_directory;
            const auto right_capability_directory = right_cursor->exists ? right_cursor->directory
                                                                          : right_cursor->capability_directory;
            auto component_equivalent = macos_unicode_equivalent_in_directories(
                left_capability_directory, right_capability_directory, left_components[index], right_components[index]);
            if (!component_equivalent) return std::unexpected{component_equivalent.error()};
            if (!*component_equivalent) return false;
        }
        if (!left_parent.empty()) left_parent += '/';
        left_parent += left_components[index];
        if (!right_parent.empty()) right_parent += '/';
        right_parent += right_components[index];
    }
    return true;
#else
    return equivalent_in_directory(project_root, left, right);
#endif
}

Result<bool> FilesystemCapabilities::equivalent(std::string_view left, std::string_view right) const {
    return equivalent_path(left, right);
}

[[nodiscard]] Result<bool> is_path_ancestor(std::string_view ancestor, std::string_view candidate,
                                             const FilesystemCapabilities& capabilities) {
    const auto ancestor_components = path_components(ancestor);
    const auto candidate_components = path_components(candidate);
    if (ancestor_components.size() >= candidate_components.size()) return false;
    std::string left_prefix;
    std::string right_prefix;
    for (std::size_t index = 0; index < ancestor_components.size(); ++index) {
        if (!left_prefix.empty()) left_prefix += '/';
        left_prefix += ancestor_components[index];
        if (!right_prefix.empty()) right_prefix += '/';
        right_prefix += candidate_components[index];
        auto equivalent = capabilities.equivalent(left_prefix, right_prefix);
        if (!equivalent) return std::unexpected{equivalent.error()};
        if (!*equivalent) return false;
    }
    return true;
}

[[nodiscard]] Result<void> reject_file_ancestor(const std::filesystem::path& root,
                                                 const std::filesystem::path& relative,
                                                 const FilesystemCapabilities& capabilities,
                                                 const std::vector<std::filesystem::path>& removable_files = {}) {
    // Resolve each existing ancestor through the same equivalence relation as
    // output and manifest comparisons.  This prevents a Unicode alias from
    // turning an existing file into a directory ancestor on a filesystem that
    // treats the names as equivalent.
    auto existing_component = [&capabilities](const std::filesystem::path& parent,
                                              const std::filesystem::path& component)
        -> Result<std::optional<std::filesystem::path>> {
        std::error_code ec;
        const auto exact = parent / component;
        const auto exact_status = std::filesystem::symlink_status(exact, ec);
        if (!ec && exact_status.type() != std::filesystem::file_type::not_found) return exact;
        if (ec && ec != std::errc::no_such_file_or_directory) {
            return std::unexpected{error("cannot inspect destination ancestor: " + ec.message(), exact.generic_string())};
        }
        ec.clear();
        std::filesystem::directory_iterator entries(parent, ec);
        if (ec == std::errc::no_such_file_or_directory) return std::optional<std::filesystem::path>{};
        if (ec) return std::unexpected{error("cannot inspect destination ancestor: " + ec.message(), parent.generic_string())};
        for (const auto& entry : entries) {
            auto equivalent = capabilities.equivalent_in_directory(
                parent, generic_utf8_string(entry.path().filename()), generic_utf8_string(component));
            if (!equivalent) return std::unexpected{equivalent.error()};
            if (*equivalent) {
                return entry.path();
            }
        }
        return std::optional<std::filesystem::path>{};
    };

    std::error_code ec;
    auto current = root;
    for (const auto& component : relative.parent_path()) {
        auto matched = existing_component(current, component);
        if (!matched) return std::unexpected{matched.error()};
        if (!*matched) return {};
        current = **matched;
        const auto status = std::filesystem::symlink_status(current, ec);
        if (ec) return std::unexpected{error("cannot inspect destination ancestor: " + ec.message(), current.generic_string())};
        const bool removable_regular_file = std::filesystem::is_regular_file(status) &&
                                           std::ranges::find(removable_files, current) != removable_files.end();
        if (!std::filesystem::is_directory(status) && !removable_regular_file) {
            return std::unexpected{error("generated path collides with an existing file ancestor", current.generic_string())};
        }
    }
    return {};
}

[[nodiscard]] Result<void> reject_symlink_parents(const std::filesystem::path& root,
                                                   const std::filesystem::path& relative,
                                                   const FilesystemCapabilities& capabilities) {
    std::error_code ec;
    auto current = root;
    for (const auto& component : relative.parent_path()) {
        const auto exact = current / component;
        const auto exact_status = std::filesystem::symlink_status(exact, ec);
        (void)exact_status;
        if (!ec && exact_status.type() != std::filesystem::file_type::not_found) {
            current = exact;
        } else {
            if (ec && ec != std::errc::no_such_file_or_directory) {
                return std::unexpected{error("cannot inspect destination: " + ec.message(), exact.generic_string())};
            }
            ec.clear();
            std::filesystem::directory_iterator entries(current, ec);
            if (ec == std::errc::no_such_file_or_directory) break;
            if (ec) return std::unexpected{error("cannot inspect destination: " + ec.message(), current.generic_string())};
            std::optional<std::filesystem::path> matched;
            for (const auto& entry : entries) {
                auto equivalent = capabilities.equivalent_in_directory(
                    current, generic_utf8_string(entry.path().filename()), generic_utf8_string(component));
                if (!equivalent) return std::unexpected{equivalent.error()};
                if (*equivalent) {
                    matched = entry.path();
                    break;
                }
            }
            if (!matched) break;
            current = *matched;
        }
        const auto status = std::filesystem::symlink_status(current, ec);
        if (ec) {
            return std::unexpected{error("cannot inspect destination: " + ec.message(), current.generic_string())};
        }
        if (std::filesystem::is_symlink(status)) {
            return std::unexpected{error("destination parent cannot be a symbolic link", current.generic_string())};
        }
    }
    return {};
}

// Resolve an already-existing portable alias so that a case-only (or Win32
// trailing-dot/space-only) spelling change cannot create a second physical
// output when the generator runs on a case-sensitive host.
[[nodiscard]] Result<std::filesystem::path> resolve_existing_alias(const std::filesystem::path& root,
                                                                    const std::filesystem::path& relative,
                                                                    const FilesystemCapabilities& capabilities) {
    std::error_code ec;
    auto current = root;
    for (const auto& component : relative) {
        const auto exact = current / component;
        const auto exact_status = std::filesystem::symlink_status(exact, ec);
        if (ec == std::errc::not_a_directory) {
            // An earlier component is a stale owned file scheduled for
            // removal. Return the unresolved suffix so the write preflight
            // can treat it as absent after validating that removal.
            ec.clear();
            return exact;
        }
        if (ec && ec != std::errc::no_such_file_or_directory) {
            return std::unexpected{error("cannot inspect destination: " + ec.message(), exact.generic_string())};
        }
        ec.clear();
        std::filesystem::directory_iterator entries(current, ec);
        if (ec == std::errc::no_such_file_or_directory) {
            current = exact;
            continue;
        }
        // A stale generated file can be replaced by a directory in this run.
        // Its hash has already been verified before resolving new outputs, so
        // defer the ancestor transition until the commit phase.
        if (ec == std::errc::not_a_directory) {
            ec.clear();
            current = exact;
            continue;
        }
        if (ec) return std::unexpected{error("cannot inspect destination: " + ec.message(), current.generic_string())};
        std::optional<std::filesystem::path> matched;
        for (const auto& entry : entries) {
            auto equivalent = capabilities.equivalent_in_directory(
                current, generic_utf8_string(entry.path().filename()), generic_utf8_string(component));
            if (!equivalent) return std::unexpected{equivalent.error()};
            if (*equivalent) {
                if (matched) {
                    return std::unexpected{error("destination has ambiguous equivalent path aliases", current.generic_string())};
                }
                matched = entry.path();
            }
        }
        // Do not short-circuit on an exact spelling: a case-sensitive host can
        // contain both Foo.txt (manifest-owned) and foo.txt (unowned).  Either
        // spelling would collide on macOS/Windows, so ownership lookup must not
        // select the exact entry and overwrite it before this ambiguity is seen.
        current = matched ? *matched : exact;
    }
    return current;
}

[[nodiscard]] Result<std::string> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::unexpected{error("cannot read file", path.generic_string())};
    }
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] Result<Manifest> parse_manifest(const std::string& source,
                                               const std::filesystem::path& path) {
    try {
        const auto json = nlohmann::json::parse(source);
        if (!json.is_object() || json.value("version", 0) != 1 || !json.contains("files") || !json["files"].is_array()) {
            return std::unexpected{error("manifest has an unsupported format", path.generic_string())};
        }
        Manifest manifest;
        std::set<std::string, std::less<>> portable_paths;
        for (const auto& entry : json["files"]) {
            if (!entry.is_object() || !entry.contains("path") || !entry.contains("sha256") ||
                !entry["path"].is_string() || !entry["sha256"].is_string()) {
                return std::unexpected{error("manifest contains an invalid file entry", path.generic_string())};
            }
            auto normalized = normalize_relative_path(path_from_utf8(entry["path"].get<std::string>()));
            if (!normalized || *normalized != entry["path"].get<std::string>() ||
                !portable_paths.emplace(portable_equivalence_key(*normalized)).second ||
                !manifest.files.emplace(*normalized, entry["sha256"].get<std::string>()).second) {
                return std::unexpected{error("manifest contains duplicate or unsafe paths", path.generic_string())};
            }
        }
        return manifest;
    } catch (const nlohmann::json::exception&) {
        return std::unexpected{error("manifest is not valid JSON", path.generic_string())};
    }
}

[[nodiscard]] Result<Manifest> load_manifest(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        if (ec) return std::unexpected{error("cannot inspect manifest: " + ec.message(), path.generic_string())};
        return Manifest{};
    }
    auto source = read_file(path);
    if (!source) return std::unexpected{source.error()};
    return parse_manifest(*source, path);
}

[[nodiscard]] Result<std::optional<std::string>> find_equivalent_manifest_path(
    const Manifest& manifest, std::string_view relative, const FilesystemCapabilities& capabilities) {
    for (const auto& [path, hash] : manifest.files) {
        (void)hash;
        auto equivalent = capabilities.equivalent(path, relative);
        if (!equivalent) return std::unexpected{equivalent.error()};
        if (*equivalent) return path;
    }
    return std::optional<std::string>{};
}

[[nodiscard]] Result<void> validate_manifest_equivalence(const Manifest& manifest,
                                                          const FilesystemCapabilities& capabilities) {
    for (const auto& [path, hash] : manifest.files) {
        (void)hash;
        auto valid = validate_target_unicode_capability(capabilities, path);
        if (!valid) return std::unexpected{valid.error()};
    }
    for (auto left = manifest.files.begin(); left != manifest.files.end(); ++left) {
        for (auto right = std::next(left); right != manifest.files.end(); ++right) {
            auto equivalent = capabilities.equivalent(left->first, right->first);
            if (!equivalent) return std::unexpected{equivalent.error()};
            if (*equivalent) {
                return std::unexpected{error("manifest contains equivalent file paths", right->first)};
            }
            auto left_ancestor = is_path_ancestor(left->first, right->first, capabilities);
            if (!left_ancestor) return std::unexpected{left_ancestor.error()};
            auto right_ancestor = is_path_ancestor(right->first, left->first, capabilities);
            if (!right_ancestor) return std::unexpected{right_ancestor.error()};
            if (*left_ancestor || *right_ancestor) {
                return std::unexpected{error("manifest contains file/ancestor path collisions", right->first)};
            }
        }
    }
    return {};
}

[[nodiscard]] std::string render_manifest(const Manifest& manifest) {
    std::ostringstream output;
    output << "{\n  \"version\": 1,\n  \"files\": [";
    bool first = true;
    for (const auto& [path, hash] : manifest.files) {
        output << (first ? "\n" : ",\n") << "    {\"path\": " << nlohmann::json(path).dump()
               << ", \"sha256\": " << nlohmann::json(hash).dump() << '}';
        first = false;
    }
    if (!first) output << '\n';
    output << "  ]\n}\n";
    return output.str();
}

[[nodiscard]] std::filesystem::path temporary_path(const std::filesystem::path& destination) {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path name{"."};
    name += destination.filename();
    name += ".cmakegen-";
    name += std::to_string(nonce);
    name += ".tmp";
    return destination.parent_path() / name;
}

[[nodiscard]] std::filesystem::path transaction_temp_path(const std::filesystem::path& destination) {
    std::filesystem::path name{"."};
    name += destination.filename();
    name += ".cmakegen-transaction.tmp";
    return destination.parent_path() / name;
}

[[nodiscard]] std::string transaction_backup_name(std::size_t index) {
    std::ostringstream output;
    output << "backups/" << std::setw(6) << std::setfill('0') << index << ".bak";
    return output.str();
}

[[nodiscard]] bool is_not_found(std::error_code error_code) {
    return error_code == std::errc::no_such_file_or_directory;
}

[[nodiscard]] Result<void> write_file_without_replacement(const std::filesystem::path& path,
                                                           std::string_view content) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (!ec && status.type() != std::filesystem::file_type::not_found) {
        return std::unexpected{error("transaction artifact already exists", path.generic_string())};
    }
    if (ec && !is_not_found(ec)) {
        return std::unexpected{error("cannot inspect transaction artifact: " + ec.message(), path.generic_string())};
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return std::unexpected{error("cannot create transaction artifact", path.generic_string())};
    }
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    output.flush();
    output.close();
    if (!output) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        return std::unexpected{error("cannot write transaction artifact", path.generic_string())};
    }
    return {};
}

[[nodiscard]] Result<void> replace_file(const std::filesystem::path& temporary,
                                         const std::filesystem::path& destination) {
    std::error_code ec;
#ifdef _WIN32
    // std::filesystem::rename cannot replace an existing file on Windows.
    // ReplaceFile provides the corresponding same-volume replacement; use
    // MoveFileEx only for the initial creation, where no destination exists.
    if (!::ReplaceFileW(destination.c_str(), temporary.c_str(), nullptr,
                        REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)) {
        const auto replace_error = ::GetLastError();
        if (replace_error != ERROR_FILE_NOT_FOUND ||
            !::MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
            ec = {static_cast<int>(replace_error == ERROR_FILE_NOT_FOUND ? ::GetLastError() : replace_error),
                  std::system_category()};
        }
    }
#else
    std::filesystem::rename(temporary, destination, ec);
#endif
    if (ec) {
        return std::unexpected{error("cannot replace output atomically: " + ec.message(),
                                     destination.generic_string())};
    }
    return {};
}

[[nodiscard]] Result<void> move_to_backup(const std::filesystem::path& source,
                                          const std::filesystem::path& backup) {
    std::error_code ec;
#ifdef _WIN32
    if (!::MoveFileExW(source.c_str(), backup.c_str(), MOVEFILE_WRITE_THROUGH)) {
        ec = {static_cast<int>(::GetLastError()), std::system_category()};
    }
#else
    std::filesystem::rename(source, backup, ec);
#endif
    if (ec) {
        return std::unexpected{error("cannot move generated file into transaction backup: " + ec.message(),
                                     source.generic_string())};
    }
    return {};
}

[[nodiscard]] Result<void> discard_atomic_temporary(const std::filesystem::path& temporary) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(temporary, ec);
    if (is_not_found(ec) || (!ec && status.type() == std::filesystem::file_type::not_found)) return {};
    if (ec) {
        return std::unexpected{error("cannot inspect transaction temporary: " + ec.message(),
                                     temporary.generic_string())};
    }
    if (std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status)) {
        return std::unexpected{error("transaction temporary is not a regular file", temporary.generic_string())};
    }
    std::filesystem::remove(temporary, ec);
    if (ec) {
        return std::unexpected{error("cannot remove stale transaction temporary: " + ec.message(),
                                     temporary.generic_string())};
    }
    return {};
}

[[nodiscard]] Result<void> atomic_write_text(const std::filesystem::path& destination,
                                              std::string_view content) {
    const auto temporary = destination.string() + ".tmp";
    auto discarded = discard_atomic_temporary(temporary);
    if (!discarded) return std::unexpected{discarded.error()};
    auto written = write_file_without_replacement(temporary, content);
    if (!written) return std::unexpected{written.error()};
    auto replaced = replace_file(temporary, destination);
    if (!replaced) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return std::unexpected{replaced.error()};
    }
    return {};
}

constexpr std::array<std::uint32_t, 64> sha_constants = {
    0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
    0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
    0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
    0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
    0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
    0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
    0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
    0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U};

[[nodiscard]] constexpr std::uint32_t rotr(std::uint32_t value, unsigned count) { return (value >> count) | (value << (32U - count)); }

[[nodiscard]] Hash hash_bytes(std::string_view source) {
    std::vector<std::uint8_t> bytes(source.begin(), source.end());
    const auto bit_length = static_cast<std::uint64_t>(bytes.size()) * 8U;
    bytes.push_back(0x80U);
    while ((bytes.size() % 64U) != 56U) bytes.push_back(0U);
    for (int shift = 56; shift >= 0; shift -= 8) bytes.push_back(static_cast<std::uint8_t>(bit_length >> shift));
    std::array<std::uint32_t, 8> state = {0x6a09e667U,0xbb67ae85U,0x3c6ef372U,0xa54ff53aU,0x510e527fU,0x9b05688cU,0x1f83d9abU,0x5be0cd19U};
    for (std::size_t offset = 0; offset < bytes.size(); offset += 64) {
        std::array<std::uint32_t, 64> words{};
        for (unsigned i = 0; i < 16; ++i) words[i] = (static_cast<std::uint32_t>(bytes[offset + i * 4]) << 24U) | (static_cast<std::uint32_t>(bytes[offset + i * 4 + 1]) << 16U) | (static_cast<std::uint32_t>(bytes[offset + i * 4 + 2]) << 8U) | bytes[offset + i * 4 + 3];
        for (unsigned i = 16; i < 64; ++i) { const auto s0 = rotr(words[i-15],7)^rotr(words[i-15],18)^(words[i-15]>>3U); const auto s1 = rotr(words[i-2],17)^rotr(words[i-2],19)^(words[i-2]>>10U); words[i] = words[i-16]+s0+words[i-7]+s1; }
        auto [a,b,c,d,e,f,g,h] = state;
        for (unsigned i = 0; i < 64; ++i) { const auto s1=rotr(e,6)^rotr(e,11)^rotr(e,25); const auto choice=(e&f)^((~e)&g); const auto t1=h+s1+choice+sha_constants[i]+words[i]; const auto s0=rotr(a,2)^rotr(a,13)^rotr(a,22); const auto majority=(a&b)^(a&c)^(b&c); const auto t2=s0+majority; h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2; }
        state[0]+=a;state[1]+=b;state[2]+=c;state[3]+=d;state[4]+=e;state[5]+=f;state[6]+=g;state[7]+=h;
    }
    Hash result{};
    for (unsigned i=0;i<8;++i) for (unsigned byte=0;byte<4;++byte) result[i*4+byte]=static_cast<std::uint8_t>(state[i] >> (24U-byte*8U));
    return result;
}

[[nodiscard]] Result<void> validate_generated_file_equivalence(
    const std::map<std::string, const GeneratedFile*, std::less<>>& files,
    const FilesystemCapabilities& capabilities) {
    for (auto left = files.begin(); left != files.end(); ++left) {
        for (auto right = std::next(left); right != files.end(); ++right) {
            auto equivalent = capabilities.equivalent(left->first, right->first);
            if (!equivalent) return std::unexpected{equivalent.error()};
            if (*equivalent) {
                return std::unexpected{error("generated file set contains equivalent output paths", right->first)};
            }

            auto left_ancestor = is_path_ancestor(left->first, right->first, capabilities);
            if (!left_ancestor) return std::unexpected{left_ancestor.error()};
            auto right_ancestor = is_path_ancestor(right->first, left->first, capabilities);
            if (!right_ancestor) return std::unexpected{right_ancestor.error()};
            if (*left_ancestor || *right_ancestor) {
                return std::unexpected{error("generated file set contains file/ancestor path collisions", right->first)};
            }
        }
    }
    return {};
}

[[nodiscard]] Result<void> validate_generated_staging_collisions(
    const std::map<std::string, const GeneratedFile*, std::less<>>& files,
    const Manifest& post_manifest,
    const FilesystemCapabilities& capabilities) {
    for (const auto& [relative, file] : files) {
        (void)file;
        const auto staging = transaction_temp_path(std::filesystem::path{relative}).generic_string();
        for (const auto& [other, hash] : post_manifest.files) {
            (void)hash;
            auto equivalent = capabilities.equivalent(other, staging);
            if (!equivalent) return std::unexpected{equivalent.error()};
            if (*equivalent) {
                return std::unexpected{error("generated output collides with a transaction staging path",
                                             other)};
            }
            auto staging_ancestor = is_path_ancestor(staging, other, capabilities);
            if (!staging_ancestor) return std::unexpected{staging_ancestor.error()};
            auto output_ancestor = is_path_ancestor(other, staging, capabilities);
            if (!output_ancestor) return std::unexpected{output_ancestor.error()};
            if (*staging_ancestor || *output_ancestor) {
                return std::unexpected{error("generated output collides with a transaction staging path",
                                             other)};
            }
        }
    }
    return {};
}

enum class TransactionEntryKind {
    remove,
    replace,
    manifest,
};

enum class TransactionEntryStatus {
    pending,
    backing_up,
    backed_up,
    staging,
    staged,
    in_progress,
    applied,
    restored,
};

[[nodiscard]] std::string_view entry_kind_name(TransactionEntryKind kind) {
    switch (kind) {
    case TransactionEntryKind::remove:
        return "remove";
    case TransactionEntryKind::replace:
        return "replace";
    case TransactionEntryKind::manifest:
        return "manifest";
    }
    return "replace";
}

[[nodiscard]] std::string_view entry_status_name(TransactionEntryStatus status) {
    switch (status) {
    case TransactionEntryStatus::pending:
        return "pending";
    case TransactionEntryStatus::backing_up:
        return "backing_up";
    case TransactionEntryStatus::backed_up:
        return "backed_up";
    case TransactionEntryStatus::staging:
        return "staging";
    case TransactionEntryStatus::staged:
        return "staged";
    case TransactionEntryStatus::in_progress:
        return "in_progress";
    case TransactionEntryStatus::applied:
        return "applied";
    case TransactionEntryStatus::restored:
        return "restored";
    }
    return "pending";
}

[[nodiscard]] Result<TransactionEntryKind> parse_entry_kind(std::string_view value) {
    if (value == "remove") return TransactionEntryKind::remove;
    if (value == "replace") return TransactionEntryKind::replace;
    if (value == "manifest") return TransactionEntryKind::manifest;
    return std::unexpected{error("transaction journal contains an unknown entry kind")};
}

[[nodiscard]] Result<TransactionEntryStatus> parse_entry_status(std::string_view value) {
    if (value == "pending") return TransactionEntryStatus::pending;
    if (value == "backing_up") return TransactionEntryStatus::backing_up;
    if (value == "backed_up") return TransactionEntryStatus::backed_up;
    if (value == "staging") return TransactionEntryStatus::staging;
    if (value == "staged") return TransactionEntryStatus::staged;
    if (value == "in_progress") return TransactionEntryStatus::in_progress;
    if (value == "applied") return TransactionEntryStatus::applied;
    if (value == "restored") return TransactionEntryStatus::restored;
    return std::unexpected{error("transaction journal contains an unknown entry status")};
}

struct ManifestMetadata {
    bool present = false;
    std::string content;
};

[[nodiscard]] std::string render_manifest_metadata(const ManifestMetadata& metadata) {
    nlohmann::ordered_json json;
    json["version"] = 1;
    json["present"] = metadata.present;
    json["content"] = metadata.content;
    return json.dump(2) + "\n";
}

[[nodiscard]] Result<ManifestMetadata> load_manifest_metadata(const std::filesystem::path& path) {
    auto source = read_file(path);
    if (!source) return std::unexpected{source.error()};
    try {
        const auto json = nlohmann::json::parse(*source);
        if (!json.is_object() || json.value("version", 0) != 1 ||
            !json.contains("present") || !json["present"].is_boolean() ||
            !json.contains("content") || !json["content"].is_string()) {
            return std::unexpected{error("transaction metadata has an unsupported format", path.generic_string())};
        }
        return ManifestMetadata{json["present"].get<bool>(), json["content"].get<std::string>()};
    } catch (const nlohmann::json::exception&) {
        return std::unexpected{error("transaction metadata is not valid JSON", path.generic_string())};
    }
}

struct PlannedEntry {
    TransactionEntryKind kind = TransactionEntryKind::replace;
    std::string logical_path;
    std::filesystem::path destination;
    bool had_original = false;
    std::string pre_hash;
    bool post_exists = false;
    std::string post_hash;
    std::string content;
};

struct TransactionEntry {
    TransactionEntryKind kind = TransactionEntryKind::replace;
    TransactionEntryStatus status = TransactionEntryStatus::pending;
    std::string logical_path;
    std::filesystem::path destination;
    std::filesystem::path temporary;
    std::filesystem::path backup;
    bool had_original = false;
    std::string pre_hash;
    bool post_exists = false;
    std::string post_hash;
    std::string content;
    bool staging_claimed = false;
};

struct Transaction {
    std::filesystem::path root;
    std::filesystem::path directory;
    std::filesystem::path journal;
    std::filesystem::path pre_metadata_path;
    std::filesystem::path post_metadata_path;
    std::filesystem::path backups_directory;
    std::string state = "prepared";
    std::string phase = "prepared";
    bool pre_manifest_exists = false;
    std::string pre_manifest_content;
    bool post_manifest_exists = false;
    std::string post_manifest_content;
    std::vector<TransactionEntry> entries;
    // This is intentionally not serialized.  A path becomes transaction-owned
    // only after create_directory succeeds and it is appended to
    // created_directories below.
    std::vector<std::filesystem::path> planned_directories;
    std::vector<std::filesystem::path> created_directories;
    // Persisted before create_directory so recovery can distinguish an
    // interrupted file-to-directory transition from an unrelated directory.
    std::optional<std::filesystem::path> creating_directory;
    FilesystemCapabilities capabilities;
};

struct PreparedPlan {
    ManifestMetadata pre_manifest;
    ManifestMetadata post_manifest;
    std::vector<PlannedEntry> entries;
    WriteResult result;
    FilesystemCapabilities capabilities;
};

[[nodiscard]] std::string relative_path(const std::filesystem::path& root,
                                        const std::filesystem::path& path) {
    return path.lexically_relative(root).generic_string();
}

[[nodiscard]] std::string backup_relative_path(const Transaction& transaction,
                                               const std::filesystem::path& path) {
    return path.lexically_relative(transaction.directory).generic_string();
}

[[nodiscard]] std::string temporary_relative_path(const Transaction& transaction,
                                                  const std::filesystem::path& path) {
    return path.lexically_relative(transaction.root).generic_string();
}

[[nodiscard]] nlohmann::ordered_json render_journal_json(const Transaction& transaction) {
    nlohmann::ordered_json json;
    json["version"] = 1;
    json["state"] = transaction.state;
    json["phase"] = transaction.phase;
    json["pre_manifest"] = "pre-manifest.json";
    json["post_manifest"] = "post-manifest.json";
    json["created_directories"] = nlohmann::ordered_json::array();
    for (const auto& directory : transaction.created_directories) {
        json["created_directories"].push_back(relative_path(transaction.root, directory));
    }
    if (transaction.creating_directory) {
        json["creating_directory"] = relative_path(transaction.root, *transaction.creating_directory);
    } else {
        json["creating_directory"] = nullptr;
    }
    json["entries"] = nlohmann::ordered_json::array();
    for (const auto& entry : transaction.entries) {
        nlohmann::ordered_json serialized;
        serialized["kind"] = entry_kind_name(entry.kind);
        serialized["status"] = entry_status_name(entry.status);
        serialized["path"] = entry.logical_path;
        serialized["destination"] = relative_path(transaction.root, entry.destination);
        serialized["temporary"] = entry.temporary.empty()
                                        ? ""
                                        : temporary_relative_path(transaction, entry.temporary);
        serialized["backup"] = backup_relative_path(transaction, entry.backup);
        serialized["had_original"] = entry.had_original;
        serialized["pre_sha256"] = entry.pre_hash;
        serialized["post_exists"] = entry.post_exists;
        serialized["post_sha256"] = entry.post_hash;
        serialized["staging_claimed"] = entry.staging_claimed;
        json["entries"].push_back(std::move(serialized));
    }
    return json;
}

[[nodiscard]] Result<void> write_journal(Transaction& transaction) {
    return atomic_write_text(transaction.journal, render_journal_json(transaction).dump(2) + "\n");
}

std::mutex failure_injection_mutex;
std::deque<detail::safe_writer_testing::FailureInjection> failure_injections;

[[nodiscard]] bool consume_failure(detail::safe_writer_testing::FailurePoint point,
                                   std::size_t operation_index) {
    std::scoped_lock lock{failure_injection_mutex};
    if (failure_injections.empty() || failure_injections.front().point != point ||
        failure_injections.front().operation_index != operation_index) {
        return false;
    }
    failure_injections.pop_front();
    return true;
}

[[nodiscard]] std::string failure_point_name(detail::safe_writer_testing::FailurePoint point) {
    using Point = detail::safe_writer_testing::FailurePoint;
    switch (point) {
    case Point::before_replacement:
        return "before replacement";
    case Point::during_staging:
        return "during staging";
    case Point::during_replacement:
        return "during replacement";
    case Point::after_replacement:
        return "after replacement";
    case Point::manifest_update:
        return "manifest update";
    case Point::rollback:
        return "rollback";
    case Point::recovery:
        return "recovery";
    }
    return "transaction";
}

[[nodiscard]] DiagnosticSet injected_failure(std::string_view point,
                                             const std::filesystem::path& path) {
    return error("injected SafeWriter failure " + std::string{point}, path.generic_string());
}

[[nodiscard]] std::filesystem::path transaction_directory(const std::filesystem::path& root) {
    return root / ".cmakegen/transaction";
}

[[nodiscard]] std::filesystem::path transaction_lock_path(const std::filesystem::path& root) {
    return root / ".cmakegen/transaction.lock";
}

class TransactionLock {
public:
    TransactionLock() = default;
    ~TransactionLock() { release(); }

    TransactionLock(const TransactionLock&) = delete;
    TransactionLock& operator=(const TransactionLock&) = delete;

    TransactionLock(TransactionLock&& other) noexcept {
#ifdef _WIN32
        handle_ = other.handle_;
        other.handle_ = INVALID_HANDLE_VALUE;
#else
        descriptor_ = other.descriptor_;
        other.descriptor_ = -1;
#endif
    }

    TransactionLock& operator=(TransactionLock&& other) noexcept {
        if (this == &other) return *this;
        release();
#ifdef _WIN32
        handle_ = other.handle_;
        other.handle_ = INVALID_HANDLE_VALUE;
#else
        descriptor_ = other.descriptor_;
        other.descriptor_ = -1;
#endif
        return *this;
    }

    [[nodiscard]] static Result<TransactionLock> acquire(const std::filesystem::path& path) {
        TransactionLock lock;
#ifdef _WIN32
        const auto handle = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                          nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            return std::unexpected{error("cannot open SafeWriter transaction lock: " +
                                             std::error_code(static_cast<int>(::GetLastError()),
                                                             std::system_category())
                                                 .message(),
                                         path.generic_string())};
        }
        OVERLAPPED overlapped{};
        if (!::LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                          0, MAXDWORD, MAXDWORD, &overlapped)) {
            const auto lock_error = ::GetLastError();
            ::CloseHandle(handle);
            if (lock_error == ERROR_LOCK_VIOLATION || lock_error == ERROR_SHARING_VIOLATION) {
                return std::unexpected{error("another SafeWriter writer is active; transaction recovery is deferred",
                                             path.generic_string())};
            }
            return std::unexpected{error("cannot acquire SafeWriter transaction lock: " +
                                             std::error_code(static_cast<int>(lock_error),
                                                             std::system_category())
                                                 .message(),
                                         path.generic_string())};
        }
        lock.handle_ = handle;
#else
        const auto descriptor = ::open(path.c_str(), O_RDWR | O_CREAT, 0666);
        if (descriptor == -1) {
            return std::unexpected{error("cannot open SafeWriter transaction lock: " +
                                             std::error_code(errno, std::generic_category()).message(),
                                         path.generic_string())};
        }
        if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
            const auto lock_error = errno;
            ::close(descriptor);
            if (lock_error == EWOULDBLOCK || lock_error == EAGAIN) {
                return std::unexpected{error("another SafeWriter writer is active; transaction recovery is deferred",
                                             path.generic_string())};
            }
            return std::unexpected{error("cannot acquire SafeWriter transaction lock: " +
                                             std::error_code(lock_error, std::generic_category()).message(),
                                         path.generic_string())};
        }
        lock.descriptor_ = descriptor;
#endif
        return lock;
    }

private:
    void release() noexcept {
#ifdef _WIN32
        if (handle_ == INVALID_HANDLE_VALUE) return;
        OVERLAPPED overlapped{};
        (void)::UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &overlapped);
        (void)::CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
#else
        if (descriptor_ == -1) return;
        (void)::flock(descriptor_, LOCK_UN);
        (void)::close(descriptor_);
        descriptor_ = -1;
#endif
    }

#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int descriptor_ = -1;
#endif
};

[[nodiscard]] Result<TransactionLock> acquire_transaction_lock(
    const std::filesystem::path& root,
    const FilesystemCapabilities& capabilities) {
    const auto lock_path = transaction_lock_path(root);
    auto parents = reject_symlink_parents(root, ".cmakegen/transaction.lock", capabilities);
    if (!parents) return std::unexpected{parents.error()};

    std::error_code ec;
    std::filesystem::create_directories(root / ".cmakegen", ec);
    if (ec) {
        return std::unexpected{error("cannot create transaction metadata directory: " + ec.message(),
                                     (root / ".cmakegen").generic_string())};
    }
    parents = reject_symlink_parents(root, ".cmakegen/transaction.lock", capabilities);
    if (!parents) return std::unexpected{parents.error()};

    const auto status = std::filesystem::symlink_status(lock_path, ec);
    if (!ec && status.type() != std::filesystem::file_type::not_found &&
        (std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status))) {
        return std::unexpected{error("transaction lock must be a regular file", lock_path.generic_string())};
    }
    if (ec && ec != std::errc::no_such_file_or_directory) {
        return std::unexpected{error("cannot inspect transaction lock: " + ec.message(), lock_path.generic_string())};
    }
    return TransactionLock::acquire(lock_path);
}

[[nodiscard]] Result<bool> transaction_directory_present(const std::filesystem::path& root,
                                                         const FilesystemCapabilities& capabilities) {
    const auto directory = transaction_directory(root);
    auto resolved = resolve_existing_alias(root, ".cmakegen/transaction", capabilities);
    if (!resolved) return std::unexpected{resolved.error()};
    if (*resolved != directory) {
        return std::unexpected{error("transaction metadata has a portable path alias",
                                     resolved->generic_string())};
    }

    std::error_code ec;
    const auto status = std::filesystem::symlink_status(directory, ec);
    if (ec && !is_not_found(ec)) {
        return std::unexpected{error("cannot inspect transaction metadata: " + ec.message(),
                                     directory.generic_string())};
    }
    if (is_not_found(ec) || status.type() == std::filesystem::file_type::not_found) return false;
    if (std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status)) {
        return std::unexpected{error("transaction metadata path is not a directory",
                                     directory.generic_string())};
    }
    return true;
}

[[nodiscard]] Result<void> ensure_manifest_path_is_safe(const std::filesystem::path& root,
                                                        const std::filesystem::path& manifest,
                                                        const FilesystemCapabilities& capabilities) {
    auto resolved = resolve_existing_alias(root, manifest_relative_path, capabilities);
    if (!resolved) return std::unexpected{resolved.error()};
    if (*resolved != manifest) {
        return std::unexpected{error("manifest has a portable path alias", resolved->generic_string())};
    }
    auto parents = reject_symlink_parents(root, manifest_relative_path, capabilities);
    if (!parents) return std::unexpected{parents.error()};

    std::error_code ec;
    const auto status = std::filesystem::symlink_status(manifest, ec);
    if (ec && !is_not_found(ec)) {
        return std::unexpected{error("cannot inspect manifest: " + ec.message(), manifest.generic_string())};
    }
    if (!is_not_found(ec) && status.type() != std::filesystem::file_type::not_found &&
        (std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status))) {
        return std::unexpected{error("manifest must be a regular file", manifest.generic_string())};
    }
    return {};
}

[[nodiscard]] Result<void> validate_transaction_destination(const std::string& value,
                                                             bool allow_manifest,
                                                             const std::filesystem::path& journal,
                                                             const FilesystemCapabilities& capabilities) {
    if (allow_manifest && value == manifest_relative_path) return {};
    auto normalized = normalize_relative_path(std::filesystem::path{value});
    if (!normalized || *normalized != value) {
        return std::unexpected{error("transaction journal contains an unsafe destination", journal.generic_string())};
    }
    return capabilities.validate_path(*normalized);
}

[[nodiscard]] Result<Transaction> load_transaction(const std::filesystem::path& root,
                                                   const FilesystemCapabilities& capabilities) {
    const auto directory = transaction_directory(root);
    const auto journal = directory / "journal.json";
    const auto pre_metadata_path = directory / "pre-manifest.json";
    const auto post_metadata_path = directory / "post-manifest.json";
    const auto backups_directory = directory / "backups";

    auto source = read_file(journal);
    if (!source) return std::unexpected{source.error()};

    Transaction transaction{root, directory, journal, pre_metadata_path, post_metadata_path,
                            backups_directory};
    transaction.capabilities = capabilities;
    try {
        const auto json = nlohmann::json::parse(*source);
        if (!json.is_object() || json.value("version", 0) != 1 ||
            !json.contains("state") || !json["state"].is_string() ||
            !json.contains("phase") || !json["phase"].is_string() ||
            json.value("pre_manifest", "") != "pre-manifest.json" ||
            json.value("post_manifest", "") != "post-manifest.json" ||
            !json.contains("entries") || !json["entries"].is_array() ||
            !json.contains("created_directories") || !json["created_directories"].is_array() ||
            (json.contains("creating_directory") && !json["creating_directory"].is_null() &&
             !json["creating_directory"].is_string())) {
            return std::unexpected{error("transaction journal has an unsupported format", journal.generic_string())};
        }
        transaction.state = json["state"].get<std::string>();
        transaction.phase = json["phase"].get<std::string>();
        if (transaction.state != "prepared" && transaction.state != "applying" &&
            transaction.state != "manifest_pending" && transaction.state != "committed" &&
            transaction.state != "rolled_back" && transaction.state != "recovery_required") {
            return std::unexpected{error("transaction journal has an unknown state", journal.generic_string())};
        }

        auto pre_metadata = load_manifest_metadata(pre_metadata_path);
        if (!pre_metadata) return std::unexpected{pre_metadata.error()};
        auto post_metadata = load_manifest_metadata(post_metadata_path);
        if (!post_metadata) return std::unexpected{post_metadata.error()};
        transaction.pre_manifest_exists = pre_metadata->present;
        transaction.pre_manifest_content = std::move(pre_metadata->content);
        transaction.post_manifest_exists = post_metadata->present;
        transaction.post_manifest_content = std::move(post_metadata->content);

        for (const auto& value : json["created_directories"]) {
            if (!value.is_string()) {
                return std::unexpected{error("transaction journal contains an invalid created directory",
                                             journal.generic_string())};
            }
            auto normalized = normalize_relative_path(std::filesystem::path{value.get<std::string>()});
            if (!normalized || *normalized != value.get<std::string>() ||
                is_reserved_metadata_path(*normalized)) {
                return std::unexpected{error("transaction journal contains an unsafe created directory",
                                             journal.generic_string())};
            }
            auto unicode_valid = capabilities.validate_path(*normalized);
            if (!unicode_valid) return std::unexpected{unicode_valid.error()};
            transaction.created_directories.push_back(root / *normalized);
        }
        if (json.contains("creating_directory") && !json["creating_directory"].is_null()) {
            auto normalized = normalize_relative_path(
                std::filesystem::path{json["creating_directory"].get<std::string>()});
            if (!normalized || *normalized != json["creating_directory"].get<std::string>() ||
                is_reserved_metadata_path(*normalized)) {
                return std::unexpected{error("transaction journal contains an unsafe creating directory",
                                             journal.generic_string())};
            }
            auto unicode_valid = capabilities.validate_path(*normalized);
            if (!unicode_valid) return std::unexpected{unicode_valid.error()};
            transaction.creating_directory = root / *normalized;
        }

        std::size_t index = 0;
        for (const auto& value : json["entries"]) {
            if (!value.is_object() || !value.contains("kind") || !value["kind"].is_string() ||
                !value.contains("status") || !value["status"].is_string() ||
                !value.contains("path") || !value["path"].is_string() ||
                !value.contains("destination") || !value["destination"].is_string() ||
                !value.contains("temporary") || !value["temporary"].is_string() ||
                !value.contains("backup") || !value["backup"].is_string() ||
                !value.contains("had_original") || !value["had_original"].is_boolean() ||
                !value.contains("pre_sha256") || !value["pre_sha256"].is_string() ||
                !value.contains("post_exists") || !value["post_exists"].is_boolean() ||
                !value.contains("post_sha256") || !value["post_sha256"].is_string()) {
                return std::unexpected{error("transaction journal contains an invalid entry",
                                             journal.generic_string())};
            }
            auto kind = parse_entry_kind(value["kind"].get<std::string>());
            if (!kind) return std::unexpected{kind.error()};
            auto status = parse_entry_status(value["status"].get<std::string>());
            if (!status) return std::unexpected{status.error()};
            bool staging_claimed = *status == TransactionEntryStatus::staging ||
                                   *status == TransactionEntryStatus::staged ||
                                   *status == TransactionEntryStatus::in_progress ||
                                   *status == TransactionEntryStatus::applied ||
                                   *status == TransactionEntryStatus::restored;
            if (value.contains("staging_claimed")) {
                if (!value["staging_claimed"].is_boolean()) {
                    return std::unexpected{error("transaction journal contains an invalid staging ownership flag",
                                                 journal.generic_string())};
                }
                staging_claimed = value["staging_claimed"].get<bool>();
            }

            const auto logical_path = value["path"].get<std::string>();
            const auto destination = value["destination"].get<std::string>();
            const bool is_manifest = *kind == TransactionEntryKind::manifest;
            auto safe_destination = validate_transaction_destination(destination, is_manifest, journal, capabilities);
            if (!safe_destination) return std::unexpected{safe_destination.error()};
            if (is_manifest && (logical_path != manifest_relative_path || destination != manifest_relative_path)) {
                return std::unexpected{error("transaction journal contains an invalid manifest entry",
                                             journal.generic_string())};
            }
            if (!is_manifest) {
                auto safe_logical = normalize_relative_path(std::filesystem::path{logical_path});
                if (!safe_logical || *safe_logical != logical_path) {
                    return std::unexpected{error("transaction journal contains an unsafe logical path",
                                                 journal.generic_string())};
                }
                auto unicode_valid = capabilities.validate_path(*safe_logical);
                if (!unicode_valid) return std::unexpected{unicode_valid.error()};
            }
            for (const auto& existing : transaction.entries) {
                auto equivalent = capabilities.equivalent(existing.logical_path, logical_path);
                if (!equivalent) return std::unexpected{equivalent.error()};
                if (*equivalent) {
                    return std::unexpected{error("transaction journal contains duplicate destinations",
                                                 journal.generic_string())};
                }
            }

            const auto expected_backup = transaction_backup_name(index);
            if (value["backup"].get<std::string>() != expected_backup) {
                return std::unexpected{error("transaction journal contains an invalid backup path",
                                             journal.generic_string())};
            }
            const auto temporary_value = value["temporary"].get<std::string>();
            std::filesystem::path temporary;
            if (*kind == TransactionEntryKind::replace ||
                (is_manifest && value["post_exists"].get<bool>())) {
                temporary = root / temporary_value;
                if (temporary_value != transaction_temp_path(root / destination).lexically_relative(root).generic_string()) {
                    return std::unexpected{error("transaction journal contains an invalid temporary path",
                                                 journal.generic_string())};
                }
            } else if (!temporary_value.empty()) {
                return std::unexpected{error("transaction journal contains an unexpected temporary path",
                                             journal.generic_string())};
            }

            const auto pre_hash = value["pre_sha256"].get<std::string>();
            const auto post_hash = value["post_sha256"].get<std::string>();
            if ((value["had_original"].get<bool>() && pre_hash.size() != 64) ||
                (value["post_exists"].get<bool>() && post_hash.size() != 64)) {
                return std::unexpected{error("transaction journal contains an invalid content hash",
                                             journal.generic_string())};
            }
            transaction.entries.push_back(TransactionEntry{
                *kind,
                *status,
                logical_path,
                root / destination,
                temporary,
                directory / expected_backup,
                value["had_original"].get<bool>(),
                pre_hash,
                value["post_exists"].get<bool>(),
                post_hash,
                {},
                staging_claimed});
            ++index;
        }
    } catch (const nlohmann::json::exception&) {
        return std::unexpected{error("transaction journal is not valid JSON", journal.generic_string())};
    }
    return transaction;
}

[[nodiscard]] Result<void> remove_if_present(const std::filesystem::path& path,
                                              std::string_view description) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (is_not_found(ec) || (!ec && status.type() == std::filesystem::file_type::not_found)) return {};
    if (ec) return std::unexpected{error("cannot inspect " + std::string{description} + ": " + ec.message(),
                                         path.generic_string())};
    if (!std::filesystem::remove(path, ec) && ec) {
        return std::unexpected{error("cannot remove " + std::string{description} + ": " + ec.message(),
                                     path.generic_string())};
    }
    return {};
}

[[nodiscard]] Result<void> remove_directory_if_empty(const std::filesystem::path& path) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (is_not_found(ec) || (!ec && status.type() == std::filesystem::file_type::not_found)) return {};
    if (ec) return std::unexpected{error("cannot inspect transaction directory: " + ec.message(), path.generic_string())};
    if (!std::filesystem::is_directory(status) || std::filesystem::is_symlink(status)) {
        return std::unexpected{error("transaction path is not a directory", path.generic_string())};
    }
    if (!std::filesystem::is_empty(path, ec)) {
        if (ec) return std::unexpected{error("cannot inspect transaction directory: " + ec.message(), path.generic_string())};
        return std::unexpected{error("transaction directory contains unexpected artifacts", path.generic_string())};
    }
    std::filesystem::remove(path, ec);
    if (ec) return std::unexpected{error("cannot remove transaction directory: " + ec.message(), path.generic_string())};
    return {};
}

[[nodiscard]] Result<void> remove_metadata_directory_if_empty(const std::filesystem::path& path) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (is_not_found(ec) || (!ec && status.type() == std::filesystem::file_type::not_found)) return {};
    if (ec) return std::unexpected{error("cannot inspect metadata directory: " + ec.message(), path.generic_string())};
    if (std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status)) {
        return std::unexpected{error("metadata path is not a directory", path.generic_string())};
    }
    if (!std::filesystem::is_empty(path, ec)) {
        if (ec) return std::unexpected{error("cannot inspect metadata directory: " + ec.message(), path.generic_string())};
        return {};
    }
    std::filesystem::remove(path, ec);
    if (ec) return std::unexpected{error("cannot remove metadata directory: " + ec.message(), path.generic_string())};
    return {};
}

[[nodiscard]] Result<void> validate_recovery_path_parents(const Transaction& transaction,
                                                           const std::filesystem::path& path);

[[nodiscard]] Result<void> remove_staged_file(const Transaction& transaction,
                                              const TransactionEntry& entry) {
    if (entry.temporary.empty() || !entry.staging_claimed) return {};
    auto parents = validate_recovery_path_parents(transaction, entry.temporary);
    if (!parents) return std::unexpected{parents.error()};
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(entry.temporary, ec);
    if (is_not_found(ec) || ec == std::errc::not_a_directory ||
        (!ec && status.type() == std::filesystem::file_type::not_found)) return {};
    if (ec) return std::unexpected{error("cannot inspect staged output: " + ec.message(),
                                         entry.temporary.generic_string())};
    if (std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status)) {
        return std::unexpected{error("staged output is not a regular file", entry.temporary.generic_string())};
    }
    if (entry.status == TransactionEntryStatus::staging || entry.status == TransactionEntryStatus::staged) {
        // The journal records that this deterministic temporary path was
        // claimed by the transaction before its contents were written.  A
        // crash can therefore leave a short or otherwise partial file here;
        // it is safe to remove the transaction-owned artifact without
        // treating its hash as an external edit.
        return remove_if_present(entry.temporary, "staged output");
    }
    auto content = read_file(entry.temporary);
    if (!content) return std::unexpected{content.error()};
    if (SafeWriter::sha256(*content) != entry.post_hash) {
        return std::unexpected{error("staged output changed outside the active transaction",
                                     entry.temporary.generic_string())};
    }
    parents = validate_recovery_path_parents(transaction, entry.temporary);
    if (!parents) return std::unexpected{parents.error()};
    return remove_if_present(entry.temporary, "staged output");
}

[[nodiscard]] Result<void> cleanup_transaction(Transaction& transaction) {
    for (const auto& entry : transaction.entries) {
        auto staged = remove_staged_file(transaction, entry);
        if (!staged) return std::unexpected{staged.error()};
    }
    auto backup_parents = validate_recovery_path_parents(
        transaction, transaction.backups_directory / ".cmakegen-backup-check");
    if (!backup_parents) return std::unexpected{backup_parents.error()};
    for (const auto& entry : transaction.entries) {
        backup_parents = validate_recovery_path_parents(transaction, entry.backup);
        if (!backup_parents) return std::unexpected{backup_parents.error()};
        auto backup = remove_if_present(entry.backup, "transaction backup");
        if (!backup) return std::unexpected{backup.error()};
    }
    auto journal_tmp = remove_if_present(transaction.journal.string() + ".tmp", "transaction journal temporary");
    if (!journal_tmp) return std::unexpected{journal_tmp.error()};
    auto pre_tmp = remove_if_present(transaction.pre_metadata_path.string() + ".tmp",
                                     "pre-manifest metadata temporary");
    if (!pre_tmp) return std::unexpected{pre_tmp.error()};
    auto post_tmp = remove_if_present(transaction.post_metadata_path.string() + ".tmp",
                                      "post-manifest metadata temporary");
    if (!post_tmp) return std::unexpected{post_tmp.error()};
    // Remove the journal before its metadata.  If cleanup is interrupted
    // after this point, the journal-less recovery path validates and removes
    // only the known transaction-owned scaffolding.
    auto journal = remove_if_present(transaction.journal, "transaction journal");
    if (!journal) return std::unexpected{journal.error()};
    auto pre = remove_if_present(transaction.pre_metadata_path, "pre-manifest metadata");
    if (!pre) return std::unexpected{pre.error()};
    auto post = remove_if_present(transaction.post_metadata_path, "post-manifest metadata");
    if (!post) return std::unexpected{post.error()};
    auto backups = remove_directory_if_empty(transaction.backups_directory);
    if (!backups) return std::unexpected{backups.error()};
    auto directory = remove_directory_if_empty(transaction.directory);
    if (!directory) return std::unexpected{directory.error()};
    // clean() removes the manifest, and a rollback may restore the state where
    // no manifest ever existed.  Remove only an empty metadata directory so
    // unowned files under .cmakegen are never touched.
    return remove_metadata_directory_if_empty(transaction.root / ".cmakegen");
}

[[nodiscard]] bool is_transaction_backup_filename(std::string_view name) {
    if (name.size() != 10 || name.substr(6) != ".bak") return false;
    return std::ranges::all_of(name.substr(0, 6), [](char character) {
        return character >= '0' && character <= '9';
    });
}

[[nodiscard]] bool is_journalless_transaction_file(std::string_view name) {
    return name == "pre-manifest.json" || name == "post-manifest.json" ||
           name == "journal.json.tmp" || name == "pre-manifest.json.tmp" ||
           name == "post-manifest.json.tmp";
}

[[nodiscard]] Result<void> cleanup_journalless_transaction(const std::filesystem::path& root) {
    const auto directory = transaction_directory(root);
    std::error_code ec;
    const auto directory_status = std::filesystem::symlink_status(directory, ec);
    if (is_not_found(ec) || (!ec && directory_status.type() == std::filesystem::file_type::not_found)) return {};
    if (ec) {
        return std::unexpected{error("cannot inspect journal-less transaction: " + ec.message(),
                                     directory.generic_string())};
    }
    if (std::filesystem::is_symlink(directory_status) || !std::filesystem::is_directory(directory_status)) {
        return std::unexpected{error("journal-less transaction path is not a directory", directory.generic_string())};
    }

    std::filesystem::directory_iterator entries(directory, ec);
    if (ec) {
        return std::unexpected{error("cannot inspect journal-less transaction: " + ec.message(),
                                     directory.generic_string())};
    }
    std::vector<std::filesystem::path> metadata_artifacts;
    std::vector<std::filesystem::path> backup_artifacts;
    std::optional<std::filesystem::path> backups_directory;
    for (const auto& entry : entries) {
        const auto name = entry.path().filename().generic_string();
        const auto status = std::filesystem::symlink_status(entry.path(), ec);
        if (ec) {
            return std::unexpected{error("cannot inspect journal-less transaction artifact: " + ec.message(),
                                         entry.path().generic_string())};
        }
        if (name == "backups") {
            if (std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status)) {
                return std::unexpected{error("journal-less transaction backup path is not a directory",
                                             entry.path().generic_string())};
            }
            std::filesystem::directory_iterator backups(entry.path(), ec);
            if (ec) {
                return std::unexpected{error("cannot inspect journal-less transaction backups: " + ec.message(),
                                             entry.path().generic_string())};
            }
            for (const auto& backup : backups) {
                const auto backup_status = std::filesystem::symlink_status(backup.path(), ec);
                if (ec) {
                    return std::unexpected{error("cannot inspect journal-less transaction backup: " + ec.message(),
                                                 backup.path().generic_string())};
                }
                if (std::filesystem::is_symlink(backup_status) ||
                    !std::filesystem::is_regular_file(backup_status) ||
                    !is_transaction_backup_filename(backup.path().filename().generic_string())) {
                    return std::unexpected{error("journal-less transaction contains an unexpected backup artifact",
                                                 backup.path().generic_string())};
                }
                backup_artifacts.push_back(backup.path());
            }
            backups_directory = entry.path();
            continue;
        }
        if (!is_journalless_transaction_file(name) || std::filesystem::is_symlink(status) ||
            !std::filesystem::is_regular_file(status)) {
            return std::unexpected{error("journal-less transaction contains an unexpected artifact",
                                         entry.path().generic_string())};
        }
        metadata_artifacts.push_back(entry.path());
    }

    for (const auto& backup : backup_artifacts) {
        auto removed = remove_if_present(backup, "journal-less transaction backup");
        if (!removed) return std::unexpected{removed.error()};
    }
    if (backups_directory) {
        auto removed_backups = remove_directory_if_empty(*backups_directory);
        if (!removed_backups) return std::unexpected{removed_backups.error()};
    }
    for (const auto& artifact : metadata_artifacts) {
        auto removed = remove_if_present(artifact, "journal-less transaction artifact");
        if (!removed) return std::unexpected{removed.error()};
    }

    auto removed_directory = remove_directory_if_empty(directory);
    if (!removed_directory) return std::unexpected{removed_directory.error()};
    return remove_metadata_directory_if_empty(root / ".cmakegen");
}

[[nodiscard]] Result<void> safe_status(const std::filesystem::path& path,
                                       std::filesystem::file_status& status) {
    std::error_code ec;
    status = std::filesystem::symlink_status(path, ec);
    if (ec && !is_not_found(ec)) {
        if (ec == std::errc::not_a_directory) {
            status = std::filesystem::file_status{};
            return {};
        }
        return std::unexpected{error("cannot inspect transaction path: " + ec.message(), path.generic_string())};
    }
    if (is_not_found(ec)) status = std::filesystem::file_status{};
    return {};
}

[[nodiscard]] Result<void> validate_recovery_path_parents(const Transaction& transaction,
                                                           const std::filesystem::path& path) {
    const auto relative = path.lexically_relative(transaction.root);
    const auto relative_value = relative.generic_string();
    if (relative.empty() || relative.is_absolute() || relative_value == "." ||
        relative_value == ".." || relative_value.starts_with("../")) {
        return std::unexpected{error("transaction recovery path escapes the project root",
                                     path.generic_string())};
    }
    return reject_symlink_parents(transaction.root, relative, transaction.capabilities);
}

[[nodiscard]] Result<void> remove_new_destination(const Transaction& transaction,
                                                  const TransactionEntry& entry,
                                                  bool may_have_been_applied) {
    auto parents = validate_recovery_path_parents(transaction, entry.destination);
    if (!parents) return std::unexpected{parents.error()};
    std::filesystem::file_status status;
    auto inspected = safe_status(entry.destination, status);
    if (!inspected) return std::unexpected{inspected.error()};
    if (!std::filesystem::exists(status)) return {};

    if (!may_have_been_applied) {
        if (entry.had_original && std::filesystem::is_regular_file(status)) {
            auto content = read_file(entry.destination);
            if (content && SafeWriter::sha256(*content) == entry.pre_hash) {
                // The journal may have been persisted as `backing_up` before
                // the move failed.  This is still the original destination,
                // not an output that rollback must remove.
                return {};
            }
        }
        return std::unexpected{error("destination changed while transaction was being rolled back",
                                     entry.destination.generic_string())};
    }
    if (std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status)) {
        return std::unexpected{error("cannot safely rollback a non-regular destination",
                                     entry.destination.generic_string())};
    }
    auto content = read_file(entry.destination);
    if (!content) return std::unexpected{content.error()};
    if (SafeWriter::sha256(*content) != entry.post_hash) {
        return std::unexpected{error("destination changed while transaction recovery was pending",
                                     entry.destination.generic_string())};
    }
    parents = validate_recovery_path_parents(transaction, entry.destination);
    if (!parents) return std::unexpected{parents.error()};
    return remove_if_present(entry.destination, "transaction output");
}

[[nodiscard]] Result<void> restore_backup(Transaction& transaction,
                                          TransactionEntry& entry,
                                          std::size_t rollback_index,
                                          bool recovery_mode) {
    using Point = detail::safe_writer_testing::FailurePoint;
    if (recovery_mode && consume_failure(Point::recovery, rollback_index)) {
        return std::unexpected{injected_failure(failure_point_name(Point::recovery), entry.destination)};
    }
    if (!recovery_mode && consume_failure(Point::rollback, rollback_index)) {
        return std::unexpected{injected_failure(failure_point_name(Point::rollback), entry.destination)};
    }

    auto destination_parents = validate_recovery_path_parents(transaction, entry.destination);
    if (!destination_parents) return std::unexpected{destination_parents.error()};
    auto backup_parents = validate_recovery_path_parents(transaction, entry.backup);
    if (!backup_parents) return std::unexpected{backup_parents.error()};

    std::filesystem::file_status backup_status;
    auto inspected_backup = safe_status(entry.backup, backup_status);
    if (!inspected_backup) return std::unexpected{inspected_backup.error()};
    const bool backup_exists = std::filesystem::exists(backup_status);

    std::filesystem::file_status destination_status;
    auto inspected_destination = safe_status(entry.destination, destination_status);
    if (!inspected_destination) return std::unexpected{inspected_destination.error()};
    const bool destination_exists = std::filesystem::exists(destination_status);

    if (entry.status == TransactionEntryStatus::restored) {
        if (entry.had_original && !destination_exists) {
            return std::unexpected{error("restored transaction output is missing",
                                         entry.destination.generic_string())};
        }
        return {};
    }

    if (!entry.had_original) {
        const bool may_have_been_applied = entry.status == TransactionEntryStatus::in_progress ||
                                           entry.status == TransactionEntryStatus::applied;
        if (destination_exists) {
            auto removed = remove_new_destination(transaction, entry, may_have_been_applied);
            if (!removed) return std::unexpected{removed.error()};
        }
        if (backup_exists) {
            return std::unexpected{error("unexpected backup for a previously absent destination",
                                         entry.backup.generic_string())};
        }
        entry.status = TransactionEntryStatus::restored;
        return {};
    }

    if (!backup_exists) {
        if (destination_exists && entry.status == TransactionEntryStatus::restored) return {};
        if (destination_exists && std::filesystem::is_regular_file(destination_status)) {
            auto content = read_file(entry.destination);
            if (content && SafeWriter::sha256(*content) == entry.pre_hash) {
                entry.status = TransactionEntryStatus::restored;
                return {};
            }
        }
        return std::unexpected{error("transaction backup is missing; manual recovery is required",
                                     entry.backup.generic_string())};
    }

    if (std::filesystem::is_symlink(backup_status) || !std::filesystem::is_regular_file(backup_status)) {
        return std::unexpected{error("transaction backup is not a regular file",
                                     entry.backup.generic_string())};
    }
    auto backup_content = read_file(entry.backup);
    if (!backup_content || SafeWriter::sha256(*backup_content) != entry.pre_hash) {
        return std::unexpected{error("transaction backup content does not match its recorded pre-state",
                                     entry.backup.generic_string())};
    }

    if (destination_exists) {
        const bool may_have_been_applied = entry.status == TransactionEntryStatus::in_progress ||
                                           entry.status == TransactionEntryStatus::applied;
        auto removed = remove_new_destination(transaction, entry, may_have_been_applied);
        if (!removed) return std::unexpected{removed.error()};
    }
    std::filesystem::file_status after_remove;
    auto inspected_after_remove = safe_status(entry.destination, after_remove);
    if (!inspected_after_remove) return std::unexpected{inspected_after_remove.error()};
    if (std::filesystem::exists(after_remove)) {
        return std::unexpected{error("cannot restore transaction backup over an existing destination",
                                     entry.destination.generic_string())};
    }
    destination_parents = validate_recovery_path_parents(transaction, entry.destination);
    if (!destination_parents) return std::unexpected{destination_parents.error()};
    backup_parents = validate_recovery_path_parents(transaction, entry.backup);
    if (!backup_parents) return std::unexpected{backup_parents.error()};
    auto restored = move_to_backup(entry.backup, entry.destination);
    if (!restored) return std::unexpected{restored.error()};
    entry.status = TransactionEntryStatus::restored;
    return {};
}

[[nodiscard]] Result<void> verify_pre_state(const Transaction& transaction) {
    for (const auto& entry : transaction.entries) {
        auto parents = validate_recovery_path_parents(transaction, entry.destination);
        if (!parents) return std::unexpected{parents.error()};
        std::filesystem::file_status status;
        auto inspected = safe_status(entry.destination, status);
        if (!inspected) return std::unexpected{inspected.error()};
        const bool exists = std::filesystem::exists(status);
        if (!entry.had_original) {
            if (exists) return std::unexpected{error("rollback left a newly-created destination",
                                                    entry.destination.generic_string())};
            continue;
        }
        if (!exists || std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status)) {
            return std::unexpected{error("rollback did not restore the previous destination",
                                         entry.destination.generic_string())};
        }
        auto content = read_file(entry.destination);
        if (!content || SafeWriter::sha256(*content) != entry.pre_hash) {
            return std::unexpected{error("rollback restored unexpected destination content",
                                         entry.destination.generic_string())};
        }
    }

    const auto manifest = transaction.root / manifest_relative_path;
    std::filesystem::file_status manifest_status;
    auto inspected_manifest = safe_status(manifest, manifest_status);
    if (!inspected_manifest) return std::unexpected{inspected_manifest.error()};
    const bool manifest_exists = std::filesystem::exists(manifest_status);
    if (manifest_exists != transaction.pre_manifest_exists) {
        return std::unexpected{error("rollback did not restore the previous manifest", manifest.generic_string())};
    }
    if (manifest_exists) {
        auto content = read_file(manifest);
        if (!content || *content != transaction.pre_manifest_content) {
            return std::unexpected{error("rollback restored unexpected manifest content", manifest.generic_string())};
        }
    }

    if (transaction.pre_manifest_exists) {
        auto pre_manifest = parse_manifest(transaction.pre_manifest_content, manifest);
        if (!pre_manifest) return std::unexpected{pre_manifest.error()};
        auto manifest_valid = validate_manifest_equivalence(*pre_manifest, transaction.capabilities);
        if (!manifest_valid) return std::unexpected{manifest_valid.error()};
        for (const auto& [relative, hash] : pre_manifest->files) {
            const auto destination = transaction.root / relative;
            auto parents = validate_recovery_path_parents(transaction, destination);
            if (!parents) return std::unexpected{parents.error()};
            auto resolved = resolve_existing_alias(transaction.root, relative,
                                                   transaction.capabilities);
            if (!resolved) return std::unexpected{resolved.error()};
            if (*resolved != destination) {
                return std::unexpected{error("rollback restored a portable output alias",
                                             destination.generic_string())};
            }
            std::filesystem::file_status output_status;
            auto inspected_output = safe_status(destination, output_status);
            if (!inspected_output) return std::unexpected{inspected_output.error()};
            if (!std::filesystem::exists(output_status) || std::filesystem::is_symlink(output_status) ||
                !std::filesystem::is_regular_file(output_status)) {
                return std::unexpected{error("rollback did not restore a manifest-owned output",
                                             destination.generic_string())};
            }
            auto output = read_file(destination);
            if (!output || SafeWriter::sha256(*output) != hash) {
                return std::unexpected{error("rollback restored unexpected manifest-owned output content",
                                             destination.generic_string())};
            }
        }
    }
    return {};
}

[[nodiscard]] Result<void> remove_created_directories(Transaction& transaction) {
    auto directories = transaction.created_directories;
    std::ranges::sort(directories, [](const auto& left, const auto& right) {
        return left.native().size() > right.native().size();
    });
    for (const auto& directory : directories) {
        auto parents = validate_recovery_path_parents(transaction, directory);
        if (!parents) return std::unexpected{parents.error()};
        std::filesystem::file_status status;
        auto inspected = safe_status(directory, status);
        if (!inspected) return std::unexpected{inspected.error()};
        if (!std::filesystem::exists(status)) continue;
        if (std::filesystem::is_symlink(status)) {
            return std::unexpected{error("created transaction directory changed unexpectedly",
                                         directory.generic_string())};
        }
        if (!std::filesystem::is_directory(status)) {
            const auto restored_file = std::ranges::find_if(transaction.entries,
                                                             [&directory](const auto& entry) {
                                                                 return entry.kind == TransactionEntryKind::remove &&
                                                                        entry.destination == directory &&
                                                                        entry.had_original;
                                                             });
            if (restored_file != transaction.entries.end() &&
                std::filesystem::is_regular_file(status)) {
                auto content = read_file(directory);
                if (content && SafeWriter::sha256(*content) == restored_file->pre_hash) continue;
            }
            return std::unexpected{error("created transaction directory changed unexpectedly",
                                         directory.generic_string())};
        }
        std::error_code ec;
        if (!std::filesystem::is_empty(directory, ec)) {
            if (ec) return std::unexpected{error("cannot inspect created directory: " + ec.message(),
                                                 directory.generic_string())};
            return std::unexpected{error("cannot remove a created directory containing an unowned file",
                                         directory.generic_string())};
        }
        parents = validate_recovery_path_parents(transaction, directory);
        if (!parents) return std::unexpected{parents.error()};
        std::filesystem::remove(directory, ec);
        if (ec) return std::unexpected{error("cannot rollback created directory: " + ec.message(),
                                             directory.generic_string())};
    }
    return {};
}

[[nodiscard]] Result<void> rollback_transaction(Transaction& transaction, bool recovery_mode) {
    // Staged files can live below a directory that is being recreated for a
    // file-to-directory transition. Remove them before removing that parent;
    // the hash check prevents cleanup from touching an external edit.
    for (const auto& entry : transaction.entries) {
        auto staged = remove_staged_file(transaction, entry);
        if (!staged) return std::unexpected{staged.error()};
    }

    std::size_t rollback_index = 0;
    for (auto iterator = transaction.entries.rbegin(); iterator != transaction.entries.rend(); ++iterator) {
        auto& entry = *iterator;
        if (entry.status == TransactionEntryStatus::restored) {
            ++rollback_index;
            continue;
        }
        if (entry.kind == TransactionEntryKind::remove) {
            // A remove operation has no post-write destination.  If the
            // backup move happened, the destination must still be absent.
            auto destination_parents = validate_recovery_path_parents(transaction, entry.destination);
            if (!destination_parents) return std::unexpected{destination_parents.error()};
            std::filesystem::file_status status;
            auto inspected = safe_status(entry.destination, status);
            if (!inspected) return std::unexpected{inspected.error()};
            if (std::filesystem::exists(status)) {
                const bool recreated_for_transition =
                    std::ranges::any_of(transaction.created_directories,
                                        [&entry](const auto& directory) { return directory == entry.destination; });
                bool untouched_original = false;
                if (!recreated_for_transition && entry.status != TransactionEntryStatus::applied &&
                    entry.had_original && std::filesystem::is_regular_file(status)) {
                    auto content = read_file(entry.destination);
                    untouched_original = content && SafeWriter::sha256(*content) == entry.pre_hash;
                }
                if ((!untouched_original && !recreated_for_transition) ||
                    (!untouched_original && (std::filesystem::is_symlink(status) ||
                                             !std::filesystem::is_directory(status)))) {
                    return std::unexpected{error("stale destination changed while transaction recovery was pending",
                                                 entry.destination.generic_string())};
                }
                if (!untouched_original) {
                    std::error_code ec;
                    if (!std::filesystem::is_empty(entry.destination, ec)) {
                        if (ec) {
                            return std::unexpected{error("cannot inspect recreated output directory: " + ec.message(),
                                                         entry.destination.generic_string())};
                        }
                        return std::unexpected{error("cannot remove a recreated output directory containing an unowned file",
                                                     entry.destination.generic_string())};
                    }
                    auto parents = validate_recovery_path_parents(transaction, entry.destination);
                    if (!parents) return std::unexpected{parents.error()};
                    std::filesystem::remove(entry.destination, ec);
                    if (ec) {
                        return std::unexpected{error("cannot remove recreated output directory: " + ec.message(),
                                                     entry.destination.generic_string())};
                    }
                }
            }
        }

        auto restored = restore_backup(transaction, entry, rollback_index, recovery_mode);
        if (!restored) return std::unexpected{restored.error()};
        ++rollback_index;
        transaction.phase = "rollback";
        auto journal = write_journal(transaction);
        if (!journal) return std::unexpected{journal.error()};
    }

    auto directories = remove_created_directories(transaction);
    if (!directories) return std::unexpected{directories.error()};
    auto verified = verify_pre_state(transaction);
    if (!verified) return std::unexpected{verified.error()};
    transaction.creating_directory.reset();
    transaction.state = "rolled_back";
    transaction.phase = "rollback";
    return write_journal(transaction);
}

[[nodiscard]] Result<void> ensure_transaction_directory_is_new(Transaction& transaction) {
    auto present = transaction_directory_present(transaction.root, transaction.capabilities);
    if (!present) return std::unexpected{present.error()};
    if (*present) {
        return std::unexpected{error("an existing SafeWriter transaction requires recovery",
                                     transaction.directory.generic_string())};
    }

    auto metadata_parent = reject_symlink_parents(transaction.root,
                                                   ".cmakegen/transaction/journal.json",
                                                   transaction.capabilities);
    if (!metadata_parent) return std::unexpected{metadata_parent.error()};
    std::error_code ec;
    std::filesystem::create_directories(transaction.directory.parent_path(), ec);
    if (ec) {
        return std::unexpected{error("cannot create transaction metadata directory: " + ec.message(),
                                     transaction.directory.parent_path().generic_string())};
    }
    const bool created = std::filesystem::create_directory(transaction.directory, ec);
    if (ec) {
        return std::unexpected{error("cannot create transaction directory: " + ec.message(),
                                     transaction.directory.generic_string())};
    }
    if (!created) {
        return std::unexpected{error("another SafeWriter transaction acquired the transaction directory",
                                     transaction.directory.generic_string())};
    }
    return {};
}

[[nodiscard]] Result<Transaction> start_transaction(const std::filesystem::path& root,
                                                    const PreparedPlan& plan) {
    Transaction transaction{root,
                            transaction_directory(root),
                            transaction_directory(root) / "journal.json",
                            transaction_directory(root) / "pre-manifest.json",
                            transaction_directory(root) / "post-manifest.json",
                            transaction_directory(root) / "backups"};
    transaction.pre_manifest_exists = plan.pre_manifest.present;
    transaction.pre_manifest_content = plan.pre_manifest.content;
    transaction.post_manifest_exists = plan.post_manifest.present;
    transaction.post_manifest_content = plan.post_manifest.content;
    transaction.capabilities = plan.capabilities;
    transaction.entries.reserve(plan.entries.size());
    for (std::size_t index = 0; index < plan.entries.size(); ++index) {
        const auto& planned = plan.entries[index];
        TransactionEntry entry;
        entry.kind = planned.kind;
        entry.logical_path = planned.logical_path;
        entry.destination = planned.destination;
        entry.had_original = planned.had_original;
        entry.pre_hash = planned.pre_hash;
        entry.post_exists = planned.post_exists;
        entry.post_hash = planned.post_hash;
        entry.content = planned.content;
        entry.backup = transaction.backups_directory / transaction_backup_name(index).substr(8);
        if (planned.kind == TransactionEntryKind::replace ||
            (planned.kind == TransactionEntryKind::manifest && planned.post_exists)) {
            entry.temporary = transaction_temp_path(planned.destination);
        }
        transaction.entries.push_back(std::move(entry));
    }

    auto directory = ensure_transaction_directory_is_new(transaction);
    if (!directory) return std::unexpected{directory.error()};
    auto pre = write_file_without_replacement(transaction.pre_metadata_path,
                                               render_manifest_metadata(plan.pre_manifest));
    if (!pre) {
        auto cleanup = cleanup_transaction(transaction);
        if (!cleanup) {
            return std::unexpected{error("cannot prepare transaction and recovery artifacts could not be cleaned: " +
                                         cleanup.error().diagnostics().front().message,
                                         transaction.directory.generic_string())};
        }
        return std::unexpected{pre.error()};
    }
    auto post = write_file_without_replacement(transaction.post_metadata_path,
                                                render_manifest_metadata(plan.post_manifest));
    if (!post) {
        auto cleanup = cleanup_transaction(transaction);
        if (!cleanup) {
            return std::unexpected{error("cannot prepare transaction and recovery artifacts could not be cleaned: " +
                                         cleanup.error().diagnostics().front().message,
                                         transaction.directory.generic_string())};
        }
        return std::unexpected{post.error()};
    }
    auto journal = atomic_write_text(transaction.journal,
                                     render_journal_json(transaction).dump(2) + "\n");
    if (!journal) {
        auto cleanup = cleanup_transaction(transaction);
        if (!cleanup) {
            return std::unexpected{error("cannot prepare transaction and recovery artifacts could not be cleaned: " +
                                         cleanup.error().diagnostics().front().message,
                                         transaction.directory.generic_string())};
        }
        return std::unexpected{journal.error()};
    }
    return transaction;
}

[[nodiscard]] Result<void> collect_created_directories(Transaction& transaction) {
    std::set<std::string, std::less<>> seen;
    const auto is_scheduled_removal = [&transaction](const std::filesystem::path& path) {
        return std::ranges::any_of(transaction.entries, [&path](const auto& entry) {
            return entry.kind == TransactionEntryKind::remove && entry.destination == path;
        });
    };
    for (const auto& entry : transaction.entries) {
        if (entry.kind != TransactionEntryKind::replace) continue;
        auto current = entry.destination.parent_path();
        while (current != transaction.root && !current.empty()) {
            std::filesystem::file_status status;
            auto inspected = safe_status(current, status);
            if (!inspected) return std::unexpected{inspected.error()};
            if (std::filesystem::exists(status)) {
                if (is_scheduled_removal(current)) {
                    const auto relative = current.lexically_relative(transaction.root).generic_string();
                    if (seen.emplace(portable_equivalence_key(relative)).second) {
                        transaction.planned_directories.push_back(current);
                    }
                    current = current.parent_path();
                    continue;
                }
                if (std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status)) {
                    return std::unexpected{error("output parent is not a directory",
                                                 current.generic_string())};
                }
                break;
            }
            const auto relative = current.lexically_relative(transaction.root).generic_string();
            if (relative.empty() || is_reserved_metadata_path(relative)) {
                return std::unexpected{error("output parent is unsafe", current.generic_string())};
            }
            if (seen.emplace(portable_equivalence_key(relative)).second) {
                transaction.planned_directories.push_back(current);
            }
            current = current.parent_path();
        }
    }
    std::ranges::sort(transaction.planned_directories, [](const auto& left, const auto& right) {
        return left.native().size() < right.native().size();
    });
    return {};
}

[[nodiscard]] Result<void> create_transaction_directories(Transaction& transaction) {
    for (const auto& directory : transaction.planned_directories) {
        transaction.creating_directory = directory;
        auto before = write_journal(transaction);
        if (!before) return std::unexpected{before.error()};
        std::error_code ec;
        const bool created = std::filesystem::create_directory(directory, ec);
        if (ec) return std::unexpected{error("cannot create output directory: " + ec.message(),
                                             directory.generic_string())};
        if (created) {
            transaction.created_directories.push_back(directory);
            // Do not journal planned paths.  Recording ownership only after
            // creation prevents recovery from deleting a directory another
            // process created during the preflight-to-create window.
            auto journal = write_journal(transaction);
            if (!journal) return std::unexpected{journal.error()};
        }
        transaction.creating_directory.reset();
        auto after = write_journal(transaction);
        if (!after) return std::unexpected{after.error()};
    }
    transaction.planned_directories.clear();
    return {};
}

[[nodiscard]] Result<void> stage_transaction_outputs(Transaction& transaction) {
    using Point = detail::safe_writer_testing::FailurePoint;
    std::size_t staging_seen = 0;
    for (auto& entry : transaction.entries) {
        if ((!entry.post_exists) ||
            (entry.kind != TransactionEntryKind::replace && entry.kind != TransactionEntryKind::manifest)) {
            continue;
        }
        std::error_code staging_error;
        const auto staging_status = std::filesystem::symlink_status(entry.temporary, staging_error);
        if (!is_not_found(staging_error) &&
            (staging_error || staging_status.type() != std::filesystem::file_type::not_found)) {
            return std::unexpected{error("transaction staging path already exists",
                                         entry.temporary.generic_string())};
        }
        entry.staging_claimed = true;
        entry.status = TransactionEntryStatus::staging;
        auto before = write_journal(transaction);
        if (!before) return std::unexpected{before.error()};
        if (consume_failure(Point::during_staging, staging_seen)) {
            std::ofstream partial(entry.temporary, std::ios::binary | std::ios::trunc);
            if (partial) {
                const auto partial_size = entry.content.size() / 2;
                partial.write(entry.content.data(), static_cast<std::streamsize>(partial_size));
                partial.close();
            }
            return std::unexpected{injected_failure(failure_point_name(Point::during_staging), entry.temporary)};
        }
        auto staged = write_file_without_replacement(entry.temporary, entry.content);
        if (!staged) return std::unexpected{staged.error()};
        entry.status = TransactionEntryStatus::staged;
        auto after = write_journal(transaction);
        if (!after) return std::unexpected{after.error()};
        ++staging_seen;
    }
    return {};
}

[[nodiscard]] Result<void> backup_transaction_outputs(Transaction& transaction) {
    transaction.state = "applying";
    transaction.phase = "backup";
    auto phase = write_journal(transaction);
    if (!phase) return std::unexpected{phase.error()};

    std::error_code directory_error;
    std::filesystem::create_directories(transaction.backups_directory, directory_error);
    if (directory_error) {
        return std::unexpected{error("cannot create transaction backup directory: " + directory_error.message(),
                                     transaction.backups_directory.generic_string())};
    }

    for (auto& entry : transaction.entries) {
        if (entry.had_original) {
            entry.status = TransactionEntryStatus::backing_up;
            auto before = write_journal(transaction);
            if (!before) return std::unexpected{before.error()};
            auto parents = validate_recovery_path_parents(transaction, entry.destination);
            if (!parents) return std::unexpected{parents.error()};
            std::filesystem::file_status status;
            auto inspected = safe_status(entry.destination, status);
            if (!inspected) return std::unexpected{inspected.error()};
            if (!std::filesystem::exists(status) || std::filesystem::is_symlink(status) ||
                !std::filesystem::is_regular_file(status)) {
                return std::unexpected{error("destination changed before transaction backup",
                                             entry.destination.generic_string())};
            }
            auto content = read_file(entry.destination);
            if (!content || SafeWriter::sha256(*content) != entry.pre_hash) {
                return std::unexpected{error("destination content changed before transaction backup",
                                             entry.destination.generic_string())};
            }
            parents = validate_recovery_path_parents(transaction, entry.destination);
            if (!parents) return std::unexpected{parents.error()};
            auto moved = move_to_backup(entry.destination, entry.backup);
            if (!moved) return std::unexpected{moved.error()};
        }
        entry.status = TransactionEntryStatus::backed_up;
        auto after = write_journal(transaction);
        if (!after) return std::unexpected{after.error()};
    }
    return {};
}

[[nodiscard]] Result<std::pair<bool, std::string>> existing_file_content(
    const std::filesystem::path& path,
    std::string_view description);

[[nodiscard]] Result<bool> post_state_is_complete(const Transaction& transaction) {
    for (const auto& entry : transaction.entries) {
        auto parents = validate_recovery_path_parents(transaction, entry.destination);
        if (!parents) return std::unexpected{parents.error()};
        std::filesystem::file_status status;
        auto inspected = safe_status(entry.destination, status);
        if (!inspected) return std::unexpected{inspected.error()};
        const bool exists = std::filesystem::exists(status);
        if (entry.post_exists != exists) return false;
        if (entry.post_exists) {
            if (std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status)) return false;
            auto content = read_file(entry.destination);
            if (!content || SafeWriter::sha256(*content) != entry.post_hash) return false;
        }
    }
    if (transaction.post_manifest_exists) {
        const auto manifest_path = transaction.root / manifest_relative_path;
        auto post_manifest = parse_manifest(transaction.post_manifest_content, manifest_path);
        if (!post_manifest) return std::unexpected{post_manifest.error()};
        auto manifest_valid = validate_manifest_equivalence(*post_manifest, transaction.capabilities);
        if (!manifest_valid) return std::unexpected{manifest_valid.error()};
        for (const auto& [relative, hash] : post_manifest->files) {
            const auto destination = transaction.root / relative;
            auto parents = validate_recovery_path_parents(transaction, destination);
            if (!parents) return std::unexpected{parents.error()};
            auto resolved = resolve_existing_alias(transaction.root, relative,
                                                   transaction.capabilities);
            if (!resolved) return std::unexpected{resolved.error()};
            if (*resolved != destination) return false;
            auto content = existing_file_content(*resolved, "post-state output");
            if (!content || !content->first || SafeWriter::sha256(content->second) != hash) return false;
        }
    }
    const auto manifest = transaction.root / manifest_relative_path;
    std::filesystem::file_status status;
    auto inspected = safe_status(manifest, status);
    if (!inspected) return std::unexpected{inspected.error()};
    const bool exists = std::filesystem::exists(status);
    if (exists != transaction.post_manifest_exists) return false;
    if (exists) {
        auto content = read_file(manifest);
        if (!content || *content != transaction.post_manifest_content) return false;
    }
    return true;
}

[[nodiscard]] std::string first_diagnostic_message(const DiagnosticSet& diagnostics) {
    if (diagnostics.empty()) return "unknown filesystem error";
    return diagnostics.diagnostics().front().message;
}

[[nodiscard]] DiagnosticSet recovery_required(const Transaction& transaction,
                                              std::string_view cause) {
    return error("SafeWriter transaction is uncommitted and requires operator recovery (" +
                     std::string{cause} + "); journal, backups, and pre/post manifest metadata were retained at " +
                     relative_path(transaction.root, transaction.directory) +
                     ". Stop concurrent writers, resolve any locked destination, then rerun generation to retry recovery.",
                 transaction.directory.generic_string());
}

[[nodiscard]] Result<void> retain_recovery_state(Transaction& transaction) {
    transaction.state = "recovery_required";
    transaction.phase = "rollback";
    auto journal = write_journal(transaction);
    if (!journal) return std::unexpected{journal.error()};
    return {};
}

[[nodiscard]] Result<void> recover_pending_transaction(const std::filesystem::path& root,
                                                       const FilesystemCapabilities& capabilities) {
    auto present = transaction_directory_present(root, capabilities);
    if (!present) return std::unexpected{present.error()};
    if (!*present) return {};

    const auto journal = transaction_directory(root) / "journal.json";
    std::error_code ec;
    const auto journal_status = std::filesystem::symlink_status(journal, ec);
    if (is_not_found(ec) || (!ec && journal_status.type() == std::filesystem::file_type::not_found)) {
        auto cleaned = cleanup_journalless_transaction(root);
        if (!cleaned) {
            return std::unexpected{error("transaction directory has no recoverable journal: " +
                                             cleaned.error().diagnostics().front().message,
                                         transaction_directory(root).generic_string())};
        }
        return {};
    }
    if (ec) return std::unexpected{error("cannot inspect transaction journal: " + ec.message(), journal.generic_string())};
    if (std::filesystem::is_symlink(journal_status) || !std::filesystem::is_regular_file(journal_status)) {
        return std::unexpected{error("transaction journal is not a regular file",
                                     journal.generic_string())};
    }

    auto loaded = load_transaction(root, capabilities);
    if (!loaded) return std::unexpected{loaded.error()};
    auto& transaction = *loaded;
    if (transaction.state == "committed" || transaction.state == "rolled_back") {
        auto cleanup = cleanup_transaction(transaction);
        if (!cleanup) {
            return std::unexpected{recovery_required(transaction, cleanup.error().diagnostics().front().message)};
        }
        return {};
    }

    if (transaction.state == "manifest_pending") {
        auto complete = post_state_is_complete(transaction);
        if (!complete) return std::unexpected{complete.error()};
        if (*complete) {
            transaction.state = "committed";
            transaction.phase = "commit";
            auto journal_update = write_journal(transaction);
            if (!journal_update) {
                return std::unexpected{recovery_required(transaction, journal_update.error().diagnostics().front().message)};
            }
            auto cleanup = cleanup_transaction(transaction);
            if (!cleanup) {
                return std::unexpected{recovery_required(transaction, cleanup.error().diagnostics().front().message)};
            }
            return {};
        }
    }

    auto rollback = rollback_transaction(transaction, true);
    if (!rollback) {
        auto retained = retain_recovery_state(transaction);
        (void)retained;
        return std::unexpected{recovery_required(transaction, rollback.error().diagnostics().front().message)};
    }
    auto cleanup = cleanup_transaction(transaction);
    if (!cleanup) {
        return std::unexpected{recovery_required(transaction, cleanup.error().diagnostics().front().message)};
    }
    return {};
}

[[nodiscard]] Result<bool> pending_transaction_for_read_only_operation(
    const std::filesystem::path& root,
    const FilesystemCapabilities& capabilities) {
    auto present = transaction_directory_present(root, capabilities);
    if (!present) return std::unexpected{present.error()};
    if (!*present) return false;
    return true;
}

[[nodiscard]] Result<void> handle_transaction_failure(Transaction& transaction,
                                                      const DiagnosticSet& cause) {
    auto rollback = rollback_transaction(transaction, false);
    if (rollback) {
        auto cleanup = cleanup_transaction(transaction);
        if (cleanup) return {};
        return std::unexpected{recovery_required(transaction,
                                                  "rollback completed but transaction artifact cleanup failed: " +
                                                      first_diagnostic_message(cleanup.error()))};
    }
    auto retained = retain_recovery_state(transaction);
    (void)retained;
    return std::unexpected{recovery_required(transaction,
                                              "" + first_diagnostic_message(cause) +
                                                  "; rollback failed: " + first_diagnostic_message(rollback.error()))};
}

[[nodiscard]] Result<WriteResult> execute_transaction(Transaction transaction,
                                                      const WriteResult& result) {
    auto fail = [&](const DiagnosticSet& cause) -> Result<WriteResult> {
        auto handled = handle_transaction_failure(transaction, cause);
        if (handled) {
            return std::unexpected{error("transaction failed and rollback completed; previous outputs and manifest were restored: " +
                                         first_diagnostic_message(cause),
                                         transaction.root.generic_string())};
        }
        return std::unexpected{handled.error()};
    };

    auto directories = collect_created_directories(transaction);
    if (!directories) return fail(directories.error());
    auto directory_journal = write_journal(transaction);
    if (!directory_journal) return fail(directory_journal.error());
    auto backed_up = backup_transaction_outputs(transaction);
    if (!backed_up) return fail(backed_up.error());
    auto created = create_transaction_directories(transaction);
    if (!created) return fail(created.error());
    auto staged = stage_transaction_outputs(transaction);
    if (!staged) return fail(staged.error());

    using Point = detail::safe_writer_testing::FailurePoint;
    std::size_t replacement_seen = 0;
    if (consume_failure(Point::before_replacement, 0)) {
        auto injected = injected_failure(failure_point_name(Point::before_replacement), transaction.directory);
        return fail(injected);
    }

    for (auto& entry : transaction.entries) {
        if (entry.kind == TransactionEntryKind::remove ||
            (entry.kind == TransactionEntryKind::manifest && !entry.post_exists)) {
            entry.status = TransactionEntryStatus::applied;
            transaction.phase = "remove";
            auto journal = write_journal(transaction);
            if (!journal) return fail(journal.error());
            continue;
        }
        transaction.phase = entry.kind == TransactionEntryKind::manifest ? "manifest" : "replace";
        if (entry.kind == TransactionEntryKind::manifest) {
            transaction.state = "manifest_pending";
        }
        entry.status = TransactionEntryStatus::in_progress;
        auto before = write_journal(transaction);
        if (!before) return fail(before.error());

        if (entry.kind == TransactionEntryKind::manifest && consume_failure(Point::manifest_update, 0)) {
            auto injected = injected_failure(failure_point_name(Point::manifest_update), entry.destination);
            return fail(injected);
        }
        if (entry.kind == TransactionEntryKind::replace &&
            consume_failure(Point::during_replacement, replacement_seen)) {
            auto injected = injected_failure(failure_point_name(Point::during_replacement), entry.destination);
            return fail(injected);
        }
        auto replaced = replace_file(entry.temporary, entry.destination);
        if (!replaced) return fail(replaced.error());
        entry.status = TransactionEntryStatus::applied;
        // A successful replacement consumes the staging file.  A later file
        // at this deterministic path is not transaction-owned, even if it
        // happens to have the same bytes as the published output.
        entry.staging_claimed = false;
        auto after_apply = write_journal(transaction);
        if (!after_apply) return fail(after_apply.error());
        if (entry.kind == TransactionEntryKind::replace &&
            consume_failure(Point::after_replacement, replacement_seen)) {
            auto injected = injected_failure(failure_point_name(Point::after_replacement), entry.destination);
            return fail(injected);
        }
        if (entry.kind == TransactionEntryKind::replace) ++replacement_seen;
    }

    auto complete = post_state_is_complete(transaction);
    if (!complete) return fail(complete.error());
    if (!*complete) {
        return fail(error("transaction post-state changed before commit", transaction.directory.generic_string()));
    }
    transaction.state = "committed";
    transaction.phase = "commit";
    auto committed = write_journal(transaction);
    if (!committed) return fail(committed.error());
    auto cleanup = cleanup_transaction(transaction);
    if (!cleanup) {
        return std::unexpected{recovery_required(transaction, cleanup.error().diagnostics().front().message)};
    }
    return result;
}

[[nodiscard]] Result<std::pair<bool, std::string>> existing_file_content(
    const std::filesystem::path& path,
    std::string_view description) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (is_not_found(ec) || (!ec && status.type() == std::filesystem::file_type::not_found)) {
        return std::pair<bool, std::string>{false, {}};
    }
    if (ec) return std::unexpected{error("cannot inspect " + std::string{description} + ": " + ec.message(),
                                         path.generic_string())};
    if (std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status)) {
        return std::unexpected{error(std::string{description} + " must be a regular file", path.generic_string())};
    }
    auto content = read_file(path);
    if (!content) return std::unexpected{content.error()};
    return std::pair<bool, std::string>{true, std::move(*content)};
}

[[nodiscard]] Result<PreparedPlan> prepare_write(const std::filesystem::path& root,
                                                 const GeneratedFileSet& files,
                                                 const WriteOptions& options,
                                                 const FilesystemCapabilities& capabilities) {
    const auto manifest_path = root / manifest_relative_path;
    auto manifest_safe = ensure_manifest_path_is_safe(root, manifest_path, capabilities);
    if (!manifest_safe) return std::unexpected{manifest_safe.error()};
    auto old_manifest = load_manifest(manifest_path);
    if (!old_manifest) return std::unexpected{old_manifest.error()};
    auto old_manifest_valid = validate_manifest_equivalence(*old_manifest, capabilities);
    if (!old_manifest_valid) return std::unexpected{old_manifest_valid.error()};

    auto current_manifest = existing_file_content(manifest_path, "manifest");
    if (!current_manifest) return std::unexpected{current_manifest.error()};
    const ManifestMetadata pre_manifest{current_manifest->first, current_manifest->second};

    Manifest next_manifest = options.cleanup_stale ? Manifest{} : *old_manifest;
    std::map<std::string, const GeneratedFile*, std::less<>> next_files;
    for (const auto& file : files.files()) {
        auto normalized = normalize_relative_path(file.path);
        if (!normalized) return std::unexpected{normalized.error()};
        auto unicode_valid = capabilities.validate_path(*normalized);
        if (!unicode_valid) return std::unexpected{unicode_valid.error()};
        auto parents = reject_symlink_parents(root, *normalized, capabilities);
        if (!parents) return std::unexpected{parents.error()};
        if (!next_files.emplace(*normalized, &file).second) {
            return std::unexpected{error("generated file set contains duplicate output paths", *normalized)};
        }
        auto existing = find_equivalent_manifest_path(next_manifest, *normalized, capabilities);
        if (!existing) return std::unexpected{existing.error()};
        if (*existing) next_manifest.files.erase(**existing);
        next_manifest.files.insert_or_assign(*normalized, SafeWriter::sha256(file.content));
    }

    auto output_valid = validate_generated_file_equivalence(next_files, capabilities);
    if (!output_valid) return std::unexpected{output_valid.error()};
    auto staging_valid = validate_generated_staging_collisions(next_files, next_manifest, capabilities);
    if (!staging_valid) return std::unexpected{staging_valid.error()};
    auto next_manifest_valid = validate_manifest_equivalence(next_manifest, capabilities);
    if (!next_manifest_valid) return std::unexpected{next_manifest_valid.error()};

    PreparedPlan plan;
    plan.pre_manifest = pre_manifest;
    plan.capabilities = capabilities;
    std::vector<std::filesystem::path> stale_destinations;
    if (options.cleanup_stale) {
        for (const auto& [relative, hash] : old_manifest->files) {
            bool retained = false;
            for (const auto& [next_relative, file] : next_files) {
                (void)file;
                auto equivalent = capabilities.equivalent(relative, next_relative);
                if (!equivalent) return std::unexpected{equivalent.error()};
                if (*equivalent) {
                    retained = true;
                    break;
                }
            }
            if (retained) continue;
            auto checked = normalize_relative_path(relative);
            if (!checked) return std::unexpected{checked.error()};
            auto unicode_valid = capabilities.validate_path(*checked);
            if (!unicode_valid) return std::unexpected{unicode_valid.error()};
            auto parents = reject_symlink_parents(root, *checked, capabilities);
            if (!parents) return std::unexpected{parents.error()};
            auto resolved = resolve_existing_alias(root, relative, capabilities);
            if (!resolved) return std::unexpected{resolved.error()};
            auto content = existing_file_content(*resolved, "owned stale path");
            if (!content) return std::unexpected{content.error()};
            if (!content->first) continue;
            if (SafeWriter::sha256(content->second) != hash && !options.force) {
                return std::unexpected{error("refusing to remove a generated file modified since its recorded hash; use force to remove it",
                                             relative)};
            }
            stale_destinations.push_back(*resolved);
            plan.entries.push_back(PlannedEntry{TransactionEntryKind::remove, relative, *resolved,
                                                true, SafeWriter::sha256(content->second), false, {}, {}});
        }
    }

    for (const auto& [relative, file] : next_files) {
        auto resolved = resolve_existing_alias(root, relative, capabilities);
        if (!resolved) return std::unexpected{resolved.error()};
        std::error_code ec;
        auto status = std::filesystem::symlink_status(*resolved, ec);
        if (ec == std::errc::not_a_directory) {
            ec.clear();
            status = std::filesystem::file_status{};
        }
        if (ec && !is_not_found(ec)) {
            return std::unexpected{error("cannot inspect destination: " + ec.message(), resolved->generic_string())};
        }
        if (is_not_found(ec)) {
            ec.clear();
            status = std::filesystem::file_status{};
        }
        const bool exists = std::filesystem::exists(status);
        if (exists && (std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status))) {
            return std::unexpected{error("destination must be a regular file", resolved->generic_string())};
        }
        auto old = find_equivalent_manifest_path(*old_manifest, relative, capabilities);
        if (!old) return std::unexpected{old.error()};
        const bool owned = static_cast<bool>(*old);
        if (exists && !owned && !options.force) {
            return std::unexpected{error("refusing to replace an unowned file; use force to take ownership", relative)};
        }

        std::string previous_content;
        std::string previous_hash;
        if (exists) {
            auto current = read_file(*resolved);
            if (!current) return std::unexpected{current.error()};
            previous_content = *current;
            previous_hash = SafeWriter::sha256(previous_content);
            if (owned && previous_hash != old_manifest->files.at(**old) && !options.force) {
                return std::unexpected{error("refusing to replace a generated file modified since its recorded hash; use force to take ownership",
                                             relative)};
            }
        }

        bool changed = !exists || !owned || previous_content != file->content;
        if (changed) {
            plan.entries.push_back(PlannedEntry{TransactionEntryKind::replace,
                                                relative,
                                                *resolved,
                                                exists,
                                                previous_hash,
                                                true,
                                                SafeWriter::sha256(file->content),
                                                file->content});
        }
    }

    for (const auto& [relative, file] : next_files) {
        auto ancestors = reject_file_ancestor(root, relative, capabilities, stale_destinations);
        if (!ancestors) return std::unexpected{ancestors.error()};
    }

    const auto rendered_manifest = render_manifest(next_manifest);
    const bool manifest_changed = !pre_manifest.present || pre_manifest.content != rendered_manifest;
    plan.post_manifest = ManifestMetadata{true, rendered_manifest};
    if (manifest_changed) {
        auto manifest_hash = SafeWriter::sha256(pre_manifest.content);
        plan.entries.push_back(PlannedEntry{TransactionEntryKind::manifest,
                                            std::string{manifest_relative_path},
                                            manifest_path,
                                            pre_manifest.present,
                                            pre_manifest.present ? manifest_hash : std::string{},
                                            true,
                                            SafeWriter::sha256(rendered_manifest),
                                            rendered_manifest});
    }
    plan.result.files_written = std::ranges::count_if(plan.entries, [](const auto& entry) {
        return entry.kind == TransactionEntryKind::replace;
    });
    plan.result.files_removed = std::ranges::count_if(plan.entries, [](const auto& entry) {
        return entry.kind == TransactionEntryKind::remove;
    });
    plan.result.would_change = !plan.entries.empty();
    return plan;
}

[[nodiscard]] Result<PreparedPlan> prepare_clean(const std::filesystem::path& root,
                                                 bool force,
                                                 const FilesystemCapabilities& capabilities) {
    const auto manifest_path = root / manifest_relative_path;
    auto manifest_safe = ensure_manifest_path_is_safe(root, manifest_path, capabilities);
    if (!manifest_safe) return std::unexpected{manifest_safe.error()};
    auto current_manifest = existing_file_content(manifest_path, "manifest");
    if (!current_manifest) return std::unexpected{current_manifest.error()};
    if (!current_manifest->first) return PreparedPlan{};
    auto manifest = load_manifest(manifest_path);
    if (!manifest) return std::unexpected{manifest.error()};
    auto manifest_valid = validate_manifest_equivalence(*manifest, capabilities);
    if (!manifest_valid) return std::unexpected{manifest_valid.error()};

    PreparedPlan plan;
    plan.pre_manifest = ManifestMetadata{true, current_manifest->second};
    plan.post_manifest = ManifestMetadata{false, {}};
    plan.capabilities = capabilities;
    for (const auto& [relative, hash] : manifest->files) {
        auto checked = normalize_relative_path(relative);
        if (!checked) return std::unexpected{checked.error()};
        auto unicode_valid = capabilities.validate_path(*checked);
        if (!unicode_valid) return std::unexpected{unicode_valid.error()};
        auto parents = reject_symlink_parents(root, *checked, capabilities);
        if (!parents) return std::unexpected{parents.error()};
        auto resolved = resolve_existing_alias(root, relative, capabilities);
        if (!resolved) return std::unexpected{resolved.error()};
        auto content = existing_file_content(*resolved, "owned path");
        if (!content) return std::unexpected{content.error()};
        if (!content->first) continue;
        if (SafeWriter::sha256(content->second) != hash && !force) {
            return std::unexpected{error("refusing to remove a generated file modified since its recorded hash; use force to remove it",
                                         relative)};
        }
        plan.entries.push_back(PlannedEntry{TransactionEntryKind::remove,
                                            relative,
                                            *resolved,
                                            true,
                                            SafeWriter::sha256(content->second),
                                            false,
                                            {},
                                            {}});
    }
    plan.entries.push_back(PlannedEntry{TransactionEntryKind::manifest,
                                        std::string{manifest_relative_path},
                                        manifest_path,
                                        true,
                                        SafeWriter::sha256(current_manifest->second),
                                        false,
                                        {},
                                        {}});
    plan.result.files_removed = std::ranges::count_if(plan.entries, [](const auto& entry) {
        return entry.kind == TransactionEntryKind::remove;
    });
    plan.result.would_change = true;
    return plan;
}

} // namespace

namespace detail::safe_writer_testing {

void set_failure_injection(FailureInjection injection) {
    std::scoped_lock lock{failure_injection_mutex};
    failure_injections.clear();
    failure_injections.push_back(injection);
}

void queue_failure_injection(FailureInjection injection) {
    std::scoped_lock lock{failure_injection_mutex};
    failure_injections.push_back(injection);
}

void clear_failure_injection() {
    std::scoped_lock lock{failure_injection_mutex};
    failure_injections.clear();
}

} // namespace detail::safe_writer_testing

Result<void> GeneratedFileSet::add(GeneratedFile file) {
    auto normalized = normalize_relative_path(file.path);
    if (!normalized) return std::unexpected{normalized.error()};
    const auto key = portable_equivalence_key(*normalized);
    for (const auto& existing : files_) {
        const auto existing_path = generic_utf8_string(existing.path);
        const auto existing_key = portable_equivalence_key(existing_path);
        if (existing_key == key) {
            return std::unexpected{error("generated file set contains duplicate portable output paths", *normalized)};
        }
        const auto separator = existing_key + "/";
        const auto new_separator = key + "/";
        if (key.starts_with(separator) || existing_key.starts_with(new_separator)) {
            return std::unexpected{error("generated file set contains file/ancestor path collisions", *normalized)};
        }
    }
    file.path = path_from_utf8(*normalized);
    files_.push_back(std::move(file));
    return {};
}

const std::vector<GeneratedFile>& GeneratedFileSet::files() const noexcept {
    return files_;
}

SafeWriter::SafeWriter(std::filesystem::path project_root) : project_root_(std::move(project_root)) {}

std::string SafeWriter::sha256(std::string_view content) {
    const auto digest = hash_bytes(content);
    std::ostringstream output;
    for (const auto byte : digest) {
        output << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
    }
    return output.str();
}

Result<WriteResult> SafeWriter::write(const GeneratedFileSet& files,
                                      const WriteOptions& options) const {
    std::error_code ec;
    if (!std::filesystem::is_directory(project_root_, ec) || ec) {
        return std::unexpected{error("project root must be an existing directory", project_root_.generic_string())};
    }
    auto detected_capabilities = detect_filesystem_capabilities(project_root_);
    if (!detected_capabilities) return std::unexpected{detected_capabilities.error()};
    const auto& capabilities = *detected_capabilities;
    auto pending = pending_transaction_for_read_only_operation(project_root_, capabilities);
    if (!pending) return std::unexpected{pending.error()};
    std::optional<TransactionLock> transaction_lock;
    const auto acquire_lock = [&]() -> Result<void> {
        if (transaction_lock) return {};
        auto acquired = acquire_transaction_lock(project_root_, capabilities);
        if (!acquired) return std::unexpected{acquired.error()};
        transaction_lock.emplace(std::move(*acquired));
        return {};
    };
    if (*pending) {
        if (options.check || options.dry_run) {
            return std::unexpected{error("a pending SafeWriter transaction requires recovery; check/dry-run performed no writes",
                                         transaction_directory(project_root_).generic_string())};
        }
        auto locked = acquire_lock();
        if (!locked) return std::unexpected{locked.error()};
        auto recovered = recover_pending_transaction(project_root_, capabilities);
        if (!recovered) return std::unexpected{recovered.error()};
    }
    auto plan = prepare_write(project_root_, files, options, capabilities);
    if (!plan) return std::unexpected{plan.error()};
    if (options.check || options.dry_run || !plan->result.would_change) return plan->result;
    auto locked = acquire_lock();
    if (!locked) return std::unexpected{locked.error()};
    auto pending_after_lock = pending_transaction_for_read_only_operation(project_root_, capabilities);
    if (!pending_after_lock) return std::unexpected{pending_after_lock.error()};
    if (*pending_after_lock) {
        auto recovered = recover_pending_transaction(project_root_, capabilities);
        if (!recovered) return std::unexpected{recovered.error()};
    }
    plan = prepare_write(project_root_, files, options, capabilities);
    if (!plan) return std::unexpected{plan.error()};
    if (!plan->result.would_change) return plan->result;
    auto transaction = start_transaction(project_root_, *plan);
    if (!transaction) return std::unexpected{transaction.error()};
    return execute_transaction(std::move(*transaction), plan->result);
}

Result<WriteResult> SafeWriter::clean(bool dry_run, bool force) const {
    std::error_code ec;
    if (!std::filesystem::is_directory(project_root_, ec) || ec) {
        return std::unexpected{error("project root must be an existing directory", project_root_.generic_string())};
    }
    auto detected_capabilities = detect_filesystem_capabilities(project_root_);
    if (!detected_capabilities) return std::unexpected{detected_capabilities.error()};
    const auto& capabilities = *detected_capabilities;
    auto pending = pending_transaction_for_read_only_operation(project_root_, capabilities);
    if (!pending) return std::unexpected{pending.error()};
    std::optional<TransactionLock> transaction_lock;
    const auto acquire_lock = [&]() -> Result<void> {
        if (transaction_lock) return {};
        auto acquired = acquire_transaction_lock(project_root_, capabilities);
        if (!acquired) return std::unexpected{acquired.error()};
        transaction_lock.emplace(std::move(*acquired));
        return {};
    };
    if (*pending) {
        if (dry_run) {
            return std::unexpected{error("a pending SafeWriter transaction requires recovery; clean dry-run performed no writes",
                                         transaction_directory(project_root_).generic_string())};
        }
        auto locked = acquire_lock();
        if (!locked) return std::unexpected{locked.error()};
        auto recovered = recover_pending_transaction(project_root_, capabilities);
        if (!recovered) return std::unexpected{recovered.error()};
    }
    auto plan = prepare_clean(project_root_, force, capabilities);
    if (!plan) return std::unexpected{plan.error()};
    if (dry_run || !plan->result.would_change) return plan->result;
    auto locked = acquire_lock();
    if (!locked) return std::unexpected{locked.error()};
    auto pending_after_lock = pending_transaction_for_read_only_operation(project_root_, capabilities);
    if (!pending_after_lock) return std::unexpected{pending_after_lock.error()};
    if (*pending_after_lock) {
        auto recovered = recover_pending_transaction(project_root_, capabilities);
        if (!recovered) return std::unexpected{recovered.error()};
    }
    plan = prepare_clean(project_root_, force, capabilities);
    if (!plan) return std::unexpected{plan.error()};
    if (!plan->result.would_change) return plan->result;
    auto transaction = start_transaction(project_root_, *plan);
    if (!transaction) return std::unexpected{transaction.error()};
    return execute_transaction(std::move(*transaction), plan->result);
}

} // namespace cmakegen
