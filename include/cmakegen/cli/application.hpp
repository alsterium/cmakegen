#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace cmakegen {

/// Executes the documented command-line interface.  Keeping this boundary out
/// of main makes command behavior directly testable without spawning a process.
[[nodiscard]] int run_cli(const std::vector<std::string>& arguments, std::ostream& output,
                          std::ostream& error);

} // namespace cmakegen
