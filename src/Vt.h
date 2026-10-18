// Vt - a small xterm-compatible screen emulator.
//
// It understands what shells, clil and lldb actually send: UTF-8 text,
// C0 controls, CSI cursor movement / erase / insert / delete / scroll
// regions, SGR colours (16, 256 and true colour), the alternate screen,
// bracketed paste and the cursor-position / device-attribute queries.
// Everything else is parsed and ignored so it never shows up as garbage.

#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

struct VtCell {
    uint32_t ch   = ' ';   // code point; 0 = right half of a wide glyph
    uint32_t fg   = 0;     // packed colour, see Vt::Color*
    uint32_t bg   = 0;
    uint8_t  attr = 0;     // Vt::Attr* bits
};

class Vt {
public:
    enum : uint8_t { AttrBold = 1, AttrDim = 2, AttrItalic = 4, AttrUnderline = 8, AttrInverse = 16 };

    // Colour encoding: 0 = default, 0x01000000 | index (0..255), 0x02000000 | 0xRRGGBB.
    static constexpr uint32_t ColorDefault = 0;
    static uint32_t colorIndex(int i)              { return 0x01000000u | (uint32_t(i) & 0xFF); }
    static uint32_t colorRgb(int r, int g, int b)  { return 0x02000000u | (uint32_t(r & 255) << 16) | (uint32_t(g & 255) << 8) | uint32_t(b & 255); }

    using Line = std::vector<VtCell>;

    Vt(int cols = 80, int rows = 24);

    void resize(int cols, int rows);
    void feed(const char* data, size_t len);
    void reset();

    // Bytes the emulator wants to send back to the program (query replies).
    std::string takeReplies();

    int cols() const { return m_cols; }
    int rows() const { return m_rows; }

    // Lines are addressed from the top of the scrollback: 0 .. totalLines()-1.
    // The last rows() lines are the live screen.
    int totalLines() const { return int(m_scrollback.size()) + m_rows; }
    const Line& line(int index) const;
    int scrollbackLines() const { return int(m_scrollback.size()); }
    void clearScrollback() { m_scrollback.clear(); }

    int  cursorX() const { return m_x; }
    int  cursorY() const { return m_y; }
    bool cursorVisible() const { return m_cursorVisible; }
    bool appCursorKeys() const { return m_appCursor; }
    bool bracketedPaste() const { return m_bracketedPaste; }
    bool altScreen() const { return m_alt; }
    const std::string& title() const { return m_title; }

    // Incremented on every change so views can tell when to redraw.
    uint64_t generation() const { return m_generation; }

    static int charWidth(uint32_t cp);

private:
    enum class State { Ground, Esc, Csi, Osc, OscEsc, Charset, Dcs, DcsEsc };

    void put(uint32_t cp);
    void control(uint8_t c);
    void escape(uint8_t c);
    void csi(uint8_t final);
    void sgr();
    void setMode(bool on);
    void lineFeed();
    void scrollUp(int n);
    void scrollDown(int n);
    void eraseCells(Line& l, int from, int to);
    Line blankLine() const;
    void clampCursor();
    void enterAlt(bool on);
    int  param(size_t i, int def) const;

    int m_cols, m_rows;
    std::vector<Line> m_screen;
    std::vector<Line> m_savedMain;   // main screen while the alternate one is shown
    std::deque<Line>  m_scrollback;
    static constexpr size_t kMaxScrollback = 5000;

    int  m_x = 0, m_y = 0;
    bool m_wrapPending = false;
    int  m_top = 0, m_bottom = 0;    // scroll region, inclusive
    uint32_t m_fg = 0, m_bg = 0;
    uint8_t  m_attr = 0;
    uint32_t m_lastChar = ' ';

    struct Saved { int x = 0, y = 0; uint32_t fg = 0, bg = 0; uint8_t attr = 0; } m_saved, m_savedAlt;

    bool m_cursorVisible = true;
    bool m_appCursor = false;
    bool m_bracketedPaste = false;
    bool m_autowrap = true;
    bool m_alt = false;
    bool m_originMode = false;

    State m_state = State::Ground;
    std::string m_seq;               // CSI parameters / OSC payload
    uint32_t m_utf8 = 0;
    int      m_utf8Need = 0;

    std::vector<int> m_params;
    bool m_private = false;
    std::string m_title;
    std::string m_replies;
    uint64_t m_generation = 0;
};
