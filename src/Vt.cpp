#include "Vt.h"

#include <algorithm>
#include <cstdio>

Vt::Vt(int cols, int rows)
    : m_cols(std::max(cols, 2)), m_rows(std::max(rows, 1))
{
    reset();
}

void Vt::reset()
{
    m_screen.assign(m_rows, blankLine());
    m_savedMain.clear();
    m_alt = false;
    m_x = m_y = 0;
    m_wrapPending = false;
    m_top = 0;
    m_bottom = m_rows - 1;
    m_fg = m_bg = 0;
    m_attr = 0;
    m_cursorVisible = true;
    m_appCursor = false;
    m_bracketedPaste = false;
    m_autowrap = true;
    m_originMode = false;
    m_state = State::Ground;
    ++m_generation;
}

Vt::Line Vt::blankLine() const
{
    VtCell c;
    c.bg = m_bg;
    return Line(m_cols, c);
}

const Vt::Line& Vt::line(int index) const
{
    static const Line empty;
    if (index < 0) return empty;
    if (index < int(m_scrollback.size())) return m_scrollback[index];
    index -= int(m_scrollback.size());
    if (index < m_rows) return m_screen[index];
    return empty;
}

void Vt::resize(int cols, int rows)
{
    cols = std::max(cols, 2);
    rows = std::max(rows, 1);
    if (cols == m_cols && rows == m_rows) return;

    auto fit = [&](Line& l) {
        VtCell blank;
        l.resize(cols, blank);
    };
    // Shrinking: move lines above the cursor into scrollback so the prompt
    // stays visible, like other terminals do.
    if (rows < m_rows)
    {
        int excess = m_rows - rows;
        int drop = std::min(excess, std::max(0, m_y - (rows - 1)));
        for (int i = 0; i < drop; ++i)
        {
            if (!m_alt) m_scrollback.push_back(m_screen.front());
            m_screen.erase(m_screen.begin());
        }
        m_y -= drop;
        m_screen.resize(rows);
    }
    else
    {
        // Growing: pull lines back from the scrollback first.
        int extra = rows - m_rows;
        while (extra > 0 && !m_alt && !m_scrollback.empty())
        {
            m_screen.insert(m_screen.begin(), m_scrollback.back());
            m_scrollback.pop_back();
            ++m_y;
            --extra;
        }
        VtCell blank;
        m_screen.resize(rows, Line(cols, blank));
    }
    for (auto& l : m_screen) fit(l);
    for (auto& l : m_savedMain) fit(l);
    if (!m_savedMain.empty()) m_savedMain.resize(rows, Line(cols, VtCell{}));
    m_cols = cols;
    m_rows = rows;
    m_top = 0;
    m_bottom = rows - 1;
    m_wrapPending = false;
    clampCursor();
    ++m_generation;
}

std::string Vt::takeReplies()
{
    std::string r;
    r.swap(m_replies);
    return r;
}

// --- Input parsing ------------------------------------------------------------

