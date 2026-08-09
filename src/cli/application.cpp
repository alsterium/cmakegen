#include "cmakegen/cli/application.hpp"

#include "cmakegen/diagnostics/diagnostic.hpp"
#include "cmakegen/domain/dump.hpp"
#include "cmakegen/domain/normalizer.hpp"
#include "cmakegen/fs/safe_writer.hpp"
#include "cmakegen/generator/presets_renderer.hpp"
#include "cmakegen/parser/parser.hpp"
#include "cmakegen/renderer/cmake_renderer.hpp"
#include "cmakegen/resolver/source_resolver.hpp"
#include "cmakegen/validation/semantic_validator.hpp"
#include "cmakegen/version.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace cmakegen {
namespace {

struct Options {
    std::filesystem::path project = "project.yml";
    std::optional<std::filesystem::path> root;
    bool verbose = false;
    bool quiet = false;
    bool no_color = false;
    bool check = false;
    bool force = false;
    bool resolved = false;
    bool include_external = false;
    bool dry_run = false;
    std::string format = "json";
    bool format_specified = false;
    std::string command;
};

[[nodiscard]] int diagnostic_exit(const DiagnosticSet& diagnostics) {
    return std::ranges::any_of(diagnostics.diagnostics(), [](const Diagnostic& diagnostic) {
        return diagnostic.code == diagnostic_codes::filesystem_error;
    }) ? 2 : 1;
}

void print_diagnostics(const DiagnosticSet& diagnostics, bool color, std::ostream& error) {
    error << format_diagnostics(diagnostics, {.color = color}) << '\n';
}

[[nodiscard]] std::optional<Options> parse_options(const std::vector<std::string>& args,
                                                     std::ostream& error) {
    Options options;
    for (std::size_t index = 0; index < args.size(); ++index) {
        const auto& arg = args[index];
        auto value = [&](std::string_view name) -> std::optional<std::string> {
            if (++index == args.size()) {
                error << "error: " << name << " requires a value\n";
                return std::nullopt;
            }
            return args[index];
        };
        if (arg == "--project") { auto v = value(arg); if (!v) return std::nullopt; options.project = *v; }
        else if (arg == "--root") { auto v = value(arg); if (!v) return std::nullopt; options.root = *v; }
        else if (arg == "--verbose") options.verbose = true;
        else if (arg == "--quiet") options.quiet = true;
        else if (arg == "--no-color") options.no_color = true;
        else if (arg == "--check") options.check = true;
        else if (arg == "--force") options.force = true;
        else if (arg == "--resolved") options.resolved = true;
        else if (arg == "--include-external") options.include_external = true;
        else if (arg == "--dry-run") options.dry_run = true;
        else if (arg == "--format") { auto v = value(arg); if (!v) return std::nullopt; options.format = *v; options.format_specified = true; }
        else if (arg == "--version") { options.command = "version"; }
        else if (arg == "--help" || arg == "-h") { options.command = "help"; }
        else if (arg.starts_with('-')) { error << "error: unknown option " << arg << '\n'; return std::nullopt; }
        else if (options.command.empty()) options.command = arg;
        else { error << "error: unexpected argument " << arg << '\n'; return std::nullopt; }
    }
    if (options.verbose && options.quiet) { error << "error: --verbose and --quiet cannot be combined\n"; return std::nullopt; }
    return options;
}

[[nodiscard]] bool validate_command_options(const Options& options, std::ostream& error) {
    const auto require = [&](bool supplied, std::string_view option, std::string_view command) {
        if (!supplied || options.command == command) return true;
        error << "error: " << option << " is only valid with " << command << "\n";
        return false;
    };
    if (!require(options.check, "--check", "generate")) return false;
    if (!require(options.force, "--force", "generate")) return false;
    if (!require(options.resolved, "--resolved", "validate")) return false;
    if (!require(options.include_external, "--include-external", "graph")) return false;
    if (!require(options.dry_run, "--dry-run", "clean")) return false;
    if (options.format_specified && options.command != "dump" && options.command != "graph") {
        error << "error: --format is only valid with dump or graph\n";
        return false;
    }
    return true;
}

void print_help(std::ostream& output) {
    output << "Usage: cmakegen [global options] <command> [command options]\n\n"
              "Commands: generate, validate, dump, graph, clean\n\n"
              "Global options:\n"
              "  --project <path>  Project spec path (default: project.yml)\n"
              "  --root <path>     Project root (default: spec parent directory)\n"
              "  --verbose  --quiet  --no-color  --version\n";
}

[[nodiscard]] Result<Project> load_project(const std::filesystem::path& spec) {
    std::ifstream input(spec, std::ios::binary);
    if (!input) {
        DiagnosticSet diagnostics;
        diagnostics.add({std::string{diagnostic_codes::filesystem_error}, DiagnosticSeverity::error,
                         "cannot read project specification", spec.generic_string(), std::nullopt});
        return std::unexpected{std::move(diagnostics)};
    }
    const std::string source{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    auto raw = parse_raw_spec(source);
    if (!raw) return std::unexpected{raw.error()};
    return normalize_project(*raw);
}

struct ResolvedProject {
    RenderedSourcesByTarget rendered_sources;
};

[[nodiscard]] std::string rebase_project_path(const std::string& value,
                                              const std::filesystem::path& source_root,
                                              const std::filesystem::path& output_root) {
    const auto declared = std::filesystem::path{value};
    // std::filesystem follows the host's path grammar.  The project model is
    // cross-platform, so preserve Windows drive and UNC roots even while a
    // POSIX host generates the Windows-specific portions of a project.
    const bool foreign_windows_absolute =
        (value.size() >= 3 && std::isalpha(static_cast<unsigned char>(value[0])) &&
         value[1] == ':' && (value[2] == '/' || value[2] == '\\')) ||
        value.starts_with("//") || value.starts_with("\\\\");
    // A root-directory-only POSIX path is not considered absolute by the
    // Windows std::filesystem implementation, but remains absolute in the
    // cross-platform project model.
    if (foreign_windows_absolute || value.starts_with('/')) return value;
    std::error_code error;
    const auto canonical_base = [&](const std::filesystem::path& base) {
        const auto canonical = std::filesystem::weakly_canonical(base, error);
        if (error) {
            error.clear();
            return base.lexically_normal();
        }
        return canonical;
    };
    // Canonicalize only the directory bases. The final declared component is
    // deliberately appended lexically so source-file symlinks retain their
    // user-visible spelling and CMake language suffix.
    const auto canonical_source_root = canonical_base(source_root);
    const auto canonical_output_root = canonical_base(output_root);
    const auto absolute = (declared.is_absolute() ? declared : canonical_source_root / declared).lexically_normal();
    // Unlike std::filesystem::relative(), lexical rebasing never resolves
    // source symlinks.  CMake must see the declared spelling so suffix-based
    // language detection remains consistent with the project specification.
    const auto relative = absolute.lexically_relative(canonical_output_root);
    return relative.empty() ? absolute.generic_string() : relative.generic_string();
}

[[nodiscard]] Result<ResolvedProject> resolve_project_sources(const Project& project,
                                                                const std::filesystem::path& source_root,
                                                                const std::filesystem::path& output_root) {
    ResolvedProject result;
    auto resolve = [&](const std::vector<SourceEntry>& entries,
                       std::vector<ResolvedSource>& destination) -> Result<void> {
        auto sources = resolve_sources(source_root, entries);
        if (!sources) return std::unexpected{sources.error()};
        destination = std::move(*sources);
        for (auto& source : destination) {
            source.path = rebase_project_path(source.path, source_root, output_root);
        }
        return {};
    };
    for (const auto& [name, target] : project.targets) {
        auto& rendered = result.rendered_sources[name];
        if (auto r = resolve(target.sources, rendered.common); !r) return std::unexpected{r.error()};
        if (auto r = resolve(target.platform.macos.sources, rendered.macos); !r) return std::unexpected{r.error()};
        if (auto r = resolve(target.platform.linux.sources, rendered.linux); !r) return std::unexpected{r.error()};
        if (auto r = resolve(target.platform.windows.sources, rendered.windows); !r) return std::unexpected{r.error()};
        for (auto* sources : {&rendered.common, &rendered.macos, &rendered.linux, &rendered.windows})
            std::ranges::sort(*sources, {}, &ResolvedSource::path);
    }
    return result;
}

void rebase_scoped_paths(ScopedValues<std::string>& values, const std::filesystem::path& source_root,
                         const std::filesystem::path& output_root) {
    for (auto* scope : {&values.private_values, &values.public_values, &values.interface_values})
        for (auto& value : *scope) value = rebase_project_path(value, source_root, output_root);
}

void rebase_render_paths(Project& project, const std::filesystem::path& source_root,
                         const std::filesystem::path& output_root) {
    for (auto& include : project.cmake.include_files)
        include = rebase_project_path(include, source_root, output_root);
    for (auto& [unused, target] : project.targets) {
        rebase_scoped_paths(target.include_directories, source_root, output_root);
        for (auto* platform : {&target.platform.macos, &target.platform.linux, &target.platform.windows})
            rebase_scoped_paths(platform->include_directories, source_root, output_root);
    }
    for (auto& [unused, preset] : project.presets) {
        // Only sourceDir establishes a known absolute base. Other preset
        // macros can expand to relative text and must retain spec-relative
        // rebasing when --root differs from the specification directory.
        const bool macro_root = preset.binary_dir.starts_with("${sourceDir}");
        if (!macro_root)
            preset.binary_dir = rebase_project_path(preset.binary_dir, source_root, output_root);
    }
}

void print_graph_text(const TargetGraph& graph, bool include_external, std::ostream& output) {
    std::set<std::string> children;
    for (const auto& [target, dependencies] : graph.internal_adjacency)
        children.insert(dependencies.begin(), dependencies.end());
    std::set<std::string> visited;
    std::function<void(const std::string&, const std::string&, bool, std::set<std::string>&)> visit;
    visit = [&](const std::string& target, const std::string& prefix, bool last, std::set<std::string>& ancestors) {
        output << prefix << (last ? "└── " : "├── ") << target << '\n';
        if (!ancestors.insert(target).second) return;
        if (!visited.insert(target).second) {
            ancestors.erase(target);
            return;
        }
        std::vector<std::string> dependencies = graph.internal_adjacency.at(target);
        if (include_external) for (const auto& dependency : graph.dependencies.at(target))
            if (dependency.kind == LinkDependency::Kind::imported_target) dependencies.push_back(dependency.name);
        std::ranges::sort(dependencies);
        for (std::size_t i = 0; i < dependencies.size(); ++i) {
            const bool child_last = i + 1 == dependencies.size();
            const auto child_prefix = prefix + (last ? "    " : "│   ");
            if (graph.internal_adjacency.contains(dependencies[i]))
                visit(dependencies[i], child_prefix, child_last, ancestors);
            else output << child_prefix << (child_last ? "└── " : "├── ") << dependencies[i] << '\n';
        }
        ancestors.erase(target);
    };
    std::vector<std::string> roots;
    for (const auto& [target, unused] : graph.internal_adjacency) if (!children.contains(target)) roots.push_back(target);
    std::ranges::sort(roots);
    for (std::size_t index = 0; index < roots.size(); ++index) {
        std::set<std::string> ancestors; visit(roots[index], {}, index + 1 == roots.size(), ancestors);
    }
    // The displayed graph is the union of common and platform-specific edges.
    // A union-only cycle has no root even when every effective platform graph is
    // acyclic.  Print one traversal for each component missed by the root pass.
    std::vector<std::string> remaining;
    for (const auto& [target, unused] : graph.internal_adjacency)
        if (!visited.contains(target)) remaining.push_back(target);
    std::ranges::sort(remaining);
    for (const auto& target : remaining) {
        if (visited.contains(target)) continue;
        std::set<std::string> ancestors; visit(target, {}, true, ancestors);
    }
}

[[nodiscard]] std::string dot_identifier(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size() + 2);
    for (const char character : value) {
        switch (character) {
        case '\\': escaped += "\\\\"; break;
        case '\"': escaped += "\\\""; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default: escaped += character; break;
        }
    }
    return '\"' + escaped + '\"';
}

void print_graph_dot(const TargetGraph& graph, bool include_external, std::ostream& output) {
    output << "digraph cmakegen {\n";
    for (const auto& [target, dependencies] : graph.dependencies) {
        output << "  " << dot_identifier(target) << ";\n";
        for (const auto& dependency : dependencies) if (include_external || dependency.kind == LinkDependency::Kind::project_target)
            output << "  " << dot_identifier(target) << " -> " << dot_identifier(dependency.name) << ";\n";
    }
    output << "}\n";
}

} // namespace

