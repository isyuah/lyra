module;
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
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

struct Site {
    std::string file;
    Position start, end;
    std::string lineText;
    std::size_t lineStartOffset;
};

struct SourcePiece { Span output, input; bool copied; std::size_t hygiene; };

struct Source {
    std::filesystem::path path;
    std::string text;
    std::vector<std::size_t> lineStarts;
    std::string moduleId{};
    std::optional<Site> origin{};
    std::shared_ptr<const Source> previous{};
    std::vector<SourcePiece> pieces{};

    Position position(std::size_t offset) const;
    Site site(Span span) const;
    std::size_t hygieneAt(Span span) const;
    std::optional<Site> generatedSite(Span span) const;
};

struct Diagnostic {
    std::string code, message;
    std::optional<Site> site = {};
    std::string expected = {}, actual = {}, hint = {};
    std::optional<Site> generated{};
};

Diagnostic at(const Source& source, Span span, std::string code, std::string message);
void report(const Diagnostic& error, bool json);
std::vector<Source> loadSources(const std::filesystem::path& package);
Source loadSource(const std::filesystem::path& path, std::string moduleId);
Source sourceText(const std::filesystem::path& path, std::string text, std::string moduleId);

}
