#pragma once

#include <string>

namespace bridge {

[[nodiscard]] auto calculate_hash(const std::string &input) -> std::string;

} // namespace bridge
