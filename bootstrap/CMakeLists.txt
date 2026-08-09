cmake_minimum_required(VERSION 3.28)

project(cmakegen_bootstrap LANGUAGES CXX)

include(CTest)
option(CMAKEGEN_BOOTSTRAP_TESTS
    "Enable transitional bootstrap-only test wiring (removed in T014)" ON)

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

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/dependencies.cmake")

add_library(cmakegen_core STATIC
    "${CMAKE_CURRENT_LIST_DIR}/../src/diagnostics/diagnostic.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../src/domain/dump.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../src/domain/normalizer.cpp"
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

add_executable(cmakegen "${CMAKE_CURRENT_LIST_DIR}/../src/main.cpp")
target_link_libraries(cmakegen PRIVATE cmakegen_core CLI11::CLI11)

# Transitional bootstrap-only test wiring. Generated normal CMake replaces this
# in T014, after which bootstrap is limited to a functional generator build.
if(CMAKEGEN_BOOTSTRAP_TESTS AND BUILD_TESTING)
    include(Catch)
    add_executable(cmakegen_tests
        "${CMAKE_CURRENT_LIST_DIR}/../tests/unit/diagnostic_test.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../tests/unit/normalizer_test.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../tests/unit/parser_test.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../tests/unit/cmake_renderer_test.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../tests/unit/source_resolver_test.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../tests/unit/semantic_validator_test.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../tests/unit/version_test.cpp"
    )
    target_link_libraries(cmakegen_tests PRIVATE cmakegen_core Catch2::Catch2WithMain)
    target_compile_definitions(cmakegen_tests PRIVATE CMAKEGEN_CMAKE_COMMAND="${CMAKE_COMMAND}")
    catch_discover_tests(cmakegen_tests)
endif()
