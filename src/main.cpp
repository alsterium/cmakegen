#include "cmakegen/version.hpp"

#include <CLI/CLI.hpp>

#include <iostream>

int main(int argc, char** argv) {
    CLI::App app{"Generate reproducible CMake projects from project.yml."};
    app.set_version_flag("--version", std::string{cmakegen::version()});

    CLI11_PARSE(app, argc, argv);
    std::cout << "cmakegen " << cmakegen::version() << '\n';
    return 0;
}
