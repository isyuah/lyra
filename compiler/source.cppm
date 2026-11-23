module;
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

export module lyra.source;

export namespace lyra {

struct Span {
    std::size_t begin, end;
    bool operator==(const Span&) const = default;
};

struct Position {
    std::size_t offset, line, column;
};

struct Source {
    std::filesystem::path path;
    std::string text;
    std::vector<std::size_t> lineStarts;
    std::string moduleId{};

    Position position(std::size_t offset) const;
};

struct Site {
    std::string file;
    Position start, end;
    std::string lineText;
    std::size_t lineStartOffset;
};

struct Diagnostic {
    std::string code, message;
    std::optional<Site> site = {};
    std::string expected = {}, actual = {}, hint = {};
};

Diagnostic at(const Source& source, Span span, std::string code, std::string message);
void report(const Diagnostic& error, bool json);
std::vector<Source> loadSources(const std::filesystem::path& package);
Source loadSource(const std::filesystem::path& path, std::string moduleId);

}
