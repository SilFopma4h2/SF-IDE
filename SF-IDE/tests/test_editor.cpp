// Minimal unit test for TextEdit: exercises load/save, cursor, selection,
// undo/redo, and line math. Returns 0 on success, non-zero on failure.

#include "../src/Editor.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); std::exit(1); } \
} while (0)

static void writeFile(const std::string& path, const std::string& contents)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

int main()
{
    const std::string path = "test_editor_temp.cpp";
    writeFile(path, "int main() {\n    return 0;\n}\n");

    TextEdit e;
    CHECK(e.loadFromFile(path));
    CHECK(e.lineCount() == 4);
    CHECK(e.buffer.find("return 0;") != std::string::npos);
    CHECK(!e.dirty);

    // Cursor: append text
    e.moveCursor(e.buffer.size(), false);
    e.insertText("/* tail */");
    CHECK(e.dirty);
    CHECK(e.buffer.find("/* tail */") != std::string::npos);

    // Undo
    e.undoStep();
    CHECK(e.buffer.find("/* tail */") == std::string::npos);

    // Redo
    e.redoStep();
    CHECK(e.buffer.find("/* tail */") != std::string::npos);

    // Selection delete
    e.moveCursor(0, false);
    e.moveCursor(e.buffer.size(), true);
    CHECK(e.hasSelection());
    e.deleteSelection();
    CHECK(e.buffer.empty());
    CHECK(!e.hasSelection());

    // Line/col round trip
    int line, col;
    e.offsetToLineCol(0, line, col);
    CHECK(line == 1 && col == 1);

    e.buffer = "abc\ndef\nghi";
    e.offsetToLineCol(5, line, col); // 'e' of "def"
    CHECK(line == 2 && col == 2);
    CHECK(e.lineColToOffset(2, 2) == 5);
    CHECK(e.lineColToOffset(3, 1) == 8);
    CHECK(e.lineCount() == 3);

    // Save round trip
    e.moveCursor(0, false);
    e.insertText("X"); // makes "Xabc\ndef\nghi"
    const std::string outPath = "test_editor_out.cpp";
    CHECK(e.saveAs(outPath));
    CHECK(!e.dirty);

    TextEdit e2;
    CHECK(e2.loadFromFile(outPath));
    CHECK(e2.buffer == "Xabc\ndef\nghi");

    // Cleanup
    std::remove(path.c_str());
    std::remove(outPath.c_str());
    std::printf("OK\n");
    return 0;
}
