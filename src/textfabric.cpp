#include "textfabric/textfabric.hpp"

#include <fstream>
#include <sstream>

namespace textfabric {

bool file_exists(const std::filesystem::path& path) {
    return std::filesystem::exists(path) && std::filesystem::is_regular_file(path);
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return {};

    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

} // namespace textfabric
