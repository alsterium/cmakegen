#include "cmakegen/version.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("the development version is available to clients") {
    CHECK(cmakegen::version() == "0.0.0-dev");
}
