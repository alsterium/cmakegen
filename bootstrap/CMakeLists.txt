cmake_minimum_required(VERSION 3.28)

project(cmakegen_bootstrap LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CheckCXXSourceCompiles)
check_cxx_source_compiles(
    "#include <expected>
     int main() { std::expected<int, int> value{1}; return *value - 1; }"
    CMAKEGEN_HAS_STD_EXPECTED)
if(NOT CMAKEGEN_HAS_STD_EXPECTED)
    message(FATAL_ERROR "cmakegen requires C++23 std::expected support")
endif()

# The shared dependency hook honors this mode by avoiding normal-build-only
# test dependencies. Bootstrap must remain able to build a generator alone.
set(CMAKEGEN_BOOTSTRAP_MINIMAL ON)
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/dependencies.cmake")

add_library(cmakegen_core STATIC
    "${CMAKE_CURRENT_LIST_DIR}/../src/cli/application.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../src/diagnostics/diagnostic.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../src/domain/dump.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../src/domain/normalizer.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../src/generator/presets_compatibility.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../src/generator/presets_renderer.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../src/fs/safe_writer.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../src/parser/parser.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../src/renderer/cmake_renderer.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../src/resolver/source_resolver.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../src/validation/semantic_validator.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../src/version.cpp"
)
target_compile_features(cmakegen_core PUBLIC cxx_std_23)
target_include_directories(cmakegen_core PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/../include"
)
target_link_libraries(cmakegen_core PUBLIC
    yaml-cpp::yaml-cpp
    nlohmann_json::nlohmann_json
)
if(APPLE)
    find_library(CMAKEGEN_COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    target_link_libraries(cmakegen_core PUBLIC "${CMAKEGEN_COREFOUNDATION_FRAMEWORK}")
endif()

add_executable(cmakegen "${CMAKE_CURRENT_LIST_DIR}/../src/main.cpp")
target_link_libraries(cmakegen PRIVATE cmakegen_core CLI11::CLI11)
