#include "cmakegen/fs/safe_writer.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace cmakegen {
namespace {

using Hash = std::array<std::uint8_t, 32>;
constexpr std::string_view manifest_relative_path = ".cmakegen/manifest.json";

struct Manifest {
    std::map<std::string, std::string, std::less<>> files;
};

// Generated trees must be portable to the default case-insensitive file
// systems used by macOS and Windows.  Keep this deliberately ASCII-only: the
// generator emits portable source paths, and locale-dependent folding would
// make manifests non-deterministic.
[[nodiscard]] std::string case_insensitive_key(std::string_view path) {
    std::string result{path};
    std::ranges::transform(result, result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

// Ordinary Win32 paths disregard a component's trailing dots and spaces.  Use
// that identity everywhere we reason about portable output ownership, even on
// hosts whose native filesystem permits those names.
[[nodiscard]] std::string filesystem_equivalence_key(std::string_view path) {
    std::string result;
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto end = path.find('/', start);
        auto component = path.substr(start, end == std::string_view::npos ? path.size() - start : end - start);
        while (!component.empty() && (component.back() == '.' || component.back() == ' ')) component.remove_suffix(1);
        if (!result.empty()) result += '/';
        result += case_insensitive_key(component);
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return result;
}

[[nodiscard]] auto find_equivalent_manifest_entry(const Manifest& manifest, std::string_view relative) {
    const auto key = filesystem_equivalence_key(relative);
    return std::ranges::find_if(manifest.files, [&key](const auto& entry) {
        return filesystem_equivalence_key(entry.first) == key;
    });
}

[[nodiscard]] DiagnosticSet error(std::string message, std::string path = {}) {
    DiagnosticSet diagnostics;
    diagnostics.add({std::string{diagnostic_codes::filesystem_error}, DiagnosticSeverity::error,
                     std::move(message), std::move(path), std::nullopt});
    return diagnostics;
}

[[nodiscard]] Result<std::string> normalize_relative_path(const std::filesystem::path& path) {
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
        if (component == "..") {
            return std::unexpected{error("generated path cannot escape the project root", path.generic_string())};
        }
    }
    const auto result = normalized.generic_string();
    const auto insensitive = filesystem_equivalence_key(result);
    if (insensitive == ".cmakegen" || insensitive.starts_with(".cmakegen/")) {
        return std::unexpected{error("generated path is reserved for cmakegen metadata", result)};
    }
    return result;
}

[[nodiscard]] Result<void> reject_file_ancestor(const std::filesystem::path& root,
                                                 const std::filesystem::path& relative) {
    // Resolve each existing ancestor using a case-folded name.  This keeps a
    // generated tree safe when it is produced on a case-sensitive host and
    // then used on the default filesystems of macOS or Windows.
    auto existing_component = [](const std::filesystem::path& parent,
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
            if (filesystem_equivalence_key(entry.path().filename().generic_string()) ==
                filesystem_equivalence_key(component.generic_string())) {
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
        if (std::filesystem::is_regular_file(status)) {
            return std::unexpected{error("generated path collides with an existing file ancestor", current.generic_string())};
        }
    }
    return {};
}

[[nodiscard]] Result<void> reject_symlink_parents(const std::filesystem::path& root,
                                                   const std::filesystem::path& relative) {
    std::error_code ec;
    auto current = root;
    for (const auto& component : relative.parent_path()) {
        const auto exact = current / component;
        const auto exact_status = std::filesystem::symlink_status(exact, ec);
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
                if (filesystem_equivalence_key(entry.path().filename().generic_string()) ==
                    filesystem_equivalence_key(component.generic_string())) {
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
                                                                    const std::filesystem::path& relative) {
    std::error_code ec;
    auto current = root;
    for (const auto& component : relative) {
        const auto exact = current / component;
        const auto exact_status = std::filesystem::symlink_status(exact, ec);
        if (!ec && exact_status.type() != std::filesystem::file_type::not_found) {
            current = exact;
            continue;
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
        if (ec) return std::unexpected{error("cannot inspect destination: " + ec.message(), current.generic_string())};
        std::optional<std::filesystem::path> matched;
        for (const auto& entry : entries) {
            if (filesystem_equivalence_key(entry.path().filename().generic_string()) ==
                filesystem_equivalence_key(component.generic_string())) {
                if (matched) {
                    return std::unexpected{error("destination has ambiguous portable path aliases", current.generic_string())};
                }
                matched = entry.path();
            }
        }
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
        std::set<std::string, std::less<>> insensitive_paths;
        for (const auto& entry : json["files"]) {
            if (!entry.is_object() || !entry.contains("path") || !entry.contains("sha256") ||
                !entry["path"].is_string() || !entry["sha256"].is_string()) {
                return std::unexpected{error("manifest contains an invalid file entry", path.generic_string())};
            }
            auto normalized = normalize_relative_path(entry["path"].get<std::string>());
            if (!normalized || *normalized != entry["path"].get<std::string>() ||
                !insensitive_paths.emplace(filesystem_equivalence_key(*normalized)).second ||
                !manifest.files.emplace(*normalized, entry["sha256"].get<std::string>()).second) {
                return std::unexpected{error("manifest contains duplicate or unsafe paths", path.generic_string())};
            }
        }
        return manifest;
    } catch (const nlohmann::json::exception&) {
        return std::unexpected{error("manifest is not valid JSON", path.generic_string())};
    }
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

} // namespace

Result<void> GeneratedFileSet::add(GeneratedFile file) {
    auto normalized = normalize_relative_path(file.path);
    if (!normalized) return std::unexpected{normalized.error()};
    const auto key = filesystem_equivalence_key(*normalized);
    for (const auto& existing : files_) {
        const auto existing_path = existing.path.generic_string();
        if (filesystem_equivalence_key(existing_path) == key) {
            return std::unexpected{error("generated file set contains duplicate paths on a case-insensitive filesystem", *normalized)};
        }
        const auto insensitive_existing = filesystem_equivalence_key(existing_path);
        const auto separator = insensitive_existing + "/";
        const auto new_separator = key + "/";
        if (key.starts_with(separator) || insensitive_existing.starts_with(new_separator)) {
            return std::unexpected{error("generated file set contains file/ancestor path collisions", *normalized)};
        }
    }
    file.path = std::filesystem::path{*normalized};
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
    const auto manifest_path = project_root_ / manifest_relative_path;
    auto manifest_parent = reject_symlink_parents(project_root_, manifest_relative_path);
    if (!manifest_parent) return std::unexpected{manifest_parent.error()};
    auto old_manifest = load_manifest(manifest_path); if (!old_manifest) return std::unexpected{old_manifest.error()};
    Manifest next_manifest = options.cleanup_stale ? Manifest{} : *old_manifest;
    std::map<std::string, const GeneratedFile*, std::less<>> next_files;
    std::set<std::string, std::less<>> next_file_keys;
    for (const auto& file : files.files()) {
        auto normalized=normalize_relative_path(file.path); if(!normalized) return std::unexpected{normalized.error()};
        auto parents=reject_symlink_parents(project_root_, *normalized); if(!parents) return std::unexpected{parents.error()};
        auto ancestors=reject_file_ancestor(project_root_, *normalized); if(!ancestors) return std::unexpected{ancestors.error()};
        next_files.emplace(*normalized,&file);
        next_file_keys.emplace(filesystem_equivalence_key(*normalized));
        if (const auto existing = find_equivalent_manifest_entry(next_manifest, *normalized);
            existing != next_manifest.files.end()) {
            next_manifest.files.erase(existing);
        }
        next_manifest.files.insert_or_assign(*normalized, sha256(file.content));
    }
    struct Pending { std::filesystem::path destination; std::string content; };
    std::vector<Pending> writes; std::vector<std::filesystem::path> removals;
    for (const auto& [relative,file] : next_files) {
        auto resolved_destination=resolve_existing_alias(project_root_, relative); if(!resolved_destination) return std::unexpected{resolved_destination.error()};
        const auto destination=*resolved_destination; auto status=std::filesystem::symlink_status(destination,ec);
        if(ec && !is_not_found(ec)) return std::unexpected{error("cannot inspect destination: "+ec.message(),destination.generic_string())};
        if (is_not_found(ec)) { ec.clear(); status = std::filesystem::file_status{}; }
        if(std::filesystem::is_symlink(status) || (std::filesystem::exists(status) && !std::filesystem::is_regular_file(status))) return std::unexpected{error("destination must be a regular file",destination.generic_string())};
        const auto old = find_equivalent_manifest_entry(*old_manifest, relative);
        const bool owned=old != old_manifest->files.end();
        if(std::filesystem::exists(status) && !owned && !options.force) return std::unexpected{error("refusing to replace an unowned file; use force to take ownership",relative)};
        if (std::filesystem::exists(status) && owned) {
            auto current = read_file(destination);
            if (!current) return std::unexpected{current.error()};
            if (sha256(*current) != old->second && !options.force) {
                return std::unexpected{error("refusing to replace a generated file modified since its recorded hash; use force to take ownership", relative)};
            }
        }
        if(!std::filesystem::exists(status) || !owned) {
            writes.push_back({destination,file->content});
        } else {
            auto current = read_file(destination);
            if (!current) return std::unexpected{current.error()};
            if (*current != file->content) writes.push_back({destination,file->content});
        }
    }
    if(options.cleanup_stale) for(const auto& [relative,hash] : old_manifest->files) if(!next_file_keys.contains(filesystem_equivalence_key(relative))) { auto checked=normalize_relative_path(relative); if(!checked) return std::unexpected{checked.error()}; auto parent=reject_symlink_parents(project_root_,*checked); if(!parent) return std::unexpected{parent.error()}; auto resolved_destination=resolve_existing_alias(project_root_, relative); if(!resolved_destination) return std::unexpected{resolved_destination.error()}; const auto destination=*resolved_destination; auto status=std::filesystem::symlink_status(destination,ec); if(ec && !is_not_found(ec)) return std::unexpected{error("cannot inspect stale file: "+ec.message(),destination.generic_string())}; if (is_not_found(ec)) { ec.clear(); status = std::filesystem::file_status{}; } if(std::filesystem::is_symlink(status) || (std::filesystem::exists(status)&&!std::filesystem::is_regular_file(status))) return std::unexpected{error("owned stale path is not a regular file",relative)}; if(std::filesystem::exists(status)) { auto current=read_file(destination); if(!current)return std::unexpected{current.error()}; if(sha256(*current)!=hash&&!options.force)return std::unexpected{error("refusing to remove a generated file modified since its recorded hash; use force to remove it",relative)}; removals.push_back(destination); } }
    const auto rendered_manifest=render_manifest(next_manifest); bool manifest_changed=true; if(std::filesystem::exists(manifest_path,ec) && !ec) { auto existing=read_file(manifest_path); if(!existing) return std::unexpected{existing.error()}; manifest_changed=*existing!=rendered_manifest; }
    WriteResult result{writes.size(),removals.size(),!writes.empty()||!removals.empty()||manifest_changed};
    if(options.check||options.dry_run||!result.would_change) return result;
    for(const auto& pending:writes) { std::filesystem::create_directories(pending.destination.parent_path(),ec); if(ec) return std::unexpected{error("cannot create output directory: "+ec.message(),pending.destination.parent_path().generic_string())}; }
    std::vector<std::pair<std::filesystem::path,std::filesystem::path>> temporary; temporary.reserve(writes.size()+1);
    std::optional<std::filesystem::path> manifest_temp;
    auto cleanup=[&](){ for(const auto& [temp,dest]:temporary){std::error_code ignored;std::filesystem::remove(temp,ignored);} if(manifest_temp){std::error_code ignored;std::filesystem::remove(*manifest_temp,ignored);} };
    for(const auto& pending:writes){auto temp=write_temporary(pending.destination,pending.content);if(!temp){cleanup();return std::unexpected{temp.error()};}temporary.emplace_back(*temp,pending.destination);}
    std::filesystem::create_directories(manifest_path.parent_path(),ec); if(ec){cleanup();return std::unexpected{error("cannot create manifest directory: "+ec.message(),manifest_path.parent_path().generic_string())};}
    auto pending_manifest_temp=write_temporary(manifest_path,rendered_manifest);if(!pending_manifest_temp){cleanup();return std::unexpected{pending_manifest_temp.error()};} manifest_temp=*pending_manifest_temp;
    for(const auto& stale:removals){std::filesystem::remove(stale,ec);if(ec){cleanup();return std::unexpected{error("cannot remove stale generated file: "+ec.message(),stale.generic_string())};}}
    for(const auto& [temp,destination]:temporary){auto replaced=replace_file(temp,destination);if(!replaced){cleanup();return std::unexpected{replaced.error()};}}
    auto replaced=replace_file(*manifest_temp,manifest_path);if(!replaced){cleanup();return std::unexpected{replaced.error()};}
    return result;
}

Result<WriteResult> SafeWriter::clean(bool dry_run, bool force) const {
    std::error_code ec; if(!std::filesystem::is_directory(project_root_,ec)||ec) return std::unexpected{error("project root must be an existing directory",project_root_.generic_string())};
    const auto manifest_path=project_root_/manifest_relative_path; auto manifest_parent=reject_symlink_parents(project_root_,manifest_relative_path);if(!manifest_parent)return std::unexpected{manifest_parent.error()}; auto manifest=load_manifest(manifest_path);if(!manifest)return std::unexpected{manifest.error()}; WriteResult result{};
    for(const auto& [relative,hash]:manifest->files){auto valid=normalize_relative_path(relative);if(!valid)return std::unexpected{valid.error()};auto parents=reject_symlink_parents(project_root_,*valid);if(!parents)return std::unexpected{parents.error()};auto resolved_path=resolve_existing_alias(project_root_, relative);if(!resolved_path)return std::unexpected{resolved_path.error()};const auto path=*resolved_path;auto status=std::filesystem::symlink_status(path,ec);if(ec && !is_not_found(ec))return std::unexpected{error("cannot inspect owned file: "+ec.message(),path.generic_string())};if (is_not_found(ec)) { ec.clear(); status = std::filesystem::file_status{}; }if(std::filesystem::is_symlink(status)||(std::filesystem::exists(status)&&!std::filesystem::is_regular_file(status)))return std::unexpected{error("owned path is not a regular file",relative)};if(std::filesystem::exists(status)){auto current=read_file(path);if(!current)return std::unexpected{current.error()};if(sha256(*current)!=hash&&!force)return std::unexpected{error("refusing to remove a generated file modified since its recorded hash; use force to remove it",relative)};++result.files_removed;result.would_change=true;}}
    if(std::filesystem::exists(manifest_path,ec)&&!ec)result.would_change=true; if(dry_run||!result.would_change)return result;
    for(const auto& [relative,hash]:manifest->files){auto resolved_path=resolve_existing_alias(project_root_, relative);if(!resolved_path)return std::unexpected{resolved_path.error()};std::filesystem::remove(*resolved_path,ec);if(ec)return std::unexpected{error("cannot remove owned file: "+ec.message(),relative)};}
    std::filesystem::remove(manifest_path,ec);if(ec)return std::unexpected{error("cannot remove manifest: "+ec.message(),manifest_path.generic_string())}; std::filesystem::remove(manifest_path.parent_path(),ec); return result;
}

} // namespace cmakegen
