// Consumer smoke test for the installed TextFabric CMake package.
// Proves find_package(TextFabric CONFIG) + public headers + linkage all work.
// Does not load or modify any .docx — the goal here is the surface, not behavior.

#include <textfabric/error.hpp>
#include <textfabric/merger.hpp>

int main() {
    auto merger = textfabric::make_docx_merger();
    return merger ? 0 : 1;
}
