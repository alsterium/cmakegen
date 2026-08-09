#include "cmakegen/validation/semantic_validator.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace cmakegen;

namespace {

[[nodiscard]] Target target(std::string name, TargetType type = TargetType::static_library) {
    Target result;
    result.name = std::move(name);
    result.type = type;
    return result;
}

void add_dependency(Target& target, std::string name,
                    LinkDependency::Kind kind = LinkDependency::Kind::imported_target) {
    target.dependencies.private_values.push_back({kind, std::move(name)});
}

[[nodiscard]] Project project_with_targets(std::initializer_list<Target> targets) {
    Project project;
    project.schema_version = 1;
    project.name = "example";
    for (auto& target : targets) project.targets.emplace(target.name, target);
    return project;
}

} // namespace

TEST_CASE("semantic validation builds a deterministic graph and retains imported targets") {
    auto app = target("App", TargetType::executable);
    add_dependency(app, "fmt::fmt");
    add_dependency(app, "Core");
    // Semantic validation must classify before deduplicating: a manually
    // constructed value can carry a stale kind while naming a project target.
    app.dependencies.public_values.push_back({LinkDependency::Kind::project_target, "Core"});
    app.platform.linux.dependencies.interface_values.push_back(
        {LinkDependency::Kind::imported_target, "fmt::fmt"});
    auto core = target("Core");
    add_dependency(core, "Threads::Threads");

    const auto result = validate_project(project_with_targets({app, core}));
    REQUIRE(result);
    CHECK(result->internal_adjacency ==
          std::map<std::string, std::vector<std::string>>{{"App", {"Core"}}, {"Core", {}}});
    REQUIRE(result->dependencies.at("App").size() == 2);
    CHECK(result->dependencies.at("App")[0].name == "Core");
    CHECK(result->dependencies.at("App")[0].kind == LinkDependency::Kind::project_target);
    CHECK(result->dependencies.at("App")[1].name == "fmt::fmt");
    CHECK(result->dependencies.at("App")[1].kind == LinkDependency::Kind::imported_target);
}

TEST_CASE("semantic validation includes platform dependencies in the effective graph") {
    auto app = target("App", TargetType::executable);
    app.platform.linux.dependencies.private_values.push_back(
        {LinkDependency::Kind::imported_target, "Core"});
    auto core = target("Core");
    core.platform.windows.dependencies.public_values.push_back(
        {LinkDependency::Kind::imported_target, "Threads::Threads"});

    const auto result = validate_project(project_with_targets({app, core}));
    REQUIRE(result);
    CHECK(result->internal_adjacency ==
          std::map<std::string, std::vector<std::string>>{{"App", {"Core"}}, {"Core", {}}});
    REQUIRE(result->dependencies.at("Core").size() == 1);
    CHECK(result->dependencies.at("Core").front().name == "Threads::Threads");
}

TEST_CASE("semantic validation does not combine mutually exclusive platform dependencies") {
    auto app = target("App");
    app.platform.macos.dependencies.private_values.push_back(
        {LinkDependency::Kind::imported_target, "Core"});
    auto core = target("Core");
    core.platform.windows.dependencies.private_values.push_back(
        {LinkDependency::Kind::imported_target, "App"});

    const auto result = validate_project(project_with_targets({app, core}));
    REQUIRE(result);
}

TEST_CASE("semantic validation accepts the source directory as a source path") {
    auto app = target("App", TargetType::executable);
    app.sources.push_back({".", true, {}, SourceKind::automatic});

    CHECK(validate_project(project_with_targets({app})));
}

TEST_CASE("semantic validation detects cycles in a common or single platform graph") {
    SECTION("common dependencies") {
        auto app = target("App");
        add_dependency(app, "Core");
        auto core = target("Core");
        add_dependency(core, "App");

        const auto result = validate_project(project_with_targets({app, core}));
        REQUIRE_FALSE(result);
        REQUIRE(result.error().diagnostics().size() == 1);
        CHECK(result.error().diagnostics().front().code == diagnostic_codes::dependency_cycle);
        CHECK(result.error().diagnostics().front().message == "internal dependency cycle: App -> Core -> App");
    }

    SECTION("same platform dependencies") {
        auto app = target("App");
        app.platform.macos.dependencies.private_values.push_back(
            {LinkDependency::Kind::imported_target, "Core"});
        auto core = target("Core");
        core.platform.macos.dependencies.private_values.push_back(
            {LinkDependency::Kind::imported_target, "App"});

        const auto result = validate_project(project_with_targets({app, core}));
        REQUIRE_FALSE(result);
        REQUIRE(result.error().diagnostics().size() == 1);
        CHECK(result.error().diagnostics().front().code == diagnostic_codes::dependency_cycle);
        CHECK(result.error().diagnostics().front().message == "internal dependency cycle: App -> Core -> App");
        CHECK(result.error().diagnostics().front().path == "targets.Core.platform.macos.dependencies");
    }
}

