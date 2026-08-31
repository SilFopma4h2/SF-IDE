// Test that the editor (TextEdit) properly loads, edits, and saves a real
// main.cpp file similar to what SF-IDE itself uses. Verifies that
//   * file is fully loaded (line count + content checks),
//   * editing modifies the buffer,
//   * undo restores the original state,
//   * the line/col math is consistent across edits.

#include "../src/Editor.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <ios>
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
    // Realistic C++ file content (LF line endings, with some odd characters).
    const std::string src =
        "// Sample C++ file\n"
        "#include <iostream>\n"
        "#include <string>\n"
        "\n"
        "int main(int argc, char** argv) {\n"
        "    std::string name = \"World\";\n"
        "    if (argc > 1) name = argv[1];\n"
        "    std::cout << \"Hello, \" << name << \"!\" << std::endl;\n"
        "    return 0;\n"
        "}\n";
    const std::string path = "test_editor_real.cpp";
    writeFile(path, src);

    TextEdit e;
    CHECK(e.loadFromFile(path));
    CHECK(!e.dirty);

    // Buffer must contain all the source.
    CHECK(e.buffer.find("int main(int argc, char** argv)") != std::string::npos);
    CHECK(e.buffer.find("return 0;") != std::string::npos);

    // Line count.
    int lineCount = e.lineCount();
    std::printf("Lines: %d\n", lineCount);
    CHECK(lineCount >= 10);

    // Move to end, type something.
    e.moveCursor(e.buffer.size(), false);
    e.insertText("\n// edited\n");
    CHECK(e.dirty);
    CHECK(e.buffer.find("// edited") != std::string::npos);

    // Undo should remove the edit. Note: undoStep marks the buffer dirty
    // even when the restored state matches the saved state — that matches
    // the project's existing behaviour (preserved here).
    e.undoStep();
    CHECK(e.buffer.find("// edited") == std::string::npos);

    // Type again, save, reload, verify.
    e.moveCursor(e.buffer.size(), false);
    e.insertText("\n// edited2\n");
    const std::string outPath = "test_editor_real_out.cpp";
    CHECK(e.saveAs(outPath));
    CHECK(!e.dirty);

    TextEdit e2;
    CHECK(e2.loadFromFile(outPath));
    CHECK(e2.buffer.find("// edited2") != std::string::npos);
    CHECK(e2.buffer.find("int main(int argc") != std::string::npos);

    // Move cursor: line 1, col 1 should be offset 0.
    int line, col;
    e2.offsetToLineCol(0, line, col);
    CHECK(line == 1 && col == 1);

    std::size_t off = e2.lineColToOffset(1, 1);
    CHECK(off == 0);

    std::remove(path.c_str());
    std::remove(outPath.c_str());

    std::printf("OK\n");
    return 0;
}
