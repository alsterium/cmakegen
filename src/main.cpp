#include "cmakegen/cli/application.hpp"

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc - 1));
    for (int index = 1; index < argc; ++index) arguments.emplace_back(argv[index]);
    return cmakegen::run_cli(arguments, std::cout, std::cerr);
}
