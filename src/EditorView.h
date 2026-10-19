// EditorView - draws a Document and handles editing input.
//
// Only the visible lines are tokenised and drawn, so the cost per frame is
// independent of the file size. Keyboard shortcuts follow macOS
// conventions when io.ConfigMacOSXBehaviors is set (Cmd / Option), and
// Ctrl-based ones elsewhere.

#pragma once

#include <functional>
#include <string>

#include "Document.h"

struct ImFont;

class EditorView {
public:
    ImFont* font = nullptr;   // monospace font; nullptr = current font

    // Called inside the editor's right-click popup to add items.
    std::function<void(Document&)> contextMenu;

    // Draws `doc` filling the remaining content region. Returns true while
    // the editor has keyboard focus.
    bool render(Document& doc);

    void focus() { m_wantFocus = true; }
    void revealCursor() { m_revealCursor = true; }
    void gotoLine(Document& doc, int line1, int col1 = 1);
    // Scroll so `line1` is visible without moving the cursor or taking focus.
    void scrollToLine(int line1) { m_scrollLine = line1; }

    // Find support: matches are highlighted while `findText` is non-empty.
    std::string findText;
    bool        findCaseSensitive = false;
    bool        findNext(Document& doc, bool backwards);
    int         countMatches(const Document& doc) const;
    void        replaceCurrent(Document& doc, const std::string& with);
    int         replaceAll(Document& doc, const std::string& with);

    // Editing commands, also used by the menu.
    void toggleComment(Document& doc);
    void indent(Document& doc, bool outdent);
    void duplicateLine(Document& doc);
    void copy(Document& doc);
    void cut(Document& doc);
    void paste(Document& doc);

private:
    void handleKeyboard(Document& doc);
    void handleMouse(Document& doc);
    void moveVertical(Document& doc, int lines, bool extend);
    void newline(Document& doc);
    size_t posFromMouse(Document& doc) const;
    size_t findFrom(const Document& doc, size_t from, bool backwards) const;
    bool   matchesAt(const Document& doc, size_t pos) const;

    bool  m_wantFocus = false;
    int   m_scrollLine = 0;
    bool  m_revealCursor = false;
    bool  m_dragging = false;
    bool  m_dragWords = false;
    size_t m_dragOrigin = 0;
    bool  m_focused = false;
    const Document* m_lastDoc = nullptr;

    // Geometry from the last draw pass (screen coordinates).
    float m_originX = 0, m_originY = 0;   // top-left of line 0, column 0
    float m_gutterRight = 0;
    float m_lineH = 16, m_charW = 8;
    float m_viewTop = 0, m_viewBottom = 0, m_viewLeft = 0, m_viewRight = 0;
};
