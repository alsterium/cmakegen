#pragma once

#include "cmakegen/domain/model.hpp"
#include "cmakegen/parser/raw_spec.hpp"

namespace cmakegen {

[[nodiscard]] Project normalize_project(const RawSpec& raw);

} // namespace cmakegen