void Vt::feed(const char* data, size_t len)
{
    ++m_generation;
    for (size_t i = 0; i < len; ++i)
    {
        const uint8_t c = static_cast<uint8_t>(data[i]);

        switch (m_state)
        {
        case State::Ground:
            break;
        case State::Esc:
            escape(c);
            continue;
        case State::Charset:
            m_state = State::Ground;  // ESC ( B and friends: always UTF-8 here
            continue;
        case State::Csi:
            if (c >= 0x40 && c <= 0x7E) { csi(c); m_state = State::Ground; }
            else if (c == 0x1B) m_state = State::Esc;
            else if (c < 0x20) control(c);  // controls are executed inside CSI
            else m_seq.push_back(static_cast<char>(c));
            continue;
        case State::Osc:
            if (c == 0x07) { m_state = State::Ground; }
            else if (c == 0x1B) m_state = State::OscEsc;
            else if (m_seq.size() < 4096) m_seq.push_back(static_cast<char>(c));
            if (m_state == State::Ground)
            {
                // OSC 0 / 2: window title
                if (m_seq.size() > 2 && (m_seq[0] == '0' || m_seq[0] == '2') && m_seq[1] == ';')
                    m_title = m_seq.substr(2);
            }
            continue;
        case State::OscEsc:
            if (m_seq.size() > 2 && (m_seq[0] == '0' || m_seq[0] == '2') && m_seq[1] == ';')
                m_title = m_seq.substr(2);
            m_state = State::Ground;  // ESC \\ (ST) or anything else ends the OSC
            continue;
        case State::Dcs:
            if (c == 0x1B) m_state = State::DcsEsc;
            continue;
        case State::DcsEsc:
            m_state = (c == '\\') ? State::Ground : State::Dcs;
            continue;
        }

        // Ground state: UTF-8 text and C0 controls.
        if (m_utf8Need > 0)
        {
            if ((c & 0xC0) == 0x80)
            {
                m_utf8 = (m_utf8 << 6) | (c & 0x3F);
                if (--m_utf8Need == 0) put(m_utf8);
                continue;
            }
            m_utf8Need = 0;
            put(0xFFFD);
        }
        if (c < 0x20 || c == 0x7F) { control(c); continue; }
        if (c < 0x80) { put(c); continue; }
        if ((c & 0xE0) == 0xC0) { m_utf8 = c & 0x1F; m_utf8Need = 1; }
        else if ((c & 0xF0) == 0xE0) { m_utf8 = c & 0x0F; m_utf8Need = 2; }
        else if ((c & 0xF8) == 0xF0) { m_utf8 = c & 0x07; m_utf8Need = 3; }
        else put(0xFFFD);
    }
}

void Vt::control(uint8_t c)
{
    switch (c)
    {
    case 0x07: break;  // BEL
    case 0x08:         // BS
        if (m_x > 0) --m_x;
        m_wrapPending = false;
        break;
    case 0x09:         // HT
        m_x = std::min(m_cols - 1, (m_x / 8 + 1) * 8);
        m_wrapPending = false;
        break;
    case 0x0A: case 0x0B: case 0x0C:
        lineFeed();
        break;
    case 0x0D:
        m_x = 0;
        m_wrapPending = false;
        break;
    case 0x1B:
        m_state = State::Esc;
        break;
    default:
        break;
    }
}

void Vt::escape(uint8_t c)
{
    m_state = State::Ground;
    switch (c)
    {
    case '[':
        m_state = State::Csi;
        m_seq.clear();
        break;
    case ']':
        m_state = State::Osc;
        m_seq.clear();
        break;
    case 'P': case 'X': case '^': case '_':
        m_state = State::Dcs;
        break;
    case '(': case ')': case '*': case '+':
        m_state = State::Charset;
        break;
    case '7':
        m_saved = { m_x, m_y, m_fg, m_bg, m_attr };
        break;
    case '8':
        m_x = m_saved.x; m_y = m_saved.y;
        m_fg = m_saved.fg; m_bg = m_saved.bg; m_attr = m_saved.attr;
        clampCursor();
        break;
    case 'D':
        lineFeed();
        break;
    case 'E':
        m_x = 0;
        lineFeed();
        break;
    case 'M':  // reverse index
        if (m_y == m_top) scrollDown(1);
        else if (m_y > 0) --m_y;
        m_wrapPending = false;
        break;
    case 'c':
        reset();
        break;
    default:   // '=', '>', and anything unknown
        break;
    }
}

int Vt::param(size_t i, int def) const
{
    if (i >= m_params.size() || m_params[i] <= 0) return def;
    return m_params[i];
}

