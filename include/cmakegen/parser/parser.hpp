#pragma once

#include "cmakegen/diagnostics/diagnostic.hpp"
#include "cmakegen/parser/raw_spec.hpp"

#include <string_view>

namespace cmakegen {

[[nodiscard]] Result<RawSpec> parse_raw_spec(std::string_view yaml);

} // namespace cmakegen
