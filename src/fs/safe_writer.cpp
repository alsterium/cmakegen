#include "cmakegen/fs/safe_writer.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <system_error>

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
    bool windows_volume_case_sensitive = false;
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
    result.windows_volume_case_sensitive = volume_case_sensitive;

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
        if (!capabilities.windows_volume_case_sensitive) return false;
        return std::unexpected{error("cannot determine Windows directory case sensitivity",
                                     directory.generic_string())};
    }

    constexpr auto file_case_sensitive_info = static_cast<FILE_INFO_BY_HANDLE_CLASS>(23);
    struct CaseSensitiveInformation {
        ULONG flags;
    } information{};
    const auto queried = GetFileInformationByHandleEx(handle, file_case_sensitive_info, &information,
                                                      sizeof(information));
    CloseHandle(handle);
    if (!queried) {
        if (!capabilities.windows_volume_case_sensitive) return false;
        return std::unexpected{error("cannot determine Windows directory case sensitivity",
                                     directory.generic_string())};
    }
    return (information.flags & 0x1U) != 0;
}

[[nodiscard]] Result<std::filesystem::path> resolve_windows_existing_directory(
    const std::filesystem::path& parent, std::string_view component, bool parent_case_sensitive) {
    std::error_code ec;
    const auto exact = parent / std::filesystem::path{std::string{component}};
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
        return WindowsPathCursor{cursor.directory / std::filesystem::path{std::string{component}}, false,
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
    const auto insensitive = portable_equivalence_key(result);
    if (insensitive == ".cmakegen" || insensitive.starts_with(".cmakegen/")) {
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
    if (!has_non_ascii(left) && !has_non_ascii(right)) return false;

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

[[nodiscard]] Result<Manifest> load_manifest(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        if (ec) return std::unexpected{error("cannot inspect manifest: " + ec.message(), path.generic_string())};
        return Manifest{};
    }
    auto source = read_file(path);
    if (!source) return std::unexpected{source.error()};
    try {
        const auto json = nlohmann::json::parse(*source);
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
        auto valid = capabilities.validate_path(path);
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
    return destination.parent_path() / ("." + destination.filename().string() + ".cmakegen-" + std::to_string(nonce) + ".tmp");
}

[[nodiscard]] Result<std::filesystem::path> write_temporary(const std::filesystem::path& destination,
                                                             std::string_view content) {
    const auto temporary = temporary_path(destination);
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    output.flush();
    output.close();
    if (!output) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return std::unexpected{error("cannot write temporary output", temporary.generic_string())};
    }
    return temporary;
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
    if (ec) return std::unexpected{error("cannot replace output atomically: " + ec.message(), destination.generic_string())};
    return {};
}

[[nodiscard]] bool is_not_found(std::error_code error_code) {
    return error_code == std::errc::no_such_file_or_directory;
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

} // namespace

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
const std::vector<GeneratedFile>& GeneratedFileSet::files() const noexcept { return files_; }
SafeWriter::SafeWriter(std::filesystem::path project_root) : project_root_(std::move(project_root)) {}

std::string SafeWriter::sha256(std::string_view content) {
    const auto digest = hash_bytes(content); std::ostringstream output;
    for (const auto byte : digest) output << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
    return output.str();
}

Result<WriteResult> SafeWriter::write(const GeneratedFileSet& files, const WriteOptions& options) const {
    std::error_code ec;
    if (!std::filesystem::is_directory(project_root_, ec) || ec) return std::unexpected{error("project root must be an existing directory", project_root_.generic_string())};

    auto detected_capabilities = detect_filesystem_capabilities(project_root_);
    if (!detected_capabilities) return std::unexpected{detected_capabilities.error()};
    const auto& capabilities = *detected_capabilities;

    const auto manifest_path = project_root_ / manifest_relative_path;
    auto resolved_manifest = resolve_existing_alias(project_root_, manifest_relative_path, capabilities);
    if (!resolved_manifest) return std::unexpected{resolved_manifest.error()};
    if (*resolved_manifest != manifest_path) {
        return std::unexpected{error("manifest has a portable path alias", resolved_manifest->generic_string())};
    }
    auto manifest_parent = reject_symlink_parents(project_root_, manifest_relative_path, capabilities);
    if (!manifest_parent) return std::unexpected{manifest_parent.error()};
    auto old_manifest = load_manifest(manifest_path);
    if (!old_manifest) return std::unexpected{old_manifest.error()};
    auto old_manifest_valid = validate_manifest_equivalence(*old_manifest, capabilities);
    if (!old_manifest_valid) return std::unexpected{old_manifest_valid.error()};

    Manifest next_manifest = options.cleanup_stale ? Manifest{} : *old_manifest;
    std::map<std::string, const GeneratedFile*, std::less<>> next_files;
    for (const auto& file : files.files()) {
        auto normalized = normalize_relative_path(file.path);
        if (!normalized) return std::unexpected{normalized.error()};
        auto unicode_valid = capabilities.validate_path(*normalized);
        if (!unicode_valid) return std::unexpected{unicode_valid.error()};
        auto parents = reject_symlink_parents(project_root_, path_from_utf8(*normalized), capabilities);
        if (!parents) return std::unexpected{parents.error()};
        if (!next_files.emplace(*normalized, &file).second) {
            return std::unexpected{error("generated file set contains duplicate output paths", *normalized)};
        }
    }

    auto output_valid = validate_generated_file_equivalence(next_files, capabilities);
    if (!output_valid) return std::unexpected{output_valid.error()};

    for (const auto& [relative, file] : next_files) {
        auto existing = find_equivalent_manifest_path(next_manifest, relative, capabilities);
        if (!existing) return std::unexpected{existing.error()};
        if (*existing) next_manifest.files.erase(**existing);
        next_manifest.files.insert_or_assign(relative, sha256(file->content));
    }
    auto next_manifest_valid = validate_manifest_equivalence(next_manifest, capabilities);
    if (!next_manifest_valid) return std::unexpected{next_manifest_valid.error()};

    struct Pending { std::filesystem::path destination; std::string content; };
    std::vector<Pending> writes; std::vector<std::filesystem::path> removals;
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

            auto checked = normalize_relative_path(path_from_utf8(relative));
            if (!checked) return std::unexpected{checked.error()};
            auto parent = reject_symlink_parents(project_root_, path_from_utf8(*checked), capabilities);
            if (!parent) return std::unexpected{parent.error()};
            auto resolved_destination = resolve_existing_alias(project_root_, path_from_utf8(relative), capabilities);
            if (!resolved_destination) return std::unexpected{resolved_destination.error()};
            const auto destination = *resolved_destination;
            auto status = std::filesystem::symlink_status(destination, ec);
            if (ec && !is_not_found(ec)) {
                return std::unexpected{error("cannot inspect stale file: " + ec.message(), destination.generic_string())};
            }
            if (is_not_found(ec)) {
                ec.clear();
                status = std::filesystem::file_status{};
            }
            if (std::filesystem::is_symlink(status) ||
                (std::filesystem::exists(status) && !std::filesystem::is_regular_file(status))) {
                return std::unexpected{error("owned stale path is not a regular file", relative)};
            }
            if (std::filesystem::exists(status)) {
                auto current = read_file(destination);
                if (!current) return std::unexpected{current.error()};
                if (sha256(*current) != hash && !options.force) {
                    return std::unexpected{error("refusing to remove a generated file modified since its recorded hash; use force to remove it", relative)};
                }
                removals.push_back(destination);
            }
        }
    }
    for (const auto& [relative,file] : next_files) {
        auto resolved_destination = resolve_existing_alias(project_root_, path_from_utf8(relative), capabilities);
        if (!resolved_destination) return std::unexpected{resolved_destination.error()};
        const auto destination = *resolved_destination;
        auto status = std::filesystem::symlink_status(destination, ec);
        // A stale owned file may be an ancestor of this destination. It is
        // scheduled for removal before writes, so treat ENOTDIR as absent here;
        // reject_file_ancestor below still rejects every unscheduled file.
        if (ec == std::errc::not_a_directory) ec.clear();
        if (ec && !is_not_found(ec)) {
            return std::unexpected{error("cannot inspect destination: " + ec.message(), destination.generic_string())};
        }
        if (is_not_found(ec)) {
            ec.clear();
            status = std::filesystem::file_status{};
        }
        if (std::filesystem::is_symlink(status) ||
            (std::filesystem::exists(status) && !std::filesystem::is_regular_file(status))) {
            return std::unexpected{error("destination must be a regular file", destination.generic_string())};
        }

        auto old_path = find_equivalent_manifest_path(*old_manifest, relative, capabilities);
        if (!old_path) return std::unexpected{old_path.error()};
        const bool owned = static_cast<bool>(*old_path);
        const auto old_hash = owned ? old_manifest->files.at(**old_path) : std::string{};
        if (std::filesystem::exists(status) && !owned && !options.force) {
            return std::unexpected{error("refusing to replace an unowned file; use force to take ownership", relative)};
        }
        if (std::filesystem::exists(status) && owned) {
            auto current = read_file(destination);
            if (!current) return std::unexpected{current.error()};
            if (sha256(*current) != old_hash && !options.force) {
                return std::unexpected{error("refusing to replace a generated file modified since its recorded hash; use force to take ownership", relative)};
            }
        }
        if (!std::filesystem::exists(status) || !owned) {
            writes.push_back({destination, file->content});
        } else {
            auto current = read_file(destination);
            if (!current) return std::unexpected{current.error()};
            if (*current != file->content) writes.push_back({destination, file->content});
        }
    }
    for (const auto& [relative, file] : next_files) {
        (void)file;
        auto ancestors = reject_file_ancestor(project_root_, path_from_utf8(relative), capabilities, removals);
        if (!ancestors) return std::unexpected{ancestors.error()};
    }
    const auto rendered_manifest = render_manifest(next_manifest);
    bool manifest_changed = true;
    const auto manifest_exists = std::filesystem::exists(manifest_path, ec);
    if (ec) return std::unexpected{error("cannot inspect manifest: " + ec.message(), manifest_path.generic_string())};
    if (manifest_exists) {
        auto existing = read_file(manifest_path);
        if (!existing) return std::unexpected{existing.error()};
        manifest_changed = *existing != rendered_manifest;
    }
    WriteResult result{writes.size(), removals.size(),
                       !writes.empty() || !removals.empty() || manifest_changed};
    if (options.check || options.dry_run || !result.would_change) return result;
    std::vector<bool> prerequisite_removals(removals.size());
    for (const auto& pending : writes) {
        for (std::size_t index = 0; index < removals.size(); ++index) {
            for (auto ancestor = pending.destination.parent_path(); ancestor != project_root_ && !ancestor.empty(); ancestor = ancestor.parent_path()) {
                if (ancestor == removals[index]) {
                    prerequisite_removals[index] = true;
                    break;
                }
            }
        }
    }
    for (std::size_t index = 0; index < removals.size(); ++index) if (prerequisite_removals[index]) {
        std::filesystem::remove(removals[index], ec);
        if (ec) return std::unexpected{error("cannot remove stale generated file: " + ec.message(), removals[index].generic_string())};
    }
    for(const auto& pending:writes) { std::filesystem::create_directories(pending.destination.parent_path(),ec); if(ec) return std::unexpected{error("cannot create output directory: "+ec.message(),pending.destination.parent_path().generic_string())}; }
    std::vector<std::pair<std::filesystem::path,std::filesystem::path>> temporary; temporary.reserve(writes.size()+1);
    std::optional<std::filesystem::path> manifest_temp;
    auto cleanup=[&](){ for(const auto& [temp,dest]:temporary){std::error_code ignored;std::filesystem::remove(temp,ignored);} if(manifest_temp){std::error_code ignored;std::filesystem::remove(*manifest_temp,ignored);} };
    for(const auto& pending:writes){auto temp=write_temporary(pending.destination,pending.content);if(!temp){cleanup();return std::unexpected{temp.error()};}temporary.emplace_back(*temp,pending.destination);}
    std::filesystem::create_directories(manifest_path.parent_path(),ec); if(ec){cleanup();return std::unexpected{error("cannot create manifest directory: "+ec.message(),manifest_path.parent_path().generic_string())};}
    auto pending_manifest_temp=write_temporary(manifest_path,rendered_manifest);if(!pending_manifest_temp){cleanup();return std::unexpected{pending_manifest_temp.error()};} manifest_temp=*pending_manifest_temp;
    for(std::size_t index = 0; index < removals.size(); ++index) if (!prerequisite_removals[index]) { const auto& stale = removals[index]; std::filesystem::remove(stale,ec);if(ec){cleanup();return std::unexpected{error("cannot remove stale generated file: "+ec.message(),stale.generic_string())};}}
    for(const auto& [temp,destination]:temporary){auto replaced=replace_file(temp,destination);if(!replaced){cleanup();return std::unexpected{replaced.error()};}}
    auto replaced=replace_file(*manifest_temp,manifest_path);if(!replaced){cleanup();return std::unexpected{replaced.error()};}
    return result;
}