TEST_CASE("semantic validation reports the closing edge provenance in a mixed cycle") {
    auto app = target("App");
    app.platform.macos.dependencies.private_values.push_back(
        {LinkDependency::Kind::imported_target, "Core"});
    auto core = target("Core");
    add_dependency(core, "App");

    const auto result = validate_project(project_with_targets({app, core}));
    REQUIRE_FALSE(result);
    REQUIRE(result.error().diagnostics().size() == 1);
    const auto& diagnostic = result.error().diagnostics().front();
    CHECK(diagnostic.message == "internal dependency cycle: App -> Core -> App");
    CHECK(diagnostic.path == "targets.Core.dependencies");
}

TEST_CASE("semantic validation accepts an acyclic dependency diamond") {
    auto app = target("App");
    add_dependency(app, "Core");
    add_dependency(app, "Platform");
    auto core = target("Core");
    add_dependency(core, "Base");
    auto platform = target("Platform");
    add_dependency(platform, "Base");
    const auto result = validate_project(project_with_targets({app, core, platform, target("Base")}));
    REQUIRE(result);
    CHECK(result->internal_adjacency.at("App") == std::vector<std::string>{"Core", "Platform"});
}

TEST_CASE("semantic validation reports a stable concrete multi-node cycle") {
    auto core = target("Core");
    add_dependency(core, "Platform");
    auto platform = target("Platform");
    add_dependency(platform, "App");
    auto app = target("App");
    add_dependency(app, "Core");

    const auto result = validate_project(project_with_targets({core, platform, app}));
    REQUIRE_FALSE(result);
    REQUIRE(result.error().diagnostics().size() == 1);
    const auto& diagnostic = result.error().diagnostics().front();
    CHECK(diagnostic.code == diagnostic_codes::dependency_cycle);
    CHECK(diagnostic.message == "internal dependency cycle: App -> Core -> Platform -> App");
}

TEST_CASE("interface libraries reject non-interface scopes and implementation sources") {
    auto interface_target = target("Headers", TargetType::interface_library);
    interface_target.include_directories.private_values.push_back("include");
    interface_target.platform.linux.dependencies.public_values.push_back(
        {LinkDependency::Kind::imported_target, "Threads::Threads"});
    interface_target.sources.push_back({"src/implementation.cpp", true, {}, SourceKind::source});

    ResolvedSourcesByTarget resolved{{"Headers", {{"src/automatic.cpp", SourceKind::source}}}};
    const auto result = validate_project(project_with_targets({interface_target}), resolved);
    REQUIRE_FALSE(result);
    const auto& diagnostics = result.error().diagnostics();
    REQUIRE(diagnostics.size() == 4);
    CHECK(diagnostics[0].path == "targets.Headers.includeDirectories.private");
    CHECK(diagnostics[1].path == "targets.Headers.platform.linux.dependencies.public");
    CHECK(diagnostics[2].message ==
          "interface libraries cannot contain implementation sources: src/automatic.cpp");
    CHECK(diagnostics[3].message == "interface libraries cannot contain implementation sources");
}

TEST_CASE("source semantic diagnostics retain declaration provenance") {
    auto interface_target = target("Headers", TargetType::interface_library);
    SourceLocation location{12, 7};
    interface_target.platform.linux.sources.push_back(
        {"src/implementation.cpp", true, {}, SourceKind::source,
         "targets.Headers.platform.linux.sources[0]", location});
    ResolvedSourcesByTarget resolved{{"Headers",
                                      {{"src/discovered.cpp", SourceKind::source,
                                        "targets.Headers.platform.linux.sources[0]", location}}}};

    const auto result = validate_project(project_with_targets({interface_target}), resolved);
    REQUIRE_FALSE(result);
    const auto& diagnostics = result.error().diagnostics();
    REQUIRE(diagnostics.size() == 2);
    for (const auto& diagnostic : diagnostics) {
        CHECK(diagnostic.path == "targets.Headers.platform.linux.sources[0]");
        REQUIRE(diagnostic.location);
        CHECK(*diagnostic.location == location);
    }
}

TEST_CASE("semantic validation diagnoses malformed manually constructed logical definitions and paths") {
    auto target_with_mismatched_name = target("Different");
    target_with_mismatched_name.sources.push_back({"", true, {}, SourceKind::automatic});
    Project project = project_with_targets({target_with_mismatched_name});
    project.name = "invalid/project";
    project.targets.clear();
    project.targets.emplace("not/a-target", target_with_mismatched_name);

    const auto result = validate_project(project);
    REQUIRE_FALSE(result);
    const auto& diagnostics = result.error().diagnostics();
    REQUIRE(diagnostics.size() == 4);
    CHECK(diagnostics[0].path == "name");
    CHECK(diagnostics[1].path == "targets.not/a-target");
    CHECK(diagnostics[2].path == "targets.not/a-target");
    CHECK(diagnostics[3].path == "targets.not/a-target.sources[0]");
}
