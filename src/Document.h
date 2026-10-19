// Document - one open text file: buffer, line index, cursor, undo history.
//
// The buffer is a single UTF-8 std::string with '\n' line endings; the
// original line ending style is remembered and restored on save. A line
// start index is rebuilt after every edit so line lookups are O(log n)
// instead of scanning the whole file each frame.

#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "Languages.h"

struct Diagnostic {
    int         line = 0;     // 1-based
    int         col  = 0;     // 1-based, 0 = whole line
    bool        error = true; // false = warning / note
    std::string message;
};

class Document {
public:
    enum class EditKind { Other, Insert, Delete };

    // Load / save ---------------------------------------------------------------
    bool load(const std::string& path);
    bool save();
    bool saveAs(const std::string& path);
    bool reloadFromDisk();          // keeps cursor position where possible
    bool changedOnDisk() const;     // file mtime differs from what we loaded / saved
    void markDiskStateSeen();       // forget an external change (user chose "keep")

    const std::string& path() const { return m_path; }
    std::string displayName() const;
    bool dirty() const { return m_dirty; }
    const Language& lang() const { return *m_lang; }
    void setLanguage(const Language& l) { m_lang = &l; ++m_version; }
    uint64_t version() const { return m_version; }

    // Text access ----------------------------------------------------------------
    const std::string& text() const { return m_text; }
    int         lineCount() const { return int(m_lineStarts.size()); }
    size_t      lineStart(int line) const;           // 0-based line
    size_t      lineEnd(int line) const;             // offset of '\n' (or text end)
    std::string_view lineText(int line) const;
    int         lineOf(size_t offset) const;         // 0-based
    std::string selectedText() const;

    // Cursor ------------------------------------------------------------------------
    size_t cursor = 0;
    size_t anchor = 0;
    // Preferred visual column for up/down movement (-1 = use current column).
    int    preferredCol = -1;

    bool   hasSelection() const { return cursor != anchor; }
    size_t selMin() const { return cursor < anchor ? cursor : anchor; }
    size_t selMax() const { return cursor < anchor ? anchor : cursor; }
    void   setCursor(size_t pos, bool extend);
    void   selectAll();

    // Editing -------------------------------------------------------------------------
    // Replace [from, to) with `s`, leaving the cursor after the inserted text.
    void replace(size_t from, size_t to, const std::string& s, EditKind kind = EditKind::Other);
    void insert(const std::string& s, EditKind kind = EditKind::Insert);  // at cursor / over selection
    void backspace();
    void deleteForward();
    void deleteRange(size_t from, size_t to);
    // Replace the whole text with `text` as an undoable edit made by another
    // program (clil): only the changed region is touched, the user's cursor
    // stays where it was, and the changed lines light up briefly.
    // Returns the first changed line (1-based), 0 if nothing changed.
    int applyExternal(const std::string& text);
    int    flashFrom = 0, flashTo = 0;   // 1-based lines highlighted after an external edit
    double flashUntil = 0;               // steady-clock seconds

    void undo();
    void redo();
    bool canUndo() const { return !m_undo.empty(); }
    bool canRedo() const { return !m_redo.empty(); }
    void breakUndoGroup() { m_lastKind = EditKind::Other; }

    // Breakpoints (1-based lines) and build diagnostics follow edits.
    std::set<int>           breakpoints;
    std::vector<Diagnostic> diagnostics;
    void toggleBreakpoint(int line1);
    int execLine = 0;  // 1-based line where the debugger is stopped, 0 = none

    // Syntax state each line starts in (syntax::State*: block comment,
    // multi-line string ...). Updated lazily after edits.
    const std::vector<uint8_t>& lineStates();

    // Cached width of the longest line in columns (tabs expanded).
    int maxColumns();

    // UTF-8 helpers
    size_t nextChar(size_t pos) const;
    size_t prevChar(size_t pos) const;
    size_t wordLeft(size_t pos) const;
    size_t wordRight(size_t pos) const;
    void   wordAt(size_t pos, size_t& b, size_t& e) const;

    static constexpr int kTabWidth = 4;
    // Visual column (0-based) of `offset` in `line`, tabs expanded.
    int    columnOf(int line, size_t offset) const;
    size_t offsetAtColumn(int line, int col) const;

private:
    struct Snapshot { std::string text; size_t cursor, anchor; };

    void rebuildLines();
    void pushUndo(EditKind kind);
    void shiftMarkers(int fromLine0, int delta);
    void detectLang();
    void updateDirtyFromHash();

    std::string m_text;
    std::vector<size_t> m_lineStarts{0};
    std::string m_path;
    bool m_dirty = false;
    size_t m_savedHash = 0;     // hash of the text as last loaded / saved
    bool m_crlf = false;
    const Language* m_lang = &plainText();
    int64_t m_mtime = 0;
    int64_t m_size = -1;
    uint64_t m_version = 1;

    std::vector<Snapshot> m_undo, m_redo;
    EditKind m_lastKind = EditKind::Other;
    double   m_lastEditTime = 0;
    static constexpr size_t kMaxUndo = 500;

    std::vector<uint8_t> m_lineStates;
    uint64_t m_statesVersion = 0;
    int      m_maxCols = 0;
    uint64_t m_maxColsVersion = 0;
};