void Vt::csi(uint8_t final)
{
    // Parse "?1;2" style parameter strings. ':' sub-parameters are treated
    // like ';' which is what 38:2:r:g:b style SGR needs in practice.
    m_params.clear();
    m_private = false;
    bool intermediate = false;
    int cur = -1;
    for (char ch : m_seq)
    {
        if (ch == '?' || ch == '>' || ch == '<' || ch == '=') { m_private = true; continue; }
        if (ch >= '0' && ch <= '9')
        {
            cur = (cur < 0 ? 0 : cur) * 10 + (ch - '0');
            if (cur > 65535) cur = 65535;
        }
        else if (ch == ';' || ch == ':')
        {
            m_params.push_back(cur);
            cur = -1;
        }
        else if (ch >= 0x20 && ch <= 0x2F)
        {
            intermediate = true;
        }
    }
    m_params.push_back(cur);
    if (intermediate && final != 'q') return;  // e.g. CSI ! p, CSI SP q: ignore

    auto& row = m_screen[m_y];
    switch (final)
    {
    case '@': {  // ICH
        int n = std::min(param(0, 1), m_cols - m_x);
        row.insert(row.begin() + m_x, n, VtCell{ ' ', 0, m_bg, 0 });
        row.resize(m_cols);
        break;
    }
    case 'A': {  // CUU / CUD stop at the scroll region when inside it
        int lo = m_y >= m_top ? m_top : 0;
        m_y = std::max(lo, m_y - param(0, 1));
        m_wrapPending = false;
        break;
    }
    case 'B': {
        int hi = m_y <= m_bottom ? m_bottom : m_rows - 1;
        m_y = std::min(hi, m_y + param(0, 1));
        m_wrapPending = false;
        break;
    }
    case 'C': m_x = std::min(m_cols - 1, m_x + param(0, 1)); m_wrapPending = false; break;
    case 'D': m_x = std::max(0, std::min(m_x, m_cols - 1) - param(0, 1)); m_wrapPending = false; break;
    case 'E': m_y = std::min(m_rows - 1, m_y + param(0, 1)); m_x = 0; m_wrapPending = false; break;
    case 'F': m_y = std::max(0, m_y - param(0, 1)); m_x = 0; m_wrapPending = false; break;
    case 'G': case '`': m_x = std::clamp(param(0, 1) - 1, 0, m_cols - 1); m_wrapPending = false; break;
    case 'H': case 'f':
        m_y = param(0, 1) - 1 + (m_originMode ? m_top : 0);
        m_x = param(1, 1) - 1;
        m_wrapPending = false;
        clampCursor();
        break;
    case 'd': m_y = std::clamp(param(0, 1) - 1, 0, m_rows - 1); m_wrapPending = false; break;
    case 'J': {
        int mode = std::max(0, m_params[0]);
        if (mode == 0)
        {
            eraseCells(m_screen[m_y], m_x, m_cols);
            for (int y = m_y + 1; y < m_rows; ++y) eraseCells(m_screen[y], 0, m_cols);
        }
        else if (mode == 1)
        {
            for (int y = 0; y < m_y; ++y) eraseCells(m_screen[y], 0, m_cols);
            eraseCells(m_screen[m_y], 0, m_x + 1);
        }
        else if (mode == 2)
        {
            for (auto& l : m_screen) eraseCells(l, 0, m_cols);
        }
        else if (mode == 3)
        {
            m_scrollback.clear();
        }
        break;
    }
    case 'K': {
        int mode = std::max(0, m_params[0]);
        if (mode == 0)      eraseCells(row, m_x, m_cols);
        else if (mode == 1) eraseCells(row, 0, m_x + 1);
        else                eraseCells(row, 0, m_cols);
        break;
    }
    case 'L': case 'M': {  // IL / DL inside the scroll region
        if (m_y < m_top || m_y > m_bottom) break;
        int n = std::min(param(0, 1), m_bottom - m_y + 1);
        for (int i = 0; i < n; ++i)
        {
            if (final == 'L')
            {
                m_screen.erase(m_screen.begin() + m_bottom);
                m_screen.insert(m_screen.begin() + m_y, blankLine());
            }
            else
            {
                m_screen.erase(m_screen.begin() + m_y);
                m_screen.insert(m_screen.begin() + m_bottom, blankLine());
            }
        }
        m_x = 0;
        m_wrapPending = false;
        break;
    }
    case 'P': {  // DCH
        int n = std::min(param(0, 1), m_cols - m_x);
        row.erase(row.begin() + m_x, row.begin() + m_x + n);
        row.resize(m_cols, VtCell{ ' ', 0, m_bg, 0 });
        break;
    }
    case 'X': eraseCells(row, m_x, std::min(m_cols, m_x + param(0, 1))); break;
    case 'S': scrollUp(param(0, 1)); break;
    case 'T': if (!m_private) scrollDown(param(0, 1)); break;
    case 'b': for (int i = 0, n = std::min(param(0, 1), 4096); i < n; ++i) put(m_lastChar); break;
    case 'm': if (!m_private) sgr(); break;
    case 'r': {
        if (m_private) break;
        int top = param(0, 1) - 1;
        int bottom = param(1, m_rows) - 1;
        if (top < bottom && bottom < m_rows)
        {
            m_top = top;
            m_bottom = bottom;
        }
        else
        {
            m_top = 0;
            m_bottom = m_rows - 1;
        }
        m_x = 0;
        m_y = m_originMode ? m_top : 0;
        m_wrapPending = false;
        break;
    }
    case 's': if (!m_private) m_saved = { m_x, m_y, m_fg, m_bg, m_attr }; break;
    case 'u':
        if (!m_private)
        {
            m_x = m_saved.x; m_y = m_saved.y;
            m_fg = m_saved.fg; m_bg = m_saved.bg; m_attr = m_saved.attr;
            clampCursor();
        }
        break;
    case 'h': setMode(true); break;
    case 'l': setMode(false); break;
    case 'n':
        if (m_params[0] == 5) m_replies += "\x1b[0n";
        else if (m_params[0] == 6)
        {
            char buf[32];
            std::snprintf(buf, sizeof buf, "\x1b[%d;%dR", m_y + 1, std::min(m_x, m_cols - 1) + 1);
            m_replies += buf;
        }
        break;
    case 'c':
        if (!m_private) m_replies += "\x1b[?1;2c";
        else            m_replies += "\x1b[>0;10;0c";
        break;
    default:
        break;  // 't' (window ops), 'q' (cursor style) and others are ignored
    }
}

