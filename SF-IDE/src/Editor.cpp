#include "Editor.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace {

// Replace all "\r\n" / "\r" with "\n" so we can treat the buffer as line based.
void normaliseNewlines(std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        char c = s[i];
        if (c == '\r')
        {
            out.push_back('\n');
            if (i + 1 < s.size() && s[i + 1] == '\n')
                ++i;
        }
        else
        {
            out.push_back(c);
        }
    }
    s.swap(out);
}

// Convert internal '\n' newlines back to "\r\n" on Windows for nicer file output.
void denormaliseNewlines(std::string& s)
{
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s)
    {
        if (c == '\n')
            out.append("\r\n");
        else
            out.push_back(c);
    }
    s.swap(out);
}

} // namespace

// --- File I/O ---------------------------------------------------------------

bool TextEdit::loadFromFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    buffer = ss.str();
    normaliseNewlines(buffer);
    filePath = path;
    cursor  = 0;
    anchor  = 0;
    dirty   = false;
    undo.clear();
    redo.clear();
    return true;
}

bool TextEdit::saveToFile(const std::string& path)
{
    std::string out = buffer;
    denormaliseNewlines(out);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f)
        return false;
    f.write(out.data(), static_cast<std::streamsize>(out.size()));
    if (!f)
        return false;
    filePath = path;
    dirty = false;
    return true;
}

bool TextEdit::save()
{
    if (filePath.empty())
        return false;
    return saveToFile(filePath);
}

bool TextEdit::saveAs(const std::string& path)
{
    return saveToFile(path);
}

// --- Editing ----------------------------------------------------------------

void TextEdit::pushUndo()
{
    undo.push_back(buffer);
    if (undo.size() > kMaxUndo)
        undo.erase(undo.begin());
    redo.clear();
}

void TextEdit::insertText(const std::string& text)
{
    if (text.empty()) return;
    // One user edit = one undo entry, even when it overwrites a selection.
    pushUndo();
    eraseSelectionNoUndo();
    buffer.insert(cursor, text);
    cursor += text.size();
    anchor = cursor;
    dirty = true;
}

void TextEdit::deleteSelection()
{
    if (!hasSelection()) return;
    pushUndo();
    eraseSelectionNoUndo();
    dirty = true;
}

void TextEdit::eraseSelectionNoUndo()
{
    if (!hasSelection()) return;
    const std::size_t lo = selectionMin();
    const std::size_t hi = selectionMax();
    buffer.erase(lo, hi - lo);
    cursor = lo;
    anchor = lo;
}

void TextEdit::deleteBackspace()
{
    if (hasSelection()) { deleteSelection(); return; }
    if (cursor == 0) return;
    pushUndo();

    // If the previous character is '\n' (possibly preceded by '\r' which we
    // normalised away) just remove one char. Otherwise join with previous line.
    std::size_t eraseStart = cursor - 1;
    buffer.erase(eraseStart, 1);
    cursor = eraseStart;
    anchor = cursor;
    dirty = true;
}

void TextEdit::deleteForward()
{
    if (hasSelection()) { deleteSelection(); return; }
    if (cursor >= buffer.size()) return;
    pushUndo();
    buffer.erase(cursor, 1);
    anchor = cursor;
    dirty = true;
}

void TextEdit::newLine()
{
    insertText("\n");
}

void TextEdit::moveCursor(std::size_t newPos, bool extendSelection)
{
    cursor = std::min(newPos, buffer.size());
    if (!extendSelection)
        anchor = cursor;
}

void TextEdit::undoStep()
{
    if (undo.empty()) return;
    redo.push_back(buffer);
    buffer = undo.back();
    undo.pop_back();
    if (cursor > buffer.size()) cursor = buffer.size();
    if (anchor > buffer.size()) anchor = cursor;
    dirty = true;
}

void TextEdit::redoStep()
{
    if (redo.empty()) return;
    undo.push_back(buffer);
    buffer = redo.back();
    redo.pop_back();
    if (cursor > buffer.size()) cursor = buffer.size();
    if (anchor > buffer.size()) anchor = cursor;
    dirty = true;
}

// --- Queries ----------------------------------------------------------------

std::size_t TextEdit::selectionMin() const
{
    return std::min(cursor, anchor);
}

std::size_t TextEdit::selectionMax() const
{
    return std::max(cursor, anchor);
}

bool TextEdit::hasSelection() const
{
    return cursor != anchor;
}

void TextEdit::clearSelection()
{
    anchor = cursor;
}

std::vector<std::string_view> TextEdit::lines() const
{
    std::vector<std::string_view> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i < buffer.size(); ++i)
    {
        if (buffer[i] == '\n')
        {
            out.emplace_back(buffer.data() + start, i - start);
            start = i + 1;
        }
    }
    out.emplace_back(buffer.data() + start, buffer.size() - start);
    return out;
}

void TextEdit::offsetToLineCol(std::size_t offset, int& line, int& col) const
{
    line = 1; col = 1;
    for (std::size_t i = 0; i < offset && i < buffer.size(); ++i)
    {
        if (buffer[i] == '\n') { ++line; col = 1; }
        else                   { ++col; }
    }
}

std::size_t TextEdit::lineColToOffset(int line, int col) const
{
    if (line < 1) line = 1;
    if (col < 1)  col  = 1;
    int curLine = 1;
    std::size_t i = 0;
    while (i < buffer.size() && curLine < line)
    {
        if (buffer[i] == '\n') ++curLine;
        ++i;
    }
    std::size_t lineStart = i;
    int curCol = 1;
    while (i < buffer.size() && curCol < col && buffer[i] != '\n')
    {
        ++i; ++curCol;
    }
    return lineStart + (curCol - 1);
}

int TextEdit::lineCount() const
{
    if (buffer.empty()) return 1;
    int n = 1;
    for (char c : buffer) if (c == '\n') ++n;
    return n;
}
