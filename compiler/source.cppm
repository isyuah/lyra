module;
#include <compare>
#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

export module lyra.source;

export namespace lyra {

struct Span {
    std::size_t begin, end;
    auto operator<=>(const Span&) const = default;
};

struct SourceAnchor {
    std::size_t file;
    Span span;
    auto operator<=>(const SourceAnchor&) const = default;
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


struct SourcePiece { Span output, input; bool copied; std::size_t hygiene; std::shared_ptr<const Site> macroDefinition{}; };
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
    std::shared_ptr<const Site> macroDefinition{};

    Position position(std::size_t offset) const;
    Site site(Span span) const;
    Site userSite(Span span) const;
    std::optional<Span> authoredSpan(Span span) const;
    std::size_t hygieneAt(Span span) const;
    std::optional<Site> generatedSite(Span span) const;
    const Site* macroSite(Span span) const;
};

enum class MissingNameKind { Value, Call, Type, Constraint };
struct MissingName { MissingNameKind kind; std::string name; };
struct DiagnosticNote { Site site; std::string message; };
struct Diagnostic {
    std::string code, message;
    std::optional<Site> site = {};
    std::string expected = {}, actual = {}, hint = {};
    std::optional<Site> generated{};
    std::optional<MissingName> missingName{};
    std::vector<DiagnosticNote> notes{};
    bool removeTailSemicolon{};
};

struct AnalysisDiagnostics {
    std::vector<Diagnostic> errors, warnings;
    bool full() const { return errors.size() >= 50; }
    void add(Diagnostic error);
    void sort();
};

struct AnalysisCancelled {};
inline thread_local std::stop_token analysisStop;

inline void checkAnalysisCancellation() {
    if (analysisStop.stop_requested()) throw AnalysisCancelled{};
}

Diagnostic at(const Source& source, Span span, std::string code, std::string message);
Diagnostic missingName(const Source& source, Span span, std::string code, std::string message, MissingNameKind kind, const std::string& name);
void report(const Diagnostic& error, bool json);
void reportWarnings(const std::vector<Diagnostic>& warnings, bool json);
void reportDiagnostics(std::span<const Diagnostic> warnings, std::span<const Diagnostic> errors, bool json);
std::vector<Source> loadSources(const std::filesystem::path& package);
Source loadSource(const std::filesystem::path& path, std::string moduleId, const SourceOverlay* overlay = nullptr);
Source sourceText(const std::filesystem::path& path, std::string text, std::string moduleId);

}