void Vt::setMode(bool on)
{
    for (int p : m_params)
    {
        if (!m_private)
        {
            continue;  // IRM (4) / LNM (20) are not used by our programs
        }
        switch (p)
        {
        case 1:    m_appCursor = on; break;
        case 6:    m_originMode = on; m_x = 0; m_y = on ? m_top : 0; break;
        case 7:    m_autowrap = on; break;
        case 25:   m_cursorVisible = on; break;
        case 47: case 1047: enterAlt(on); break;
        case 1048:
            if (on) m_saved = { m_x, m_y, m_fg, m_bg, m_attr };
            else { m_x = m_saved.x; m_y = m_saved.y; clampCursor(); }
            break;
        case 1049:
            if (on) { m_savedAlt = { m_x, m_y, m_fg, m_bg, m_attr }; enterAlt(true); }
            else    { enterAlt(false); m_x = m_savedAlt.x; m_y = m_savedAlt.y; clampCursor(); }
            break;
        case 2004: m_bracketedPaste = on; break;
        default: break;
        }
    }
}

void Vt::enterAlt(bool on)
{
    if (on == m_alt) return;
    if (on)
    {
        m_savedMain = m_screen;
        m_screen.assign(m_rows, blankLine());
    }
    else
    {
        m_screen = m_savedMain;
        m_screen.resize(m_rows, blankLine());
        m_savedMain.clear();
    }
    m_alt = on;
    m_top = 0;
    m_bottom = m_rows - 1;
    m_wrapPending = false;
}

