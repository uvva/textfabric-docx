#pragma once

#include "textfabric/export.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace textfabric {

/// Library version
constexpr std::string_view version = "0.1.0";

/// Check whether a file exists at the given path.
TEXTFABRIC_API [[nodiscard]] bool file_exists(const std::filesystem::path& path);

/// Read entire file contents into a string.
/// Returns empty string if the file cannot be read.
TEXTFABRIC_API [[nodiscard]] std::string read_file(const std::filesystem::path& path);

} // namespace textfabric
