module;
#include <filesystem>
#include <string_view>
#include <vector>

export module lyra.build;
import lyra.check;

export namespace lyra {
struct NativeBuildOptions { bool release{}, debugInfo{}; };
void build(const CheckedModule& functions, const std::vector<Source>& sources, const std::filesystem::path& output,
    NativeBuildOptions options, const std::vector<std::filesystem::path>& libraries);
struct NativeOutput { std::filesystem::path standard, error; };
int execute(const std::filesystem::path& executable, const std::vector<std::string_view>& arguments,
    const NativeOutput* output = nullptr, unsigned timeoutSeconds = 0);
}
