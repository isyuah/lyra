module;
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/Unicode.h>
#include <llvm/Support/raw_ostream.h>
#include <vector>

module lyra.source;
import lyra.platform;

namespace lyra {

namespace fs = std::filesystem;

static Position rawPosition(const Source& source, std::size_t offset) {
    const auto& text = source.text;
    const auto& lineStarts = source.lineStarts;
    auto next = std::upper_bound(lineStarts.begin(), lineStarts.end(), offset);
    auto start = *std::prev(next);
    std::size_t column = 1;
    for (auto index = start; index < offset; ++index) {
        if ((static_cast<unsigned char>(text[index]) & 0xc0) != 0x80) ++column;
    }
    return {offset, static_cast<std::size_t>(next - lineStarts.begin()), column};
}

static Site rawSite(const Source& source, Span span) {
    auto start = rawPosition(source, span.begin);
    auto lineStart = source.lineStarts[start.line - 1];
    auto lineEnd = source.text.find_first_of("\r\n", lineStart);
    return {pathText(source.path), start, rawPosition(source, span.end), source.text.substr(lineStart, lineEnd - lineStart), lineStart};
}

static const SourcePiece& pieceAt(const Source& source, std::size_t offset) {
    return *std::prev(std::ranges::upper_bound(source.pieces, offset, {}, [](const auto& piece) { return piece.output.begin; }));
}

static Span beforeExpansion(const Source& source, Span span) {
    const auto& first = pieceAt(source, span.begin);
    const auto& last = span.end == span.begin ? first : pieceAt(source, span.end - 1);
    auto begin = first.copied ? first.input.begin + span.begin - first.output.begin : first.input.begin;
    auto end = last.copied ? last.input.begin + span.end - last.output.begin : last.input.end;
    return {std::min(begin, end), std::max(begin, end)};
}

Site Source::site(Span span) const {
    return previous ? previous->site(beforeExpansion(*this, span)) : rawSite(*this, span);
}

Site Source::userSite(Span span) const {
    return origin && !previous ? *origin : site(span);
}

Position Source::position(std::size_t offset) const {
    return previous ? site({offset, offset}).start : rawPosition(*this, offset);
}

std::size_t Source::hygieneAt(Span span) const {
    if (!previous) return 0;
    const auto& piece = pieceAt(*this, span.begin);
    return piece.copied ? previous->hygieneAt(beforeExpansion(*this, span)) : piece.hygiene;
}

std::optional<Site> Source::generatedSite(Span span) const {
    if (!previous) return {};
    const auto& piece = pieceAt(*this, span.begin);
    if (piece.copied) return previous->generatedSite(beforeExpansion(*this, span));
    return piece.hygiene ? std::optional(rawSite(*this, span)) : std::nullopt;
}

Diagnostic at(const Source& source, Span span, std::string code, std::string message) {
    Diagnostic error{std::move(code), std::move(message), source.userSite(span)};
    if (source.origin && !source.previous) error.generated = source.site(span);
    else error.generated = source.generatedSite(span);
    return error;
}

Diagnostic missingName(const Source& source, Span span, std::string code, std::string message, MissingNameKind kind, const std::string& name) {
    auto error = at(source, span, std::move(code), std::move(message));
    if (!error.generated && !name.empty() && name.find_first_of(".$:") == name.npos) error.missingName = MissingName{kind, name};
    return error;
}

static llvm::json::Object jsonPosition(Position position) {
    return llvm::json::Object{{"offset", position.offset}, {"line", position.line}, {"column", position.column}};
}

static llvm::json::Object jsonDiagnostic(const Diagnostic& error) {
    llvm::json::Object item{{"code", error.code}, {"message", llvm::json::fixUTF8(error.message)}};
    if (error.site) {
        item["file"] = llvm::json::fixUTF8(error.site->file);
        item["range"] = llvm::json::Object{{"start", jsonPosition(error.site->start)}, {"end", jsonPosition(error.site->end)}};
    }
    if (!error.expected.empty()) item["expected"] = error.expected;
    if (!error.actual.empty()) item["actual"] = error.actual;
    if (!error.hint.empty()) item["hint"] = error.hint;
    if (error.generated) item["generated"] = llvm::json::Object{{"file", error.generated->file}, {"start", jsonPosition(error.generated->start)}, {"end", jsonPosition(error.generated->end)}, {"source", error.generated->lineText}};
    return item;
}

static void reportText(const Diagnostic& error, bool warning) {
    auto& stream = llvm::errs();
    if (error.site) stream << error.site->file << ':' << error.site->start.line << ':' << error.site->start.column << ": ";
    if (warning) stream << "warning: ";
    stream << error.code << ": " << error.message << '\n';
    if (error.site) {
        stream << error.site->lineText << '\n';
        auto prefix = llvm::StringRef(error.site->lineText).take_front(error.site->start.offset - error.site->lineStartOffset);
        while (!prefix.empty()) {
            auto [segment, rest] = prefix.split('\t');
            stream.indent(std::max(0, llvm::sys::unicode::columnWidthUTF8(segment)));
            if (segment.size() != prefix.size()) stream << '\t';
            prefix = rest;
        }
        stream << "^\n";
    }
    if (!error.hint.empty()) stream << "提示：" << error.hint << '\n';
    if (error.generated) stream << "展开代码 " << error.generated->file << ':' << error.generated->start.line << ": " << error.generated->lineText << '\n';
}

void report(const Diagnostic& error, bool json) {
    reportDiagnostics({}, &error, json);
}

void reportWarnings(const std::vector<Diagnostic>& warnings, bool json) {
    reportDiagnostics(warnings, nullptr, json);
}

void reportDiagnostics(const std::vector<Diagnostic>& warnings, const Diagnostic* error, bool json) {
    if (!json) {
        for (const auto& warning : warnings) reportText(warning, true);
        if (error) reportText(*error, false);
        return;
    }
    llvm::json::Array diagnostics;
    for (const auto& warning : warnings) {
        auto item = jsonDiagnostic(warning);
        item["severity"] = "warning";
        diagnostics.push_back(std::move(item));
    }
    if (error) diagnostics.push_back(jsonDiagnostic(*error));
    llvm::errs() << llvm::formatv("{0}\n", llvm::json::Value(std::move(diagnostics)));
}

std::vector<Source> loadSources(const fs::path& package) {
    auto directory = package / "src";
    if (!fs::is_directory(directory)) throw Diagnostic{"LYI0001", "找不到源码目录：" + pathText(directory)};
    std::vector<fs::path> paths;
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (entry.path().extension() == ".ly" && entry.is_regular_file()) paths.push_back(entry.path());
    }
    if (paths.empty()) throw Diagnostic{"LYI0001", "源码目录里没有 .ly 文件：" + pathText(directory)};
    std::sort(paths.begin(), paths.end());
    std::vector<Source> sources;
    for (const auto& path : paths) sources.push_back(loadSource(path, {}));
    return sources;
}

