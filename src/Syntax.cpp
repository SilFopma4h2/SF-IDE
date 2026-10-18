#include "Syntax.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

namespace syntax {
namespace {

bool identStart(char c) { return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (unsigned char)c >= 0x80; }
bool identChar(char c)  { return identStart(c) || (c >= '0' && c <= '9'); }
bool digit(char c)      { return c >= '0' && c <= '9'; }

// Sorted so lookups can use binary search.
const char* const kKeywords[] = {
    "alignas", "alignof", "and", "asm", "auto", "break", "case", "catch", "class", "co_await", "co_return",
    "co_yield", "concept", "const", "const_cast", "consteval", "constexpr", "constinit", "continue", "decltype",
    "default", "defer", "delete", "do", "dynamic_cast", "else", "enum", "explicit", "export", "extern", "false",
    "final", "for", "friend", "goto", "if", "import", "inline", "module", "mutable", "namespace", "new",
    "noexcept", "not", "nullptr", "operator", "or", "override", "private", "protected", "public", "register",
    "reinterpret_cast", "requires", "return", "sizeof", "static", "static_assert", "static_cast", "struct",
    "switch", "template", "this", "thread_local", "throw", "true", "try", "typedef", "typeid", "typename",
    "union", "using", "virtual", "volatile", "while",
};

const char* const kTypes[] = {
    "FILE", "bool", "char", "char16_t", "char32_t", "char8_t", "double", "float", "int", "int16_t", "int32_t",
    "int64_t", "int8_t", "intptr_t", "long", "ptrdiff_t", "short", "signed", "size_t", "ssize_t", "std",
    "string", "string_view", "uint16_t", "uint32_t", "uint64_t", "uint8_t", "uintptr_t", "unsigned", "vector",
    "void", "wchar_t",
};

const char* const kHashKeywords[] = {
    "add_executable", "add_library", "and", "as", "case", "cmake_minimum_required", "def", "do", "done", "elif",
    "else", "endforeach", "endfunction", "endif", "esac", "export", "fi", "find_package", "for", "foreach",
    "from", "function", "if", "import", "in", "include", "lambda", "local", "not", "or", "project", "return",
    "set", "target_compile_options", "target_include_directories", "target_link_libraries", "then", "while",
};

template <size_t N>
bool lookup(const char* const (&table)[N], std::string_view w)
{
    auto it = std::lower_bound(std::begin(table), std::end(table), w,
                               [](const char* a, std::string_view b) { return std::string_view(a) < b; });
    return it != std::end(table) && std::string_view(*it) == w;
}

void push(std::vector<Token>& out, size_t b, size_t e, Kind k)
{
    if (e <= b) return;
    if (!out.empty() && out.back().kind == k && out.back().end == b) { out.back().end = uint32_t(e); return; }
    out.push_back({ uint32_t(b), uint32_t(e), k });
}

size_t scanNumber(std::string_view s, size_t i)
{
    const size_t n = s.size();
    while (i < n && (identChar(s[i]) || s[i] == '.' || s[i] == '\'' ||
                     ((s[i] == '+' || s[i] == '-') && i > 0 && (s[i - 1] == 'e' || s[i - 1] == 'E' || s[i - 1] == 'p'))))
        ++i;
    return i;
}

size_t scanQuoted(std::string_view s, size_t i, char q)
{
    const size_t n = s.size();
    ++i;
    while (i < n && s[i] != q)
    {
        if (s[i] == '\\' && i + 1 < n) ++i;
        ++i;
    }
    return i < n ? i + 1 : n;
}

void tokenizeC(std::string_view s, bool inBlock, std::vector<Token>& out)
{
    const size_t n = s.size();
    size_t i = 0;
    if (inBlock)
    {
        size_t e = s.find("*/");
        e = (e == std::string_view::npos) ? n : e + 2;
        push(out, 0, e, Kind::Comment);
        i = e;
    }
    // A line starting with '#' is a preprocessor directive.
    size_t first = s.find_first_not_of(" \t");
    bool directive = !inBlock && first != std::string_view::npos && s[first] == '#';
    bool include = false;
    while (i < n)
    {
        char c = s[i];
        if (c == '/' && i + 1 < n && s[i + 1] == '/') { push(out, i, n, Kind::Comment); return; }
        if (c == '/' && i + 1 < n && s[i + 1] == '*')
        {
            size_t e = s.find("*/", i + 2);
            e = (e == std::string_view::npos) ? n : e + 2;
            push(out, i, e, Kind::Comment);
            i = e;
            continue;
        }
        if (c == '"' || c == '\'')
        {
            size_t e = scanQuoted(s, i, c);
            push(out, i, e, Kind::String);
            i = e;
            continue;
        }
        if (include && c == '<')
        {
            size_t e = s.find('>', i);
            e = (e == std::string_view::npos) ? n : e + 1;
            push(out, i, e, Kind::String);
            i = e;
            continue;
        }
        if (digit(c) || (c == '.' && i + 1 < n && digit(s[i + 1])))
        {
            size_t e = scanNumber(s, i);
            push(out, i, e, Kind::Number);
            i = e;
            continue;
        }
        if (c == '#' && directive && i == first)
        {
            size_t j = i + 1;
            while (j < n && (s[j] == ' ' || s[j] == '\t')) ++j;
            size_t k = j;
            while (k < n && identChar(s[k])) ++k;
            include = s.substr(j, k - j) == "include" || s.substr(j, k - j) == "import";
            push(out, i, k, Kind::Preproc);
            i = k;
            continue;
        }
        if (identStart(c))
        {
            size_t e = i + 1;
            while (e < n && identChar(s[e])) ++e;
            std::string_view w = s.substr(i, e - i);
            Kind k = Kind::Default;
            if (lookup(kKeywords, w)) k = Kind::Keyword;
            else if (lookup(kTypes, w)) k = Kind::Type;
            else
            {
                size_t j = e;
                while (j < n && s[j] == ' ') ++j;
                if (j < n && s[j] == '(') k = Kind::Function;
                else if (w.size() > 1 && w[0] >= 'A' && w[0] <= 'Z' && std::any_of(w.begin(), w.end(), [](char ch) { return ch >= 'a' && ch <= 'z'; }))
                    k = Kind::Type;  // CamelCase names are usually types
            }
            push(out, i, e, k);
            i = e;
            continue;
        }
        if (std::strchr("{}()[];,.<>=+-*/%&|^!~?:", c))
        {
            push(out, i, i + 1, Kind::Punct);
            ++i;
            continue;
        }
        push(out, i, i + 1, Kind::Default);
        ++i;
    }
}

void tokenizeHash(std::string_view s, std::vector<Token>& out)
{
    const size_t n = s.size();
    size_t i = 0;
    while (i < n)
    {
        char c = s[i];
        if (c == '#') { push(out, i, n, Kind::Comment); return; }
        if (c == '"' || c == '\'')
        {
            size_t e = scanQuoted(s, i, c);
            push(out, i, e, Kind::String);
            i = e;
            continue;
        }
        if (c == '$' && i + 1 < n && (s[i + 1] == '{' || s[i + 1] == '('))
        {
            size_t e = s.find(s[i + 1] == '{' ? '}' : ')', i);
            e = (e == std::string_view::npos) ? n : e + 1;
            push(out, i, e, Kind::Preproc);
            i = e;
            continue;
        }
        if (digit(c))
        {
            size_t e = scanNumber(s, i);
            push(out, i, e, Kind::Number);
            i = e;
            continue;
        }
        if (identStart(c))
        {
            size_t e = i + 1;
            while (e < n && (identChar(s[e]) || s[e] == '-')) ++e;
            std::string_view w = s.substr(i, e - i);
            Kind k = Kind::Default;
            std::string lower(w);
            for (auto& ch : lower) ch = char(std::tolower((unsigned char)ch));
            if (lookup(kHashKeywords, lower)) k = Kind::Keyword;
            else if (e < n && s[e] == '(') k = Kind::Function;
            else if (std::all_of(w.begin(), w.end(), [](char ch) { return (ch >= 'A' && ch <= 'Z') || ch == '_' || digit(ch); }) && w.size() > 1)
                k = Kind::Type;
            push(out, i, e, k);
            i = e;
            continue;
        }
        push(out, i, i + 1, std::strchr("{}()[]=:;,", c) ? Kind::Punct : Kind::Default);
        ++i;
    }
}

void tokenizeMarkdown(std::string_view s, std::vector<Token>& out)
{
    size_t first = s.find_first_not_of(' ');
    if (first != std::string_view::npos && s[first] == '#') { push(out, 0, s.size(), Kind::Keyword); return; }
    if (s.substr(0, 3) == "```") { push(out, 0, s.size(), Kind::Preproc); return; }
    const size_t n = s.size();
    size_t i = 0;
    while (i < n)
    {
        if (s[i] == '`')
        {
            size_t e = s.find('`', i + 1);
            e = (e == std::string_view::npos) ? n : e + 1;
            push(out, i, e, Kind::String);
            i = e;
            continue;
        }
        push(out, i, i + 1, Kind::Default);
        ++i;
    }
}

} // namespace

void tokenize(Document::Lang lang, std::string_view line, bool startsInBlockComment, std::vector<Token>& out)
{
    out.clear();
    switch (lang)
    {
    case Document::Lang::CFamily:  tokenizeC(line, startsInBlockComment, out); break;
    case Document::Lang::Hash:     tokenizeHash(line, out); break;
    case Document::Lang::Markdown: tokenizeMarkdown(line, out); break;
    default:                       push(out, 0, line.size(), Kind::Default); break;
    }
}

} // namespace syntax
