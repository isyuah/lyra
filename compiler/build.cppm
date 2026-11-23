module;
#include <filesystem>
#include <string_view>
#include <vector>

export module lyra.build;
import lyra.check;

export namespace lyra {
void build(const CheckedModule& functions, const std::vector<Source>& sources, const std::filesystem::path& output, bool release);
int execute(const std::filesystem::path& executable, const std::vector<std::string_view>& arguments);
}
