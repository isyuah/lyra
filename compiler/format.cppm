module;
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

export module lyra.format;
import lyra.platform;
import lyra.source;
import lyra.syntax;

export namespace lyra {

struct FormatOptions { unsigned tabSize = 4; bool insertSpaces = true; };
std::string formatSource(const Source& source, FormatOptions options = {});
void formatFile(const std::filesystem::path& path, bool checkOnly);

}

namespace lyra {
namespace fs = std::filesystem;
namespace {

bool horizontal(char byte) { return byte == ' ' || byte == '\t'; }

struct Delimiter { char close; std::size_t openingLine; };

char closingDelimiter(char open) {
    if (open == '{') return '}';
    if (open == '[') return ']';
    if (open == '(') return ')';
    return '\0';
}

}

std::string formatSource(const Source& source, FormatOptions options) {
    std::string_view text = source.text;
    auto tokens = syntaxTokens(source, true);
    std::vector<Delimiter> delimiters;
    std::string output;
    output.reserve(text.size());
    std::string_view firstNewline;
    std::size_t tokenIndex = 0, level = 0;
    for (std::size_t line = 0; line < source.lineStarts.size(); ++line) {
        auto begin = source.lineStarts[line];
        auto next = line + 1 == source.lineStarts.size() ? text.size() : source.lineStarts[line + 1];
        auto end = next;
        if (end > begin && text[end - 1] == '\n') --end;
        if (end > begin && text[end - 1] == '\r') --end;
        if (firstNewline.empty()) firstNewline = text.substr(end, next - end);

        auto indent = level;
        auto protectedEnd = begin;
        bool insideToken = tokenIndex < tokens.size() && tokens[tokenIndex].span.begin < begin;
        bool leading = !insideToken;
        while (tokenIndex < tokens.size() && tokens[tokenIndex].span.begin < end) {
            const auto& token = tokens[tokenIndex];
            auto spelling = text.substr(token.span.begin, token.span.end - token.span.begin);
            char symbol = token.kind == "token" && spelling.size() == 1 ? spelling.front() : '\0';
            if (!delimiters.empty() && symbol == delimiters.back().close) {
                auto opening = delimiters.back().openingLine;
                delimiters.pop_back();
                if (delimiters.empty() || delimiters.back().openingLine != opening) --level;
                if (leading) indent = level;
            } else {
                leading = false;
                if (auto close = closingDelimiter(symbol)) {
                    if (delimiters.empty() || delimiters.back().openingLine != line) ++level;
                    delimiters.push_back({close, line});
                }
            }
            protectedEnd = std::min(end, token.span.end);
            if (token.span.end > end) break;
            ++tokenIndex;
        }

        auto trimmedEnd = end;
        while (trimmedEnd > protectedEnd && horizontal(text[trimmedEnd - 1])) --trimmedEnd;
        auto content = begin;
        if (!insideToken) {
            while (content < trimmedEnd && horizontal(text[content])) ++content;
            if (content != trimmedEnd) output.append(options.insertSpaces ? indent * options.tabSize : indent, options.insertSpaces ? ' ' : '\t');
        }
        output.append(text.substr(content, trimmedEnd - content));
        output.append(text.substr(end, next - end));
    }
    if (!output.empty() && output.back() != '\n' && output.back() != '\r') output.append(firstNewline.empty() ? "\n" : firstNewline);
    return output;
}

void formatFile(const fs::path& path, bool checkOnly) {
    auto status = fs::symlink_status(path);
    if (path.extension() != ".ly" || !fs::is_regular_file(status))
        throw Diagnostic{"LYI0001", "格式化需要普通 .ly 文件：" + pathText(path)};
    auto source = loadSource(path, {});
    auto output = formatSource(source);
    if (output == source.text) return;
    if (checkOnly) {
        auto difference = std::ranges::mismatch(source.text, output).in1 - source.text.begin();
        auto begin = static_cast<std::size_t>(difference);
        throw at(source, {begin, begin}, "LYFMT0001", "文件尚未按 Lyra 格式整理；运行 lyra fmt");
    }
    StagingDirectory staging(path.parent_path());
    auto temporary = staging.path / path.filename();
    std::ofstream stream(temporary, std::ios::binary);
    stream << output;
    stream.close();
    if (!stream) throw Diagnostic{"LYI0001", "无法写入格式化结果：" + pathText(path)};
    fs::permissions(temporary, status.permissions());
    publishExecutable(temporary, path);
}

}
