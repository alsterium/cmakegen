#include "platform_info.hpp"

#include <iostream>

int main() {
    std::cout << common_component() << " + " << platform_name() << '\n';
    return 0;
}
