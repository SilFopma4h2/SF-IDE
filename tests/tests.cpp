// Unit tests for the parts of SF-IDE that do not need a window:
// the text buffer, the terminal emulator, the pty, the tokenizer and the
// compiler-diagnostic parser. Run with `ctest` or ./sfide_tests.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "BuildRunner.h"
#include "Document.h"
#include "IdeBridge.h"
#include "Pty.h"
#include "Syntax.h"
#include "IdeUtil.h"
#include "Languages.h"
#include "Vt.h"

namespace fs = std::filesystem;

void wakeUi(int) {}  // the GUI event loop is not part of the tests

static int g_failed = 0, g_passed = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (cond) ++g_passed;                                                       \
        else { ++g_failed; std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); } \
    } while (0)

#define CHECK_EQ(a, b)                                                              \
    do {                                                                            \
        auto va = (a); auto vb = (b);                                               \
        if (va == vb) ++g_passed;                                                   \
        else { ++g_failed; std::fprintf(stderr, "%s:%d: %s == %s failed\n", __FILE__, __LINE__, #a, #b); } \
    } while (0)

static std::string tmpPath(const char* name)
{
    return (fs::temp_directory_path() / (std::string("sfide-test-") + std::to_string(getpid()) + "-" + name)).string();
}

static std::string screenRow(const Vt& vt, int row)
{
    std::string s;
    for (const auto& c : vt.line(vt.scrollbackLines() + row))
    {
        if (c.ch == 0) continue;
        char buf[8];
        uint32_t cp = c.ch;
        if (cp < 0x80) { s.push_back(char(cp)); continue; }
        // minimal UTF-8 encode for the test
        if (cp < 0x800) { buf[0] = char(0xC0 | (cp >> 6)); buf[1] = char(0x80 | (cp & 0x3F)); s.append(buf, 2); }
        else if (cp < 0x10000) { buf[0] = char(0xE0 | (cp >> 12)); buf[1] = char(0x80 | ((cp >> 6) & 0x3F)); buf[2] = char(0x80 | (cp & 0x3F)); s.append(buf, 3); }
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

static void testDocument()
{
    std::string path = tmpPath("doc.cpp");
    writeTextFile(path, "int main()\r\n{\r\n    return 0;\r\n}\r\n");
    Document d;
    CHECK(d.load(path));
    CHECK_EQ(d.lineCount(), 5);
    CHECK(d.text().find('\r') == std::string::npos);
    CHECK_EQ(d.lang().id, std::string("cpp"));
    CHECK_EQ(std::string(d.lineText(2)), std::string("    return 0;"));
    CHECK_EQ(d.lineOf(d.lineStart(3)), 3);

    // Insert and undo; typing is grouped into one undo step.
    d.setCursor(d.lineEnd(0), false);
    d.insert("/", Document::EditKind::Insert);
    d.insert("/", Document::EditKind::Insert);
    d.insert(" x", Document::EditKind::Insert);
    CHECK_EQ(std::string(d.lineText(0)), std::string("int main()// x"));
    CHECK(d.dirty());
    d.undo();
    CHECK_EQ(std::string(d.lineText(0)), std::string("int main()"));
    CHECK(!d.dirty());  // back at the saved text
    d.redo();
    CHECK_EQ(std::string(d.lineText(0)), std::string("int main()// x"));

    // Breakpoints follow inserted lines.
    d.breakpoints = { 3 };
    d.setCursor(0, false);
    d.insert("// header\n", Document::EditKind::Other);
    CHECK(d.breakpoints.count(4) == 1);

    // CRLF is kept on save.
    CHECK(d.save());
    std::string raw;
    readTextFile(path, raw);
    CHECK(raw.find("\r\n") != std::string::npos);
    CHECK(!d.dirty());

    // External change detection.
    CHECK(!d.changedOnDisk());
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    writeTextFile(path, "changed\n");
    CHECK(d.changedOnDisk());
    CHECK(d.reloadFromDisk());
    CHECK_EQ(std::string(d.lineText(0)), std::string("changed"));
    CHECK(d.canUndo());  // a reload can be undone

    // Tabs and columns.
    Document t;
    t.replace(0, 0, "\tab\tc");
    CHECK_EQ(t.columnOf(0, 1), 4);
    CHECK_EQ(t.columnOf(0, 4), 8);
    CHECK_EQ(t.offsetAtColumn(0, 4), size_t(1));
    CHECK_EQ(t.offsetAtColumn(0, 1), size_t(0));
    CHECK_EQ(t.offsetAtColumn(0, 3), size_t(1));

    // UTF-8 aware movement.
    Document u;
    u.replace(0, 0, "h\xC3\xA9llo");
    CHECK_EQ(u.nextChar(1), size_t(3));
    CHECK_EQ(u.prevChar(3), size_t(1));
    CHECK_EQ(u.columnOf(0, 3), 2);

    // Word movement.
    Document w;
    w.replace(0, 0, "foo_bar baz(qux)");
    CHECK_EQ(w.wordRight(0), size_t(7));
    CHECK_EQ(w.wordLeft(11), size_t(8));

    // Block comment state.
    Document c;
    std::string cpath = tmpPath("c.c");
    writeTextFile(cpath, "a /* b\nc\nd */ e\nf \"/*\" g\nh\n");
    c.load(cpath);
    const auto& st = c.lineStates();
    CHECK_EQ(int(st[1]), 1);
    CHECK_EQ(int(st[2]), 1);
    CHECK_EQ(int(st[3]), 0);
    CHECK_EQ(int(st[4]), 0);  // "/*" inside a string does not open a comment

    std::remove(path.c_str());
    std::remove(cpath.c_str());
}

static void testVt()
{
    Vt vt(20, 5);
    const char* s = "hello\r\nworld";
    vt.feed(s, std::strlen(s));
    CHECK_EQ(screenRow(vt, 0), std::string("hello"));
    CHECK_EQ(screenRow(vt, 1), std::string("world"));
    CHECK_EQ(vt.cursorX(), 5);
    CHECK_EQ(vt.cursorY(), 1);

    // Colours.
    const char* c = "\x1b[1;31mR\x1b[0m\x1b[38;5;208mO\x1b[38;2;1;2;3mT";
    vt.feed(c, std::strlen(c));
    const auto& row = vt.line(vt.scrollbackLines() + 1);
    CHECK(row[5].fg == Vt::colorIndex(1) && (row[5].attr & Vt::AttrBold));
    CHECK(row[6].fg == Vt::colorIndex(208));
    CHECK(row[7].fg == Vt::colorRgb(1, 2, 3));

    // Erase line, cursor movement (what clil's line editor sends).
    const char* e = "\r\x1b[Kab\x1b[2Dz";
    vt.feed(e, std::strlen(e));
    CHECK_EQ(screenRow(vt, 1), std::string("zb"));

    // Scrolling into scrollback.
    Vt sb(10, 3);
    for (int i = 0; i < 6; ++i)
    {
        std::string l = "L" + std::to_string(i) + "\r\n";
        sb.feed(l.data(), l.size());
    }
    CHECK_EQ(sb.scrollbackLines(), 4);
    CHECK_EQ(screenRow(sb, 0), std::string("L4"));

    // Auto-wrap with deferred wrap.
    Vt wr(4, 3);
    wr.feed("abcdef", 6);
    CHECK_EQ(screenRow(wr, 0), std::string("abcd"));
    CHECK_EQ(screenRow(wr, 1), std::string("ef"));

    // UTF-8 + clil's logo blocks and a wide character.
    Vt u(10, 2);
    const char* logo = "\xE2\x96\x80\xE2\x96\x84 \xE4\xBD\xA0!";
    u.feed(logo, std::strlen(logo));
    CHECK_EQ(u.line(0)[0].ch, uint32_t(0x2580));
    CHECK_EQ(u.line(0)[1].ch, uint32_t(0x2584));
    CHECK_EQ(u.line(0)[3].ch, uint32_t(0x4F60));
    CHECK_EQ(u.line(0)[4].ch, uint32_t(0));  // right half of the wide glyph
    CHECK_EQ(u.line(0)[5].ch, uint32_t('!'));

    // Cursor position report.
    Vt q(10, 5);
    q.feed("ab\x1b[6n", 6);
    CHECK_EQ(q.takeReplies(), std::string("\x1b[1;3R"));

    // Alternate screen keeps the main screen.
    Vt alt(10, 3);
    alt.feed("main", 4);
    const char* a = "\x1b[?1049hALT\x1b[?1049l";
    alt.feed(a, std::strlen(a));
    CHECK_EQ(screenRow(alt, 0), std::string("main"));

    // Bracketed paste mode toggles.
    Vt bp(10, 3);
    bp.feed("\x1b[?2004h", 8);
    CHECK(bp.bracketedPaste());
    bp.feed("\x1b[?2004l", 8);
    CHECK(!bp.bracketedPaste());

    // Resize keeps content.
    Vt rs(10, 4);
    rs.feed("one\r\ntwo\r\nthree", 15);
    rs.resize(6, 2);
    CHECK_EQ(screenRow(rs, 1), std::string("three"));
    rs.resize(12, 4);
    CHECK_EQ(screenRow(rs, 0), std::string("one"));
}

static void testPty()
{
    Pty p;
    CHECK(p.start({ "/bin/sh", "-c", "printf 'size=%s\\n' \"$(stty size)\"; read x; echo got:$x" },
                  "/", 77, 20));
    p.write("hi\r");
    std::string all, chunk;
    for (int i = 0; i < 200 && (p.running() || all.find("got:") == std::string::npos); ++i)
    {
        if (p.take(chunk)) all += chunk;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (p.take(chunk)) all += chunk;
    CHECK(all.find("size=20 77") != std::string::npos);
    CHECK(all.find("got:hi") != std::string::npos);
    p.stop();
    CHECK(!p.running());
}

static bool hasKind(const std::vector<syntax::Token>& toks, const std::string& line, syntax::Kind k, const char* text)
{
    for (auto& t : toks)
        if (t.kind == k && line.substr(t.begin, t.end - t.begin) == text) return true;
    return false;
}

static void testSyntax()
{
    const Language& cpp = languageForFile("a.cpp");
    std::vector<syntax::Token> toks;
    std::string l1 = "#include <vector> // c";
    syntax::tokenize(cpp, l1, syntax::StateNormal, &toks);
    CHECK(!toks.empty() && toks[0].kind == syntax::Kind::Preproc);
    CHECK(hasKind(toks, l1, syntax::Kind::String, "<vector>"));
    CHECK(hasKind(toks, l1, syntax::Kind::Comment, "// c"));

    std::string l2 = "return foo(1.5e-3);";
    syntax::tokenize(cpp, l2, syntax::StateNormal, &toks);
    CHECK(toks[0].kind == syntax::Kind::Keyword);
    CHECK(hasKind(toks, l2, syntax::Kind::Function, "foo"));
    CHECK(hasKind(toks, l2, syntax::Kind::Number, "1.5e-3"));
    // Tokens cover the whole line without gaps.
    uint32_t end = 0;
    for (auto& t : toks) { CHECK_EQ(t.begin, end); end = t.end; }
    CHECK_EQ(end, uint32_t(l2.size()));
    CHECK_EQ(int(syntax::tokenize(cpp, "x /* open", syntax::StateNormal, nullptr)), int(syntax::StateBlockComment));
    std::string l3 = "still comment */ int x;";
    CHECK_EQ(int(syntax::tokenize(cpp, l3, syntax::StateBlockComment, &toks)), int(syntax::StateNormal));
    CHECK(toks[0].kind == syntax::Kind::Comment);

    // Python: keywords, decorators, f-strings, triple-quoted strings over lines, '#' comments.
    const Language& py = languageForFile("x.py");
    std::string p1 = "@cache";
    syntax::tokenize(py, p1, syntax::StateNormal, &toks);
    CHECK(hasKind(toks, p1, syntax::Kind::Preproc, "@cache"));
    std::string p2 = "def f(x): return f\"{x}\" if x else None  # done";
    syntax::tokenize(py, p2, syntax::StateNormal, &toks);
    CHECK(hasKind(toks, p2, syntax::Kind::Keyword, "def"));
    CHECK(hasKind(toks, p2, syntax::Kind::String, "f\"{x}\""));
    CHECK(hasKind(toks, p2, syntax::Kind::Number, "None"));
    CHECK(hasKind(toks, p2, syntax::Kind::Comment, "# done"));
    CHECK_EQ(int(syntax::tokenize(py, "s = \"\"\"doc", syntax::StateNormal, nullptr)), int(syntax::StateTripleDouble));
    CHECK_EQ(int(syntax::tokenize(py, "end\"\"\" + x", syntax::StateTripleDouble, nullptr)), int(syntax::StateNormal));

    // JavaScript template strings may span lines; Rust lifetimes are not strings.
    const Language& js = languageForFile("app.js");
    CHECK_EQ(int(syntax::tokenize(js, "const s = `a ${b}", syntax::StateNormal, nullptr)), int(syntax::StateBacktick));
    const Language& rs = languageForFile("main.rs");
    std::string r1 = "fn f<'a>(x: &'a str) -> char { println!(\"hi\"); 'x' }";
    syntax::tokenize(rs, r1, syntax::StateNormal, &toks);
    CHECK(hasKind(toks, r1, syntax::Kind::String, "'x'"));
    CHECK(hasKind(toks, r1, syntax::Kind::Type, "str"));
    CHECK(hasKind(toks, r1, syntax::Kind::Function, "println"));

    // Shell variables, Lua block comments, HTML tags, SQL keywords in any case.
    const Language& sh = languageForFile("run.sh");
    std::string s1 = "echo \"$HOME\" ${PATH} $1 # note";
    syntax::tokenize(sh, s1, syntax::StateNormal, &toks);
    CHECK(hasKind(toks, s1, syntax::Kind::Preproc, "${PATH}"));
    CHECK(hasKind(toks, s1, syntax::Kind::Comment, "# note"));
    const Language& lua = languageForFile("a.lua");
    CHECK_EQ(int(syntax::tokenize(lua, "--[[ start", syntax::StateNormal, nullptr)), int(syntax::StateBlockComment));
    std::string lu = "local x = 1 -- one";
    syntax::tokenize(lua, lu, syntax::StateNormal, &toks);
    CHECK(hasKind(toks, lu, syntax::Kind::Comment, "-- one"));
    const Language& html = languageForFile("index.html");
    std::string h1 = "<a href=\"x\">it's</a>";
    syntax::tokenize(html, h1, syntax::StateNormal, &toks);
    CHECK(hasKind(toks, h1, syntax::Kind::Keyword, "a"));
    CHECK(hasKind(toks, h1, syntax::Kind::Type, "href"));
    CHECK(hasKind(toks, h1, syntax::Kind::String, "\"x\""));
    const Language& sql = languageForFile("q.sql");
    std::string q1 = "SELECT name FROM users -- all";
    syntax::tokenize(sql, q1, syntax::StateNormal, &toks);
    CHECK(hasKind(toks, q1, syntax::Kind::Keyword, "SELECT"));
}

static void testLanguages()
{
    auto id = [](const char* path, const char* first = "") { return languageForFile(path, first).id; };
    CHECK_EQ(id("/a/main.c"), std::string("c"));
    CHECK_EQ(id("x.hpp"), std::string("cpp"));
    CHECK_EQ(id("x.h"), std::string("cpp"));
    CHECK_EQ(id("script.py"), std::string("python"));
    CHECK_EQ(id("App.tsx"), std::string("typescript"));
    CHECK_EQ(id("main.go"), std::string("go"));
    CHECK_EQ(id("lib.rs"), std::string("rust"));
    CHECK_EQ(id("Main.java"), std::string("java"));
    CHECK_EQ(id("Program.cs"), std::string("csharp"));
    CHECK_EQ(id("a.swift"), std::string("swift"));
    CHECK_EQ(id("a.kt"), std::string("kotlin"));
    CHECK_EQ(id("a.rb"), std::string("ruby"));
    CHECK_EQ(id("a.php"), std::string("php"));
    CHECK_EQ(id("a.lua"), std::string("lua"));
    CHECK_EQ(id("a.hs"), std::string("haskell"));
    CHECK_EQ(id("a.ex"), std::string("elixir"));
    CHECK_EQ(id("Makefile"), std::string("make"));
    CHECK_EQ(id("CMakeLists.txt"), std::string("cmake"));
    CHECK_EQ(id("Dockerfile"), std::string("docker"));
    CHECK_EQ(id("notes.md"), std::string("markdown"));
    CHECK_EQ(id("data.JSON"), std::string("json"));
    CHECK_EQ(id("tool", "#!/usr/bin/env python3"), std::string("python"));
    CHECK_EQ(id("tool", "#!/usr/bin/env -S node --no-warnings"), std::string("javascript"));
    CHECK_EQ(id("tool", "#!/bin/bash"), std::string("shell"));
    CHECK_EQ(id("README"), std::string("text"));
    CHECK(allLanguages().size() >= 45);
    // Every language that can be run has a command that uses the file.
    for (const auto& L : allLanguages())
    {
        if (!L.run.empty()) CHECK(L.run.find("{file}") != std::string::npos || L.run.find("{out}") != std::string::npos);
        if (L.compiled) CHECK(L.build.find("{out}") != std::string::npos);
    }
}

static void testDiagnostics()
{
    Problem p;
    CHECK(BuildRunner::parseDiagnostic("src/main.cpp:12:5: error: expected ';' after expression", "/proj", p));
    CHECK_EQ(p.line, 12);
    CHECK_EQ(p.col, 5);
    CHECK(p.error);
    CHECK_EQ(p.file, std::string("/proj/src/main.cpp"));
    CHECK_EQ(p.message, std::string("expected ';' after expression"));

    CHECK(BuildRunner::parseDiagnostic("/abs/x.c:3: warning: unused variable 'y'", "/proj", p));
    CHECK_EQ(p.line, 3);
    CHECK_EQ(p.col, 0);
    CHECK(!p.error);

    CHECK(!BuildRunner::parseDiagnostic("[ 50%] Building CXX object", "/proj", p));
    CHECK(!BuildRunner::parseDiagnostic("x.cpp:1:2: note: declared here", "/proj", p));

    // TypeScript / C#.
    CHECK(BuildRunner::parseDiagnostic("src/a.ts(3,7): error TS2304: Cannot find name 'x'.", "/p", p));
    CHECK_EQ(p.file, std::string("/p/src/a.ts"));
    CHECK_EQ(p.line, 3);
    CHECK_EQ(p.col, 7);
    CHECK_EQ(p.message, std::string("Cannot find name 'x'."));
    CHECK(BuildRunner::parseDiagnostic("Program.cs(10,5): warning CS0168: The variable 'e' is declared", "/p", p));
    CHECK(!p.error);
    // PHP and bash.
    CHECK(BuildRunner::parseDiagnostic("PHP Parse error:  syntax error, unexpected end of file in /p/a.php on line 7", "/", p));
    CHECK_EQ(p.file, std::string("/p/a.php"));
    CHECK_EQ(p.line, 7);
    CHECK_EQ(p.message, std::string("syntax error, unexpected end of file"));
    CHECK(BuildRunner::parseDiagnostic("run.sh: line 4: syntax error near unexpected token `fi'", "/p", p));
    CHECK_EQ(p.line, 4);

    // Multi-line formats through the parser.
    std::string dir = tmpPath("diag");
    fs::create_directories(dir);
    writeTextFile(dir + "/a.py", "x = 1\n");
    writeTextFile(dir + "/main.go", "package main\n");
    {
        DiagnosticParser dp(dir);
        Problem q;
        CHECK(!dp.feed("Traceback (most recent call last):", q));
        CHECK(!dp.feed("  File \"" + dir + "/a.py\", line 1, in <module>", q));
        CHECK(!dp.feed("    x = 1 / 0", q));
        CHECK(dp.feed("ZeroDivisionError: division by zero", q));
        CHECK_EQ(q.line, 1);
        CHECK(q.file.find("a.py") != std::string::npos);
        CHECK_EQ(q.message, std::string("ZeroDivisionError: division by zero"));
    }
    {
        DiagnosticParser dp(dir);
        Problem q;
        CHECK(!dp.feed("error[E0425]: cannot find value `y` in this scope", q));
        CHECK(dp.feed("  --> src/main.rs:3:13", q));
        CHECK_EQ(q.line, 3);
        CHECK_EQ(q.col, 13);
        CHECK_EQ(q.message, std::string("cannot find value `y` in this scope"));
    }
    writeTextFile(dir + "/a.rb", "x\n");
    writeTextFile(dir + "/a.pl", "x\n");
    {
        // Ruby prefixes its name; Perl says "at FILE line N".
        Problem q;
        CHECK(BuildRunner::parseDiagnostic("ruby: " + dir + "/a.rb:2: syntax error, unexpected end-of-input", dir, q));
        CHECK_EQ(q.line, 2);
        CHECK(BuildRunner::parseDiagnostic("syntax error at " + dir + "/a.pl line 1, near \"= ;\"", dir, q));
        CHECK_EQ(q.line, 1);
        CHECK_EQ(q.message, std::string("syntax error"));
    }
    {
        // Go reports "path:line:col: message" without a severity word.
        DiagnosticParser dp(dir);
        Problem q;
        CHECK(dp.feed("./main.go:5:2: undefined: fmt", q));
        CHECK_EQ(q.line, 5);
        CHECK(q.error);
    }
    fs::remove_all(dir);
}

static void testBuildRunner()
{
    std::string dir = tmpPath("build");
    fs::create_directories(dir);
    writeTextFile(dir + "/bad.c", "int main(void) { return x; }\n");
    BuildRunner b;
    CHECK(b.start("cc -c bad.c -o bad.o", dir));
    for (int i = 0; i < 500 && b.running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::vector<BuildRunner::Line> lines;
    std::vector<Problem> problems;
    b.poll(lines, problems);
    CHECK(b.state() == BuildRunner::State::Failed);
    CHECK(!problems.empty());
    if (!problems.empty())
    {
        CHECK_EQ(problems[0].line, 1);
        CHECK(problems[0].file.find("bad.c") != std::string::npos);
    }
    fs::remove_all(dir);
}

static void testBridge()
{
    // Request parsing, including escapes and keys we ignore.
    IdeBridge::Request r;
    CHECK(IdeBridge::parseRequest("{\"op\":\"write\",\"n\":3,\"x\":{\"a\":[1,\"}\"]},\"path\":\"/a b\","
                                  "\"text\":\"l1\\n\\\"q\\\" \\u00e9 \\ud83d\\ude00\"}\n", r));
    CHECK_EQ(r.op, std::string("write"));
    CHECK_EQ(r.path, std::string("/a b"));
    CHECK_EQ(r.text, std::string("l1\n\"q\" \xC3\xA9 \xF0\x9F\x98\x80"));
    IdeBridge::Request bad;
    CHECK(!IdeBridge::parseRequest("not json", bad));
    CHECK_EQ(IdeBridge::quote("a\"b\n\x01"), std::string("\"a\\\"b\\n\\u0001\""));

    // External edits keep the user's cursor and only touch the changed part.
    Document d;
    d.replace(0, 0, "one\ntwo\nthree\nfour\n");
    d.setCursor(d.lineStart(3) + 2, false);  // "fo|ur"
    int line = d.applyExternal("one\nTWO!\nthree\nfour\n");
    CHECK_EQ(line, 2);
    CHECK_EQ(d.lineOf(d.cursor), 3);
    CHECK_EQ(d.cursor - d.lineStart(3), size_t(2));
    CHECK_EQ(d.flashFrom, 2);
    d.undo();
    CHECK_EQ(std::string(d.lineText(1)), std::string("two"));
    CHECK_EQ(d.applyExternal(d.text()), 0);

    // Round trip over the socket, like clil does.
    IdeBridge b;
    CHECK(b.start());
    std::thread client([&] {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un a = {};
        a.sun_family = AF_UNIX;
        std::strcpy(a.sun_path, b.socketPath().c_str());
        if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0)
        {
            std::string req = "{\"op\":\"state\"}\n";
            (void)!write(fd, req.data(), req.size());
            char buf[256] = {};
            (void)!read(fd, buf, sizeof buf - 1);
            CHECK_EQ(std::string(buf), std::string("{\"ok\":true}\n"));
        }
        close(fd);
    });
    IdeBridge::Request got;
    bool have = false;
    for (int i = 0; i < 300 && !have; ++i)
    {
        have = b.next(got);
        if (!have) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(have && got.op == "state");
    if (have) IdeBridge::reply(got, "{\"ok\":true}");
    client.join();
    b.stop();
}

static void testUtil()
{
    CHECK_EQ(shellQuote("it's"), std::string("'it'\\''s'"));
    CHECK_EQ(relativePath("/a/b/c.txt", "/a"), std::string("b/c.txt"));
    CHECK_EQ(relativePath("/x/c.txt", "/a"), std::string("/x/c.txt"));
    CHECK(!findProgram("sh").empty());
}

int main()
{
    testDocument();
    testVt();
    testPty();
    testSyntax();
    testLanguages();
    testDiagnostics();
    testBuildRunner();
    testBridge();
    testUtil();
    std::printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
