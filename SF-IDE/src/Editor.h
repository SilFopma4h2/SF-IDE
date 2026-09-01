// SF-IDE - minimal text editor for C/C++ source files.
// Backed by an in-memory line buffer, with cursor/selection, undo/redo, and
// simple syntax highlighting. No external editor library is used.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

struct TextEdit {
    // Full file contents stored as a single UTF-8 string with '\n' between lines.
    // Newlines are normalised to '\n' on load/save so cursor math is simple.
    std::string buffer;

    // Cursor and selection (character offsets into buffer, end-exclusive).
    // When anchor == cursor, there is no selection.
    std::size_t cursor  = 0;
    std::size_t anchor  = 0;

    // Current file on disk; empty if the buffer has never been saved.
    std::string filePath;
    bool        dirty   = false;

    // Undo / redo stacks store full snapshots of `buffer`. This is intentionally
    // simple and only used while the editor is alive; the file on disk is the
    // long-term record.
    std::vector<std::string> undo;
    std::vector<std::string> redo;
    static constexpr std::size_t kMaxUndo = 200;

    // Load / save -------------------------------------------------------------
    bool loadFromFile(const std::string& path);
    bool saveToFile(const std::string& path);
    bool save();   // uses filePath
    bool saveAs(const std::string& path);

    // Editing -----------------------------------------------------------------
    void insertText(const std::string& text);
    void deleteSelection();
    void deleteBackspace();
    void deleteForward();
    void newLine();

    // Cursor movement (clamps to buffer)
    void moveCursor(std::size_t newPos, bool extendSelection);

    // Undo / redo
    void pushUndo();
    void undoStep();
    void redoStep();

    // Queries -----------------------------------------------------------------
    std::size_t selectionMin() const;
    std::size_t selectionMax() const;
    bool        hasSelection() const;
    void        clearSelection();

    // Splits the buffer into lines (without the trailing '\n').
    std::vector<std::string_view> lines() const;

    // Line / column of a character offset (1-based for display).
    void offsetToLineCol(std::size_t offset, int& line, int& col) const;
    std::size_t lineColToOffset(int line, int col) const;
    int lineCount() const;

private:
    // Erase the active selection without pushing an undo snapshot. Used
    // by insertText so a single edit produces one undo entry even when
    // it overwrites a selection.
    void eraseSelectionNoUndo();
};
