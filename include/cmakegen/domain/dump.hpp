#pragma once

#include "cmakegen/domain/model.hpp"

#include <string>

namespace cmakegen {

[[nodiscard]] std::string dump_project_json(const Project& project);
[[nodiscard]] std::string dump_project_yaml(const Project& project);

} // namespace cmakegen
