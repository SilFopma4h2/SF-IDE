// Languages - what SF-IDE knows about each programming language: how to
// recognise its files, how to highlight it, how comments and indentation
// work, and how to check, run and debug a single file of it.
//
// Command templates run in the user's login shell with these placeholders:
//   {file}  the source file (quoted)     {dir}  its directory (quoted)
//   {stem}  file name without extension  {out}  a private output path (quoted)

#pragma once

#include <string>
#include <unordered_set>
#include <vector>

struct Language {
    std::string id;                          // "python", "cpp", ...
    std::string name;                        // shown in the status bar
    std::vector<std::string> extensions;     // lower case, without the dot
    std::vector<std::string> filenames;      // exact names: Makefile, Dockerfile
    std::vector<std::string> shebangs;       // interpreters named on a #! line

    // Comments and strings
    std::string lineComment;                 // "//", "#", "--", ";" ...
    std::string lineComment2;                // a second style, e.g. "#" in PHP
    std::string blockStart, blockEnd;        // "/*" "*/", "<!--" "-->", "{-" "-}"
    bool tripleQuotes = false;               // """ and ''' strings (Python, Kotlin, Swift ...)
    bool backtickStrings = false;            // `...` strings, may span lines (JS, Go)
    bool singleQuoteStrings = true;          // 'x' is a string / char literal
    bool shortCharLiterals = false;          // 'x' only when it closes within 3 chars (Rust lifetimes)
    std::string stringPrefixes;              // letters that may prefix a string: r"..", f".."
    bool preprocessor = false;               // #include / #define lines (C family)
    bool markup = false;                     // <tags> (HTML, XML)
    bool caseInsensitive = false;            // keywords in any case (SQL, Fortran)
    bool dollarVariables = false;            // $name (shell, PHP, Perl)
    bool decorators = false;                 // @name (Python, Java, TypeScript ...)
    bool lispy = false;                      // ( ... ) code, '-' in names

    // Editing
    bool indentAfterColon = false;           // Python, Nim, YAML
    bool braces = true;                      // { } blocks: indent after '{'
    bool useTabs = false;                    // Makefile, Go

    std::unordered_set<std::string> keywords, types, constants;

    // Single-file workflow
    std::string build;                       // compile to {out} or check the file; may be empty
    std::string run;                         // run it ({out} for compiled languages)
    bool compiled = false;                   // run needs a successful build first
    enum class Debugger { None, Lldb, Pdb } debugger = Debugger::None;
};

// The language for a file, by name, extension, then the #! line.
const Language& languageForFile(const std::string& path, const std::string& firstLine = {});
const Language& plainText();
const std::vector<Language>& allLanguages();
