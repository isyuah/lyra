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

namespace lyra {

namespace fs = std::filesystem;

Position Source::position(std::size_t offset) const {
    auto next = std::upper_bound(lineStarts.begin(), lineStarts.end(), offset);
    auto start = *std::prev(next);
    std::size_t column = 1;
    for (auto index = start; index < offset; ++index) {
        if ((static_cast<unsigned char>(text[index]) & 0xc0) != 0x80) ++column;
    }
    return {offset, static_cast<std::size_t>(next - lineStarts.begin()), column};
}

Diagnostic at(const Source& source, Span span, std::string code, std::string message) {
    auto start = source.position(span.begin);
    auto lineStart = source.lineStarts[start.line - 1];
    auto lineEnd = source.text.find_first_of("\r\n", lineStart);
    Site site{source.path.string(), start, source.position(span.end), source.text.substr(lineStart, lineEnd - lineStart), lineStart};
    return {std::move(code), std::move(message), std::move(site)};
}

static llvm::json::Object jsonPosition(Position position) {
    return llvm::json::Object{{"offset", position.offset}, {"line", position.line}, {"column", position.column}};
}

void report(const Diagnostic& error, bool json) {
    auto& stream = llvm::errs();
    if (json) {
        llvm::json::Object item{{"code", error.code}, {"message", llvm::json::fixUTF8(error.message)}};
        if (error.site) {
            item["file"] = llvm::json::fixUTF8(error.site->file);
            item["range"] = llvm::json::Object{{"start", jsonPosition(error.site->start)}, {"end", jsonPosition(error.site->end)}};
        }
        if (!error.expected.empty()) item["expected"] = error.expected;
        if (!error.actual.empty()) item["actual"] = error.actual;
        if (!error.hint.empty()) item["hint"] = error.hint;
        stream << llvm::formatv("{0}\n", llvm::json::Value(llvm::json::Array{std::move(item)}));
        return;
    }
    if (error.site) stream << error.site->file << ':' << error.site->start.line << ':' << error.site->start.column << ": ";
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
}

std::vector<Source> loadSources(const fs::path& package) {
    auto directory = package / "src";
    if (!fs::is_directory(directory)) throw Diagnostic{"LYI0001", "找不到源码目录：" + directory.string()};
    std::vector<fs::path> paths;
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (entry.path().extension() == ".ly" && entry.is_regular_file()) paths.push_back(entry.path());
    }
    if (paths.empty()) throw Diagnostic{"LYI0001", "源码目录里没有 .ly 文件：" + directory.string()};
    std::sort(paths.begin(), paths.end());
    std::vector<Source> sources;
    for (const auto& path : paths) {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw Diagnostic{"LYI0001", "无法打开源码：" + path.string()};
        Source source{path, {}, {0}};
        char buffer[8192];
        while (input.read(buffer, sizeof buffer) || input.gcount() > 0) source.text.append(buffer, input.gcount());
        if (!input.eof() || input.bad()) throw Diagnostic{"LYI0001", "读取源码失败：" + path.string()};
        for (std::size_t index = 0; index < source.text.size(); ++index) {
            if (source.text[index] == '\n' || (source.text[index] == '\r' && (index + 1 == source.text.size() || source.text[index + 1] != '\n'))) {
                source.lineStarts.push_back(index + 1);
            }
        }
        std::size_t invalidOffset;
        if (!llvm::json::isUTF8(source.text, &invalidOffset)) {
            throw at(source, {invalidOffset, std::min(invalidOffset + 1, source.text.size())}, "LYL0001", "源码必须是有效的 UTF-8");
        }
        sources.push_back(std::move(source));
    }
    return sources;
}

}
