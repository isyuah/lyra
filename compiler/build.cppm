module;
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module lyra.build;
import lyra.check;

export namespace lyra {
struct NativeBuildOptions { bool release{}, debugInfo{}; };
struct NativeArtifact { std::filesystem::path path; std::optional<std::string> digest; };
std::optional<std::string> fileContentKey(const std::filesystem::path& file);
std::vector<NativeArtifact> build(const CheckedModule& functions, const std::vector<Source>& sources, const std::filesystem::path& output,
    NativeBuildOptions options, const std::vector<std::filesystem::path>& libraries);
struct NativeOutput { std::filesystem::path standard, error; };
int execute(const std::filesystem::path& executable, const std::vector<std::string_view>& arguments,
    const NativeOutput* output = nullptr, unsigned timeoutSeconds = 0);
}
