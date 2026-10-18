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
#include <unistd.h>

#include "BuildRunner.h"
#include "Document.h"
#include "Pty.h"
#include "Syntax.h"
#include "IdeUtil.h"
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
    CHECK(d.lang() == Document::Lang::CFamily);
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
    const auto& st = c.blockCommentState();
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
    CHECK(p.start({ "/bin/sh", "-c", "printf 'cols=%s\\n' \"$(tput cols 2>/dev/null || stty size | cut -d' ' -f2)\"; read x; echo got:$x" },
                  "/", 77, 20));
    p.write("hi\r");
    std::string all, chunk;
    for (int i = 0; i < 200 && (p.running() || all.find("got:") == std::string::npos); ++i)
    {
        if (p.take(chunk)) all += chunk;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (p.take(chunk)) all += chunk;
    CHECK(all.find("cols=77") != std::string::npos);
    CHECK(all.find("got:hi") != std::string::npos);
    p.stop();
    CHECK(!p.running());
}

static void testSyntax()
{
    std::vector<syntax::Token> toks;
    syntax::tokenize(Document::Lang::CFamily, "#include <vector> // c", false, toks);
    CHECK(!toks.empty() && toks[0].kind == syntax::Kind::Preproc);
    bool sawString = false, sawComment = false;
    for (auto& t : toks)
    {
        if (t.kind == syntax::Kind::String) sawString = true;
        if (t.kind == syntax::Kind::Comment) sawComment = true;
    }
    CHECK(sawString && sawComment);

    syntax::tokenize(Document::Lang::CFamily, "return foo(1.5e-3);", false, toks);
    CHECK(toks[0].kind == syntax::Kind::Keyword);
    bool fn = false, num = false;
    for (auto& t : toks)
    {
        if (t.kind == syntax::Kind::Function) fn = true;
        if (t.kind == syntax::Kind::Number) num = true;
    }
    CHECK(fn && num);
    // Tokens cover the whole line without gaps.
    uint32_t end = 0;
    for (auto& t : toks) { CHECK_EQ(t.begin, end); end = t.end; }
    CHECK_EQ(end, uint32_t(std::strlen("return foo(1.5e-3);")));

    syntax::tokenize(Document::Lang::CFamily, "still comment */ int x;", true, toks);
    CHECK(toks[0].kind == syntax::Kind::Comment);
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
    testDiagnostics();
    testBuildRunner();
    testUtil();
    std::printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
