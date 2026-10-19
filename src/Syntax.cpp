#include "Syntax.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

#include "Languages.h"

namespace syntax {
namespace {

bool identStart(char c) { return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (unsigned char)c >= 0x80; }
bool identChar(char c)  { return identStart(c) || (c >= '0' && c <= '9'); }
bool digit(char c)      { return c >= '0' && c <= '9'; }
bool space(char c)      { return c == ' ' || c == '\t'; }

struct Out {
    std::vector<Token>* v;
    void push(size_t b, size_t e, Kind k)
    {
        if (!v || e <= b) return;
        if (!v->empty() && v->back().kind == k && v->back().end == b) { v->back().end = uint32_t(e); return; }
        v->push_back({ uint32_t(b), uint32_t(e), k });
    }
};

bool startsWith(std::string_view s, size_t i, const std::string& p)
{
    return !p.empty() && s.size() - i >= p.size() && s.compare(i, p.size(), p) == 0;
}

size_t scanNumber(std::string_view s, size_t i)
{
    const size_t n = s.size();
    while (i < n && (identChar(s[i]) || s[i] == '.' || s[i] == '\'' ||
                     ((s[i] == '+' || s[i] == '-') && i > 0 && (s[i - 1] == 'e' || s[i - 1] == 'E' || s[i - 1] == 'p'))))
    {
        // "1..10" ranges: stop before the second dot.
        if (s[i] == '.' && i + 1 < n && s[i + 1] == '.') break;
        ++i;
    }
    return i;
}

// Returns the index after the closing quote, or npos when the line ends first.
size_t scanQuoted(std::string_view s, size_t i, char q)
{
    const size_t n = s.size();
    ++i;
    while (i < n && s[i] != q)
    {
        if (s[i] == '\\' && i + 1 < n) ++i;
        ++i;
    }
    return i < n ? i + 1 : std::string_view::npos;
}

size_t findUnescaped(std::string_view s, size_t from, std::string_view what)
{
    for (size_t i = from; i + what.size() <= s.size(); ++i)
    {
        if (s[i] == '\\') { ++i; continue; }
        if (s.compare(i, what.size(), what) == 0) return i;
    }
    return std::string_view::npos;
}

bool lineCommentAt(const Language& L, std::string_view s, size_t i)
{
    for (const std::string* c : { &L.lineComment, &L.lineComment2 })
    {
        if (!startsWith(s, i, *c)) continue;
        // '#' starts a comment only at a word boundary: not in "$#" or "a#b".
        if (*c == "#" && i > 0 && !space(s[i - 1]) && s[i - 1] != ';' && s[i - 1] != '(' && s[i - 1] != '{')
            continue;
        // Ruby's =begin / Lua's --[[ share a prefix with their line comment.
        if (!L.blockStart.empty() && startsWith(s, i, L.blockStart)) continue;
        return true;
    }
    return false;
}

std::string lowerCopy(std::string_view w)
{
    std::string s(w);
    for (auto& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

Kind classifyWord(const Language& L, std::string_view w)
{
    std::string key(w);
    if (L.caseInsensitive) key = lowerCopy(w);
    if (L.keywords.count(key)) return Kind::Keyword;
    if (L.types.count(key)) return Kind::Type;
    if (L.constants.count(key)) return Kind::Number;
    return Kind::Default;
}

uint8_t tokenizeMarkdown(std::string_view s, Out& out, uint8_t state)
{
    const size_t n = s.size();
    size_t first = s.find_first_not_of(' ');
    bool fence = first != std::string_view::npos && s.substr(first, 3) == "```";
    if (fence)
    {
        out.push(0, n, Kind::Preproc);
        return state == StateBacktick ? StateNormal : StateBacktick;
    }
    if (state == StateBacktick)
    {
        out.push(0, n, Kind::String);
        return state;
    }
    if (first != std::string_view::npos && s[first] == '#') { out.push(0, n, Kind::Keyword); return state; }
    if (first != std::string_view::npos && (s[first] == '>' )) { out.push(0, n, Kind::Comment); return state; }
    size_t i = 0;
    while (i < n)
    {
        if (s[i] == '`')
        {
            size_t e = s.find('`', i + 1);
            e = (e == std::string_view::npos) ? n : e + 1;
            out.push(i, e, Kind::String);
            i = e;
            continue;
        }
        if (s[i] == '[')
        {
            size_t close = s.find("](", i);
            size_t end = close == std::string_view::npos ? close : s.find(')', close);
            if (end != std::string_view::npos)
            {
                out.push(i, close + 1, Kind::Function);
                out.push(close + 1, end + 1, Kind::Comment);
                i = end + 1;
                continue;
            }
        }
        if ((s[i] == '*' || s[i] == '_') && i + 1 < n && s[i + 1] == s[i])
        {
            size_t e = s.find(s.substr(i, 2), i + 2);
            if (e != std::string_view::npos)
            {
                out.push(i, e + 2, Kind::Type);
                i = e + 2;
                continue;
            }
        }
        out.push(i, i + 1, Kind::Default);
        ++i;
    }
    return state;
}

} // namespace

uint8_t tokenize(const Language& L, std::string_view s, uint8_t state, std::vector<Token>* tokens)
{
    if (tokens) tokens->clear();
    Out out{ tokens };
    const size_t n = s.size();

    if (L.id == "text") { out.push(0, n, Kind::Default); return StateNormal; }
    if (L.id == "markdown") return tokenizeMarkdown(s, out, state);

    size_t i = 0;
    // Continue what the previous line left open.
    if (state == StateBlockComment)
    {
        size_t e = L.blockEnd.empty() ? std::string_view::npos : s.find(L.blockEnd);
        if (e == std::string_view::npos) { out.push(0, n, Kind::Comment); return StateBlockComment; }
        out.push(0, e + L.blockEnd.size(), Kind::Comment);
        i = e + L.blockEnd.size();
    }
    else if (state == StateTripleDouble || state == StateTripleSingle)
    {
        const char* q = state == StateTripleDouble ? "\"\"\"" : "'''";
        size_t e = findUnescaped(s, 0, q);
        if (e == std::string_view::npos) { out.push(0, n, Kind::String); return state; }
        out.push(0, e + 3, Kind::String);
        i = e + 3;
    }
    else if (state == StateBacktick)
    {
        size_t e = findUnescaped(s, 0, "`");
        if (e == std::string_view::npos) { out.push(0, n, Kind::String); return state; }
        out.push(0, e + 1, Kind::String);
        i = e + 1;
    }

    // Preprocessor directives: "#include <x>" in the C family.
    size_t first = s.find_first_not_of(" \t");
    bool include = false;
    if (state == StateNormal && L.preprocessor && first != std::string_view::npos && s[first] == '#')
    {
        size_t j = first + 1;
        while (j < n && space(s[j])) ++j;
        size_t k = j;
        while (k < n && identChar(s[k])) ++k;
        std::string_view word = s.substr(j, k - j);
        include = word == "include" || word == "import";
        out.push(0, k, Kind::Preproc);
        i = k;
    }

    bool inTag = false;  // markup: between '<' and '>'
    while (i < n)
    {
        const char c = s[i];

        // Comments.
        if (!L.blockStart.empty() && startsWith(s, i, L.blockStart) &&
            !(L.id == "ruby" && i != 0))  // =begin only at the start of a line
        {
            size_t e = s.find(L.blockEnd, i + L.blockStart.size());
            if (e == std::string_view::npos) { out.push(i, n, Kind::Comment); return StateBlockComment; }
            out.push(i, e + L.blockEnd.size(), Kind::Comment);
            i = e + L.blockEnd.size();
            continue;
        }
        if (lineCommentAt(L, s, i)) { out.push(i, n, Kind::Comment); return StateNormal; }

        // Markup tags.
        if (L.markup)
        {
            if (c == '<' && i + 1 < n && (s[i + 1] == '/' || s[i + 1] == '!' || s[i + 1] == '?' || identStart(s[i + 1])))
            {
                size_t j = i + 1;
                if (s[j] == '/' || s[j] == '!' || s[j] == '?') ++j;
                size_t k = j;
                while (k < n && (identChar(s[k]) || s[k] == '-' || s[k] == ':' || s[k] == '.')) ++k;
                out.push(i, j, Kind::Punct);
                out.push(j, k, Kind::Keyword);
                inTag = true;
                i = k;
                continue;
            }
            if (inTag && c == '>') { out.push(i, i + 1, Kind::Punct); inTag = false; ++i; continue; }
            if (inTag && identStart(c))
            {
                size_t k = i;
                while (k < n && (identChar(s[k]) || s[k] == '-' || s[k] == ':')) ++k;
                out.push(i, k, Kind::Type);
                i = k;
                continue;
            }
            if (c == '&')
            {
                size_t e = s.find(';', i);
                if (e != std::string_view::npos && e - i < 10) { out.push(i, e + 1, Kind::Number); i = e + 1; continue; }
            }
            if (!inTag)  // text between tags: quotes there are apostrophes, not strings
            {
                out.push(i, i + 1, Kind::Default);
                ++i;
                continue;
            }
        }

        // Strings.
        if (L.tripleQuotes && (startsWith(s, i, "\"\"\"") || startsWith(s, i, "'''")))
        {
            bool dbl = c == '"';
            size_t e = findUnescaped(s, i + 3, dbl ? "\"\"\"" : "'''");
            if (e == std::string_view::npos) { out.push(i, n, Kind::String); return dbl ? StateTripleDouble : StateTripleSingle; }
            out.push(i, e + 3, Kind::String);
            i = e + 3;
            continue;
        }
        if (L.backtickStrings && c == '`')
        {
            size_t e = findUnescaped(s, i + 1, "`");
            if (e == std::string_view::npos) { out.push(i, n, Kind::String); return StateBacktick; }
            out.push(i, e + 1, Kind::String);
            i = e + 1;
            continue;
        }
        if (c == '"' || (c == '\'' && L.singleQuoteStrings))
        {
            if (c == '\'' && L.shortCharLiterals)
            {
                // 'a' or '\n' is a character; 'a on its own is a lifetime / prime.
                size_t e = scanQuoted(s, i, '\'');
                if (e == std::string_view::npos || e - i > (i + 1 < n && s[i + 1] == '\\' ? 12u : 4u))
                {
                    out.push(i, i + 1, Kind::Punct);
                    ++i;
                    continue;
                }
                out.push(i, e, Kind::String);
                i = e;
                continue;
            }
            size_t e = scanQuoted(s, i, c);
            if (e == std::string_view::npos) e = n;
            out.push(i, e, Kind::String);
            i = e;
            continue;
        }
        if (include && c == '<')
        {
            size_t e = s.find('>', i);
            e = (e == std::string_view::npos) ? n : e + 1;
            out.push(i, e, Kind::String);
            i = e;
            continue;
        }

        // Numbers.
        if (digit(c) || (c == '.' && i + 1 < n && digit(s[i + 1]) && (i == 0 || !identChar(s[i - 1]))))
        {
            size_t e = scanNumber(s, i);
            out.push(i, e, Kind::Number);
            i = e;
            continue;
        }

        // $variables, ${...}, $(...), @decorators.
        if (L.dollarVariables && c == '$' && i + 1 < n)
        {
            size_t e = i + 1;
            if (s[e] == '{' || s[e] == '(')
            {
                size_t close = s.find(s[e] == '{' ? '}' : ')', e);
                e = close == std::string_view::npos ? n : close + 1;
            }
            else
            {
                while (e < n && (identChar(s[e]) || (e == i + 1 && std::strchr("#?@*!$0123456789", s[e])))) ++e;
            }
            if (e > i + 1) { out.push(i, e, Kind::Preproc); i = e; continue; }
        }
        if (L.decorators && c == '@' && i + 1 < n && identStart(s[i + 1]))
        {
            size_t e = i + 1;
            while (e < n && (identChar(s[e]) || s[e] == '.')) ++e;
            out.push(i, e, Kind::Preproc);
            i = e;
            continue;
        }

        // Words.
        if (identStart(c) || (L.lispy && std::strchr("+-*/<>=!?", c) && i + 1 < n && !space(s[i + 1]) && s[i + 1] != ')'))
        {
            size_t e = i + 1;
            while (e < n && (identChar(s[e]) || (L.lispy && std::strchr("-?!*<>=/.", s[e])) ||
                             (L.id == "ruby" && (s[e] == '?' || s[e] == '!') && e + 1 < n && !identChar(s[e + 1]))))
                ++e;
            std::string_view w = s.substr(i, e - i);
            // String prefixes such as r"raw" or f"{x}".
            if (!L.stringPrefixes.empty() && e < n && (s[e] == '"' || s[e] == '\'') && w.size() <= 3 &&
                std::all_of(w.begin(), w.end(), [&](char ch) { return L.stringPrefixes.find(ch) != std::string::npos; }))
            {
                if (L.tripleQuotes && (startsWith(s, e, "\"\"\"") || startsWith(s, e, "'''")))
                {
                    bool dbl = s[e] == '"';
                    size_t end = findUnescaped(s, e + 3, dbl ? "\"\"\"" : "'''");
                    if (end == std::string_view::npos) { out.push(i, n, Kind::String); return dbl ? StateTripleDouble : StateTripleSingle; }
                    out.push(i, end + 3, Kind::String);
                    i = end + 3;
                    continue;
                }
                size_t end = scanQuoted(s, e, s[e]);
                if (end == std::string_view::npos) end = n;
                out.push(i, end, Kind::String);
                i = end;
                continue;
            }
            Kind k = classifyWord(L, w);
            if (k == Kind::Default)
            {
                size_t j = e;
                while (j < n && s[j] == ' ') ++j;
                if (L.lispy && i > 0 && s[i - 1] == '(') k = Kind::Function;
                else if (j < n && s[j] == '(') k = Kind::Function;
                else if (j < n && s[j] == '!' && L.id == "rust" && (j + 1 >= n || s[j + 1] != '=')) k = Kind::Function;
                else if (L.braces && w.size() > 1 && w[0] >= 'A' && w[0] <= 'Z' &&
                         std::any_of(w.begin(), w.end(), [](char ch) { return ch >= 'a' && ch <= 'z'; }))
                    k = Kind::Type;  // CamelCase names are usually types
                else if (L.lispy && w[0] == ':') k = Kind::Number;
            }
            out.push(i, e, k);
            i = e;
            continue;
        }
        if (L.lispy && c == ':' && i + 1 < n && identStart(s[i + 1]))  // :keyword
        {
            size_t e = i + 1;
            while (e < n && (identChar(s[e]) || s[e] == '-' || s[e] == '/')) ++e;
            out.push(i, e, Kind::Number);
            i = e;
            continue;
        }
        if (std::strchr("{}()[];,.<>=+-*/%&|^!~?:", c))
        {
            out.push(i, i + 1, Kind::Punct);
            ++i;
            continue;
        }
        out.push(i, i + 1, Kind::Default);
        ++i;
    }
    return StateNormal;
}

} // namespace syntax