Source loadSource(const fs::path& path, std::string moduleId, const SourceOverlay* overlay) {
    if (overlay) if (auto found = overlay->find(path); found != overlay->end()) return sourceText(path, found->second, std::move(moduleId));
    std::ifstream input(path, std::ios::binary);
    if (!input) throw Diagnostic{"LYI0001", "无法打开源码：" + pathText(path)};
    std::string text;
    char buffer[8192];
    while (input.read(buffer, sizeof buffer) || input.gcount() > 0) text.append(buffer, input.gcount());
    if (!input.eof() || input.bad()) throw Diagnostic{"LYI0001", "读取源码失败：" + pathText(path)};
    return sourceText(path, std::move(text), std::move(moduleId));
}

Source sourceText(const fs::path& path, std::string text, std::string moduleId) {
    Source source{path, std::move(text), {0}, std::move(moduleId)};
    for (std::size_t index = 0; index < source.text.size(); ++index) {
        if (source.text[index] == '\n' || (source.text[index] == '\r' && (index + 1 == source.text.size() || source.text[index + 1] != '\n'))) source.lineStarts.push_back(index + 1);
    }
    std::size_t invalidOffset;
    if (!llvm::json::isUTF8(source.text, &invalidOffset)) throw at(source, {invalidOffset, std::min(invalidOffset + 1, source.text.size())}, "LYL0001", "源码必须是有效的 UTF-8");
    return source;
}

}
