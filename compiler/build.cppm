module;
#include <filesystem>
#include <vector>

export module lyra.build;
import lyra.check;

export namespace lyra {
void build(const Module& functions, const std::vector<Source>& sources, const std::filesystem::path& output, bool release);
}