Result<WriteResult> SafeWriter::clean(bool dry_run, bool force) const {
    std::error_code ec;
    if (!std::filesystem::is_directory(project_root_, ec) || ec) {
        return std::unexpected{error("project root must be an existing directory", project_root_.generic_string())};
    }

    auto detected_capabilities = detect_filesystem_capabilities(project_root_);
    if (!detected_capabilities) return std::unexpected{detected_capabilities.error()};
    const auto& capabilities = *detected_capabilities;

    const auto manifest_path = project_root_ / manifest_relative_path;
    auto resolved_manifest = resolve_existing_alias(project_root_, manifest_relative_path, capabilities);
    if (!resolved_manifest) return std::unexpected{resolved_manifest.error()};
    if (*resolved_manifest != manifest_path) {
        return std::unexpected{error("manifest has a portable path alias", resolved_manifest->generic_string())};
    }
    auto manifest_parent = reject_symlink_parents(project_root_, manifest_relative_path, capabilities);
    if (!manifest_parent) return std::unexpected{manifest_parent.error()};
    auto manifest = load_manifest(manifest_path);
    if (!manifest) return std::unexpected{manifest.error()};
    auto manifest_valid = validate_manifest_equivalence(*manifest, capabilities);
    if (!manifest_valid) return std::unexpected{manifest_valid.error()};

    WriteResult result{};
    for (const auto& [relative, hash] : manifest->files) {
        auto valid = normalize_relative_path(path_from_utf8(relative));
        if (!valid) return std::unexpected{valid.error()};
        auto unicode_valid = capabilities.validate_path(*valid);
        if (!unicode_valid) return std::unexpected{unicode_valid.error()};
        auto parents = reject_symlink_parents(project_root_, path_from_utf8(*valid), capabilities);
        if (!parents) return std::unexpected{parents.error()};
        auto resolved_path = resolve_existing_alias(project_root_, path_from_utf8(relative), capabilities);
        if (!resolved_path) return std::unexpected{resolved_path.error()};
        const auto path = *resolved_path;
        auto status = std::filesystem::symlink_status(path, ec);
        if (ec && !is_not_found(ec)) {
            return std::unexpected{error("cannot inspect owned file: " + ec.message(), path.generic_string())};
        }
        if (is_not_found(ec)) {
            ec.clear();
            status = std::filesystem::file_status{};
        }
        if (std::filesystem::is_symlink(status) ||
            (std::filesystem::exists(status) && !std::filesystem::is_regular_file(status))) {
            return std::unexpected{error("owned path is not a regular file", relative)};
        }
        if (std::filesystem::exists(status)) {
            auto current = read_file(path);
            if (!current) return std::unexpected{current.error()};
            if (sha256(*current) != hash && !force) {
                return std::unexpected{error("refusing to remove a generated file modified since its recorded hash; use force to remove it", relative)};
            }
            ++result.files_removed;
            result.would_change = true;
        }
    }

    const auto manifest_exists = std::filesystem::exists(manifest_path, ec);
    if (ec) return std::unexpected{error("cannot inspect manifest: " + ec.message(), manifest_path.generic_string())};
    if (manifest_exists) result.would_change = true;
    if (dry_run || !result.would_change) return result;

    for (const auto& [relative, hash] : manifest->files) {
        (void)hash;
        auto resolved_path = resolve_existing_alias(project_root_, path_from_utf8(relative), capabilities);
        if (!resolved_path) return std::unexpected{resolved_path.error()};
        std::filesystem::remove(*resolved_path, ec);
        if (ec) return std::unexpected{error("cannot remove owned file: " + ec.message(), relative)};
    }
    std::filesystem::remove(manifest_path, ec);
    if (ec) return std::unexpected{error("cannot remove manifest: " + ec.message(), manifest_path.generic_string())};
    std::filesystem::remove(manifest_path.parent_path(), ec);
    return result;
}

} // namespace cmakegen