void Vt::sgr()
{
    // Standard 16-colour mapping: 30-37 / 90-97 foreground, 40-47 / 100-107 background.
    for (size_t i = 0; i < m_params.size(); ++i)
    {
        int p = m_params[i] < 0 ? 0 : m_params[i];
        switch (p)
        {
        case 0:  m_fg = m_bg = 0; m_attr = 0; break;
        case 1:  m_attr |= AttrBold; break;
        case 2:  m_attr |= AttrDim; break;
        case 3:  m_attr |= AttrItalic; break;
        case 4:  m_attr |= AttrUnderline; break;
        case 7:  m_attr |= AttrInverse; break;
        case 21: case 22: m_attr &= ~(AttrBold | AttrDim); break;
        case 23: m_attr &= ~AttrItalic; break;
        case 24: m_attr &= ~AttrUnderline; break;
        case 27: m_attr &= ~AttrInverse; break;
        case 39: m_fg = 0; break;
        case 49: m_bg = 0; break;
        case 38: case 48: {
            uint32_t col = 0;
            if (i + 1 < m_params.size() && m_params[i + 1] == 5 && i + 2 < m_params.size())
            {
                col = colorIndex(std::max(0, m_params[i + 2]));
                i += 2;
            }
            else if (i + 1 < m_params.size() && m_params[i + 1] == 2 && i + 4 < m_params.size())
            {
                col = colorRgb(std::max(0, m_params[i + 2]), std::max(0, m_params[i + 3]),
                               std::max(0, m_params[i + 4]));
                i += 4;
            }
            else
            {
                i = m_params.size();
            }
            (p == 38 ? m_fg : m_bg) = col;
            break;
        }
        default:
            if (p >= 30 && p <= 37)        m_fg = colorIndex(p - 30);
            else if (p >= 40 && p <= 47)   m_bg = colorIndex(p - 40);
            else if (p >= 90 && p <= 97)   m_fg = colorIndex(p - 90 + 8);
            else if (p >= 100 && p <= 107) m_bg = colorIndex(p - 100 + 8);
            break;
        }
    }
}

// --- Screen operations ----------------------------------------------------------

void Vt::put(uint32_t cp)
{
    int w = charWidth(cp);
    if (w == 0) return;  // combining marks / zero-width: dropped
    m_lastChar = cp;

    if (m_wrapPending)
    {
        if (m_autowrap)
        {
            m_x = 0;
            lineFeed();
        }
        m_wrapPending = false;
    }
    if (w == 2 && m_x == m_cols - 1)
    {
        if (m_autowrap)
        {
            m_screen[m_y][m_x] = VtCell{ ' ', m_fg, m_bg, m_attr };
            m_x = 0;
            lineFeed();
        }
        else
        {
            w = 1;
        }
    }

    auto& row = m_screen[m_y];
    row[m_x] = VtCell{ cp, m_fg, m_bg, m_attr };
    if (w == 2) row[m_x + 1] = VtCell{ 0, m_fg, m_bg, m_attr };
    m_x += w;
    if (m_x >= m_cols)
    {
        m_x = m_cols - 1;
        m_wrapPending = true;
    }
}

void Vt::lineFeed()
{
    m_wrapPending = false;
    if (m_y == m_bottom) scrollUp(1);
    else if (m_y < m_rows - 1) ++m_y;
}

void Vt::scrollUp(int n)
{
    n = std::min(n, m_bottom - m_top + 1);
    for (int i = 0; i < n; ++i)
    {
        if (m_top == 0 && !m_alt)
        {
            m_scrollback.push_back(std::move(m_screen[0]));
            if (m_scrollback.size() > kMaxScrollback) m_scrollback.pop_front();
        }
        m_screen.erase(m_screen.begin() + m_top);
        m_screen.insert(m_screen.begin() + m_bottom, blankLine());
    }
}

void Vt::scrollDown(int n)
{
    n = std::min(n, m_bottom - m_top + 1);
    for (int i = 0; i < n; ++i)
    {
        m_screen.erase(m_screen.begin() + m_bottom);
        m_screen.insert(m_screen.begin() + m_top, blankLine());
    }
}

void Vt::eraseCells(Line& l, int from, int to)
{
    from = std::clamp(from, 0, m_cols);
    to = std::clamp(to, 0, m_cols);
    for (int x = from; x < to; ++x) l[x] = VtCell{ ' ', 0, m_bg, 0 };
    m_wrapPending = false;
}

void Vt::clampCursor()
{
    m_x = std::clamp(m_x, 0, m_cols - 1);
    m_y = std::clamp(m_y, 0, m_rows - 1);
}

int Vt::charWidth(uint32_t cp)
{
    if ((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x200B && cp <= 0x200F) ||
        (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0x200D)
        return 0;
    if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF && cp != 0x303F) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1F64F) ||
        (cp >= 0x1F900 && cp <= 0x1F9FF) || (cp >= 0x20000 && cp <= 0x3FFFD))
        return 2;
    return 1;
}