int run_cli(const std::vector<std::string>& arguments, std::ostream& output, std::ostream& error) {
    try {
        auto parsed = parse_options(arguments, error);
        if (!parsed) return 1;
        auto& options = *parsed;
        if (!validate_command_options(options, error)) return 1;
        if (options.command == "help" || options.command.empty()) { print_help(output); return options.command.empty() ? 1 : 0; }
        if (options.command == "version") { output << "cmakegen " << version() << '\n'; return 0; }
        if (options.command != "generate" && options.command != "validate" && options.command != "dump" && options.command != "graph" && options.command != "clean") {
            error << "error: unknown command " << options.command << '\n'; return 1;
        }
        const auto spec = std::filesystem::absolute(options.project);
        const auto root = options.root ? std::filesystem::absolute(*options.root) : spec.parent_path();
        if (options.verbose) error << "verbose: command=" << options.command << " project=" << spec.generic_string()
                                   << " root=" << root.generic_string() << '\n';
        if (options.command == "clean") {
            auto cleaned = SafeWriter(root).clean(options.dry_run);
            if (!cleaned) { print_diagnostics(cleaned.error(), !options.no_color, error); return diagnostic_exit(cleaned.error()); }
            if (!options.quiet) output << (options.dry_run ? "Would remove " : "Removed ") << cleaned->files_removed << " files.\n";
            return 0;
        }
        auto project = load_project(spec);
        if (!project) { print_diagnostics(project.error(), !options.no_color, error); return diagnostic_exit(project.error()); }
        const bool need_sources = options.command == "generate" || options.resolved;
        ResolvedProject resolved;
        if (need_sources) {
            // Sources are part of the specification and therefore are always
            // resolved relative to that specification.  --root only controls
            // where generated output and its ownership manifest are written.
            auto result = resolve_project_sources(*project, spec.parent_path(), root);
            if (!result) { print_diagnostics(result.error(), !options.no_color, error); return diagnostic_exit(result.error()); }
            resolved = std::move(*result);
            if (options.verbose) {
                std::size_t source_count = 0;
                for (const auto& [target, sources] : resolved.rendered_sources)
                    source_count += sources.common.size() + sources.macos.size() + sources.linux.size() + sources.windows.size();
                error << "verbose: resolved " << source_count << " source files\n";
            }
        }
        auto graph = validate_project(*project, need_sources ? resolved.rendered_sources : ResolvedSourcesByTarget{});
        if (!graph) { print_diagnostics(graph.error(), !options.no_color, error); return diagnostic_exit(graph.error()); }
        if (options.verbose) error << "verbose: validated " << project->name << " (" << project->targets.size() << " targets)\n";
        if (options.command == "validate") { if (!options.quiet) output << "Valid.\n"; return 0; }
        if (options.command == "dump") {
            if (options.format == "json") output << dump_project_json(*project);
            else if (options.format == "yaml") output << dump_project_yaml(*project);
            else { error << "error: dump format must be json or yaml\n"; return 1; }
            return 0;
        }
        if (options.command == "graph") {
            if (!options.format_specified || options.format == "text") print_graph_text(*graph, options.include_external, output);
            else if (options.format == "dot") print_graph_dot(*graph, options.include_external, output);
            else { error << "error: graph format must be text or dot\n"; return 1; }
            return 0;
        }
        auto render_project = *project;
        rebase_render_paths(render_project, spec.parent_path(), root);
        const auto cmake = render_cmake(render_project, *graph, resolved.rendered_sources);
        GeneratedFileSet files;
        (void)files.add({"CMakeLists.txt", cmake.root_cmake_lists});
        (void)files.add({"cmake/cmakegen.generated.cmake", cmake.generated_cmake});
        (void)files.add({"CMakePresets.json", render_presets(render_project)});
        // Check mode is observational: an ownership conflict is a generation
        // mismatch, not a filesystem write failure. SafeWriter still performs
        // no replacement when check is set.
        auto written = SafeWriter(root).write(files, {.check = options.check,
                                                       .force = options.force || options.check});
        if (!written) { print_diagnostics(written.error(), !options.no_color, error); return diagnostic_exit(written.error()); }
        if (options.check && written->would_change) return 1;
        if (!options.quiet) output << (written->would_change ? "Generated " + std::to_string(written->files_written) + " files.\n" : "No changes.\n");
        return 0;
    } catch (const std::exception& exception) {
        error << "error: internal generation error: " << exception.what() << '\n';
        return 3;
    }
}

} // namespace cmakegen
