# Hand-maintained bootstrap dependency hook. This runs before generated package
# lookups and is not generated from project.yml.
include_guard(GLOBAL)

include(FetchContent)

# yaml-cpp 0.8.0 declares compatibility back to CMake 3.4. CMake 4 removed
# that policy level; retain its supported 3.5 policy baseline while embedding it.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING
    "Minimum policy version used by bootstrap third-party dependencies" FORCE)

function(cmakegen_require_package package version target url sha256)
    find_package(${package} ${version} CONFIG QUIET)
    if(TARGET ${target})
        if(package STREQUAL "Catch2")
            # Catch2Config.cmake adds its discovery module in this function's
            # scope. Keep it available to bootstrap's later include(Catch).
            set(CMAKE_MODULE_PATH "${CMAKE_MODULE_PATH}" PARENT_SCOPE)
        endif()
        return()
    endif()

    FetchContent_Declare(${package}
        URL "${url}"
        URL_HASH "SHA256=${sha256}"
        OVERRIDE_FIND_PACKAGE
    )
    FetchContent_MakeAvailable(${package})

    if(NOT TARGET ${target})
        message(FATAL_ERROR
            "${package} ${version} did not provide the required target ${target}")
    endif()

    if(package STREQUAL "Catch2")
        # FetchContent adds Catch2 in this helper's scope, so propagate its
        # test-discovery module to the including bootstrap directory.
        list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
        set(CMAKE_MODULE_PATH "${CMAKE_MODULE_PATH}" PARENT_SCOPE)
    endif()
endfunction()

# Keep third-party test and documentation targets out of the bootstrap build.
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(JSON_BuildTests OFF CACHE INTERNAL "" FORCE)
set(CATCH_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)

# Catch2 is built before schema-generated targets can apply cxx_std_23. Its
# string_view support must use the same language level as cmakegen's tests.
# Bootstrap already sets this; normal builds need it before FetchContent.
if(NOT CMAKEGEN_BOOTSTRAP_MINIMAL)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

cmakegen_require_package(
    yaml-cpp 0.8.0 yaml-cpp::yaml-cpp
    "https://github.com/jbeder/yaml-cpp/archive/refs/tags/0.8.0.tar.gz"
    "fbe74bbdcee21d656715688706da3c8becfd946d92cd44705cc6098bb23b3a16")
cmakegen_require_package(
    nlohmann_json 3.12.0 nlohmann_json::nlohmann_json
    "https://github.com/nlohmann/json/archive/refs/tags/v3.12.0.tar.gz"
    "4b92eb0c06d10683f7447ce9406cb97cd4b453be18d7279320f7b2f025c10187")
cmakegen_require_package(
    CLI11 2.6.2 CLI11::CLI11
    "https://github.com/CLIUtils/CLI11/archive/refs/tags/v2.6.2.tar.gz"
    "c6ea6b2e5608b3ea8617999bd5f47420c71b2ebdb8dc4767c1034d1da5785711")

# The normal build has a test target. Bootstrap deliberately sets
# CMAKEGEN_BOOTSTRAP_MINIMAL, so it does not acquire test-only dependencies.
if(NOT CMAKEGEN_BOOTSTRAP_MINIMAL)
    cmakegen_require_package(
        Catch2 3.15.3 Catch2::Catch2WithMain
        "https://github.com/catchorg/Catch2/archive/refs/tags/v3.15.3.tar.gz"
        "b0299ae552918220a7a6e21e7de5b714777f4e8c883fb70c4bb23fe01df8c6e3")
endif()
