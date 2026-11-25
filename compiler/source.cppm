module;
#include <cstddef>
#include <filesystem>
#include <map>
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

struct SourceAnchor {
    std::size_t file;
    Span span;
    bool operator==(const SourceAnchor&) const = default;
};

struct NameReference { SourceAnchor use, declaration; std::optional<SourceAnchor> renameTarget{}; };

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
using SourceOverlay = std::map<std::filesystem::path, std::string>;

struct Source {
    std::filesystem::path path;
    std::string text;
    std::vector<std::size_t> lineStarts;
    std::string moduleId{};
    std::optional<Site> origin{};
    std::shared_ptr<const Source> previous{};
    std::vector<SourcePiece> pieces{};
    std::optional<std::size_t> importParent{};

    Position position(std::size_t offset) const;
    Site site(Span span) const;
    Site userSite(Span span) const;
    std::size_t hygieneAt(Span span) const;
    std::optional<Site> generatedSite(Span span) const;
};

enum class MissingNameKind { Value, Call, Type, Constraint };
struct MissingName { MissingNameKind kind; std::string name; };
struct Diagnostic {
    std::string code, message;
    std::optional<Site> site = {};
    std::string expected = {}, actual = {}, hint = {};
    std::optional<Site> generated{};
    std::optional<MissingName> missingName{};
};

Diagnostic at(const Source& source, Span span, std::string code, std::string message);
Diagnostic missingName(const Source& source, Span span, std::string code, std::string message, MissingNameKind kind, const std::string& name);
void report(const Diagnostic& error, bool json);
void reportWarnings(const std::vector<Diagnostic>& warnings, bool json);
void reportDiagnostics(const std::vector<Diagnostic>& warnings, const Diagnostic* error, bool json);
std::vector<Source> loadSources(const std::filesystem::path& package);
Source loadSource(const std::filesystem::path& path, std::string moduleId, const SourceOverlay* overlay = nullptr);
Source sourceText(const std::filesystem::path& path, std::string text, std::string moduleId);

}
