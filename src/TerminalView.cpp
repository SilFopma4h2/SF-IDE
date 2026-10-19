#include "TerminalView.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <imgui.h>
#include <imgui_internal.h>

#include "Theme.h"
#include "Wake.h"

bool TerminalView::start(const std::vector<std::string>& argv, const std::string& cwd,
                         const std::vector<std::string>& env)
{
    m_vt.reset();
    m_vt.clearScrollback();
    m_scrollBack = 0;
    m_hasSel = false;
    m_pty.onActivity = [] { wakeUi(); };
    m_started = m_pty.start(argv, cwd, m_vt.cols(), m_vt.rows(), env);
    return m_started;
}

void TerminalView::sendText(const std::string& text, bool paste)
{
    if (paste && m_vt.bracketedPaste())
    {
        m_pty.write("\x1b[200~" + text + "\x1b[201~");
        return;
    }
    std::string s = text;
    std::replace(s.begin(), s.end(), '\n', '\r');
    m_pty.write(s);
}

bool TerminalView::pump()
{
    std::string data;
    if (!m_pty.take(data)) return false;
    int before = m_vt.scrollbackLines();
    m_vt.feed(data.data(), data.size());
    for (size_t i = 0; i < data.size(); ++i)
    {
        if (data[i] == '\x1b' && i + 1 < data.size() && data[i + 1] == '[')
        {
            i += 2;
            while (i < data.size() && !(data[i] >= 0x40 && data[i] <= 0x7E)) ++i;
            continue;
        }
        if (data[i] != '\r') m_recent.push_back(data[i]);
    }
    if (m_recent.size() > 8192) m_recent.erase(0, m_recent.size() - 4096);
    std::string replies = m_vt.takeReplies();
    if (!replies.empty()) m_pty.write(replies);
    // Keep the view still while the user reads older output.
    if (m_scrollBack > 0)
        m_scrollBack = std::min(m_vt.scrollbackLines(), m_scrollBack + (m_vt.scrollbackLines() - before));
    return true;
}

void TerminalView::clear()
{
    m_vt.clearScrollback();
    m_scrollBack = 0;
    m_hasSel = false;
    m_pty.write("\x0c");  // Ctrl-L: let the program redraw its screen
}

std::string TerminalView::cursorLineText() const
{
    const auto& cells = m_vt.line(m_vt.scrollbackLines() + m_vt.cursorY());
    std::string out;
    for (const auto& c : cells)
    {
        if (c.ch == 0) continue;
        char buf[8];
        out += ImTextCharToUtf8(buf, c.ch);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

bool TerminalView::cellSelected(int line, int col) const
{
    if (!m_hasSel) return false;
    const int* a = m_selA;
    const int* b = m_selB;
    if (b[0] < a[0] || (b[0] == a[0] && b[1] < a[1])) std::swap(a, b);
    if (line < a[0] || line > b[0]) return false;
    if (line == a[0] && col < a[1]) return false;
    if (line == b[0] && col >= b[1]) return false;
    return true;
}

void TerminalView::copySelection()
{
    if (!m_hasSel) return;
    const int* a = m_selA;
    const int* b = m_selB;
    if (b[0] < a[0] || (b[0] == a[0] && b[1] < a[1])) std::swap(a, b);
    std::string out;
    for (int l = a[0]; l <= b[0]; ++l)
    {
        const auto& cells = m_vt.line(l);
        int c0 = (l == a[0]) ? a[1] : 0;
        int c1 = (l == b[0]) ? b[1] : int(cells.size());
        std::string row;
        for (int c = c0; c < c1 && c < int(cells.size()); ++c)
        {
            uint32_t ch = cells[size_t(c)].ch;
            if (ch == 0) continue;
            char buf[8];
            row += ImTextCharToUtf8(buf, ch);
        }
        while (!row.empty() && row.back() == ' ') row.pop_back();
        out += row;
        if (l < b[0]) out += '\n';
    }
    ImGui::SetClipboardText(out.c_str());
}

void TerminalView::handleKeys()
{
    ImGuiIO& io = ImGui::GetIO();
    const bool mac = io.ConfigMacOSXBehaviors;
    const bool ctrl = mac ? io.KeySuper : io.KeyCtrl;   // the physical Control key
    const bool cmd = mac && io.KeyCtrl;                 // Command (macOS)
    const bool alt = io.KeyAlt;
    const bool shift = io.KeyShift;
    std::string out;
    auto pressed = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };

    // Copy / paste: Cmd+C / Cmd+V on macOS, Ctrl+Shift+C / V elsewhere.
    bool copyCombo = cmd || (!mac && ctrl && shift);
    if (copyCombo && ImGui::IsKeyPressed(ImGuiKey_C, false)) { copySelection(); return; }
    if (copyCombo && ImGui::IsKeyPressed(ImGuiKey_V, false))
    {
        if (const char* clip = ImGui::GetClipboardText()) sendText(clip, true);
        m_scrollBack = 0;
        return;
    }
    if (cmd && ImGui::IsKeyPressed(ImGuiKey_K, false)) { clear(); return; }
    // Ctrl+Alt combinations are IDE shortcuts on Linux (Cmd+Option on macOS is handled below).
    if (!mac && ctrl && alt) { io.InputQueueCharacters.resize(0); return; }
    if (cmd)
    {
        // Cmd+Left / Right: start / end of line, like Terminal.app with readline.
        if (pressed(ImGuiKey_LeftArrow)) out += "\x01";
        if (pressed(ImGuiKey_RightArrow)) out += "\x05";
        if (pressed(ImGuiKey_Backspace)) out += "\x15";
        io.InputQueueCharacters.resize(0);
        if (!out.empty()) { m_pty.write(out); m_scrollBack = 0; m_hasSel = false; }
        return;  // other Cmd shortcuts belong to the IDE
    }

    if (shift && pressed(ImGuiKey_PageUp))
    {
        m_scrollBack = std::min(m_vt.scrollbackLines(), m_scrollBack + m_vt.rows() - 1);
        return;
    }
    if (shift && pressed(ImGuiKey_PageDown))
    {
        m_scrollBack = std::max(0, m_scrollBack - (m_vt.rows() - 1));
        return;
    }

    const bool app = m_vt.appCursorKeys();
    auto arrow = [&](char c) {
        if (alt && (c == 'C' || c == 'D')) { out += (c == 'C') ? "\x1b" "f" : "\x1b" "b"; return; }
        out += app ? "\x1bO" : "\x1b[";
        out += c;
    };
    if (pressed(ImGuiKey_UpArrow)) arrow('A');
    if (pressed(ImGuiKey_DownArrow)) arrow('B');
    if (pressed(ImGuiKey_RightArrow)) arrow('C');
    if (pressed(ImGuiKey_LeftArrow)) arrow('D');
    if (pressed(ImGuiKey_Home)) out += app ? "\x1bOH" : "\x1b[H";
    if (pressed(ImGuiKey_End)) out += app ? "\x1bOF" : "\x1b[F";
    if (pressed(ImGuiKey_Delete)) out += "\x1b[3~";
    if (pressed(ImGuiKey_PageUp)) out += "\x1b[5~";
    if (pressed(ImGuiKey_PageDown)) out += "\x1b[6~";
    if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter)) out += alt ? "\x1b\r" : "\r";
    if (pressed(ImGuiKey_Backspace)) out += ctrl ? "\x08" : (alt ? "\x1b\x7f" : "\x7f");
    if (pressed(ImGuiKey_Tab)) out += shift ? "\x1b[Z" : "\t";
    if (pressed(ImGuiKey_Escape)) out += "\x1b";

    if (ctrl)
    {
        for (int k = ImGuiKey_A; k <= ImGuiKey_Z; ++k)
            if (pressed(ImGuiKey(k))) out += char(k - ImGuiKey_A + 1);
        if (pressed(ImGuiKey_LeftBracket)) out += "\x1b";
        if (pressed(ImGuiKey_Backslash)) out += "\x1c";
        if (pressed(ImGuiKey_RightBracket)) out += "\x1d";
        if (pressed(ImGuiKey_Space)) out.push_back('\0');
    }
    else
    {
        for (ImWchar c : io.InputQueueCharacters)
        {
            if (c < 32 || c == 127) continue;
            char buf[8];
            if (alt && !mac && c < 128) out += "\x1b";  // Alt as Meta on Linux
            out += ImTextCharToUtf8(buf, c);
        }
    }
    io.InputQueueCharacters.resize(0);

    if (!out.empty())
    {
        m_pty.write(out);
        m_scrollBack = 0;
        m_hasSel = false;
    }
}

bool TerminalView::render(const char* id)
{
    if (font) ImGui::PushFont(font);
    const Theme& th = theme();
    m_charW = ImGui::CalcTextSize("M").x;
    m_lineH = std::floor(ImGui::GetFontSize() * 1.3f);
    const float fs = ImGui::GetFontSize();
    ImFont* f = ImGui::GetFont();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(th.termBg));
    ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                          ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoNavInputs);
    if (m_wantFocus) { ImGui::SetWindowFocus(); m_wantFocus = false; }
    m_focused = ImGui::IsWindowFocused();

    ImGuiWindow* win = ImGui::GetCurrentWindow();
    const float pad = 8.0f;
    ImRect r = win->InnerRect;
    r.Min.x += pad; r.Min.y += pad * 0.5f; r.Max.x -= pad; r.Max.y -= pad * 0.5f;

    const int cols = std::max(10, int((r.GetWidth()) / m_charW));
    const int rows = std::max(3, int((r.GetHeight()) / m_lineH));
    if (cols != m_vt.cols() || rows != m_vt.rows())
    {
        m_vt.resize(cols, rows);
        m_pty.resize(cols, rows);
        m_scrollBack = std::min(m_scrollBack, m_vt.scrollbackLines());
    }

    if (m_focused) handleKeys();

    // Mouse wheel: scroll back through history (or arrow keys in full-screen apps).
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsWindowHovered() && io.MouseWheel != 0.0f)
    {
        m_wheelAccum += io.MouseWheel * 3.0f;
        int lines = int(m_wheelAccum);
        m_wheelAccum -= float(lines);
        if (m_vt.altScreen())
        {
            std::string seq;
            for (int i = 0; i < std::abs(lines); ++i) seq += lines > 0 ? "\x1b[A" : "\x1b[B";
            m_pty.write(seq);
        }
        else
        {
            m_scrollBack = std::clamp(m_scrollBack + lines, 0, m_vt.scrollbackLines());
        }
    }

    m_firstLine = m_vt.totalLines() - m_vt.rows() - m_scrollBack;
    m_originX = r.Min.x;
    m_originY = r.Min.y;

    // Mouse selection.
    auto cellAt = [&](ImVec2 m, int& line, int& col) {
        line = m_firstLine + std::clamp(int(std::floor((m.y - m_originY) / m_lineH)), 0, m_vt.rows() - 1);
        col = std::clamp(int(std::floor((m.x - m_originX) / m_charW + 0.5f)), 0, m_vt.cols());
    };
    if (ImGui::IsWindowHovered())
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            int l, c;
            cellAt(ImGui::GetMousePos(), l, c);
            if (io.MouseClickedCount[0] == 2)
            {
                // Select the word under the mouse.
                const auto& cells = m_vt.line(l);
                auto isWord = [&](int x) {
                    if (x < 0 || x >= int(cells.size())) return false;
                    uint32_t ch = cells[size_t(x)].ch;
                    return ch > ' ' && ch != '"' && ch != '\'' && ch != '(' && ch != ')' && ch != '[' && ch != ']';
                };
                int b = std::min(c, int(cells.size()) - 1), e = b;
                while (isWord(b - 1)) --b;
                while (isWord(e)) ++e;
                m_selA[0] = m_selB[0] = l;
                m_selA[1] = b;
                m_selB[1] = e;
                m_hasSel = e > b;
                m_selecting = false;
            }
            else
            {
                m_selA[0] = m_selB[0] = l;
                m_selA[1] = m_selB[1] = c;
                m_selecting = true;
                m_hasSel = false;
            }
        }
    }
    if (m_selecting)
    {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            int l, c;
            cellAt(ImGui::GetMousePos(), l, c);
            m_selB[0] = l;
            m_selB[1] = c;
            m_hasSel = (m_selA[0] != m_selB[0] || m_selA[1] != m_selB[1]);
        }
        else
        {
            m_selecting = false;
        }
    }

    // --- draw
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(win->InnerRect.Min, win->InnerRect.Max, true);
    const float textOffY = std::floor((m_lineH - fs) * 0.5f);
    std::string run;
    for (int row = 0; row < m_vt.rows(); ++row)
    {
        const int lineIdx = m_firstLine + row;
        const auto& cells = m_vt.line(lineIdx);
        const float y = m_originY + float(row) * m_lineH;
        const int n = std::min(int(cells.size()), m_vt.cols());

        // Backgrounds (and selection) first.
        for (int c = 0; c < n; ++c)
        {
            const VtCell& cell = cells[size_t(c)];
            bool inv = cell.attr & Vt::AttrInverse;
            bool sel = cellSelected(lineIdx, c);
            if (!inv && !sel && cell.bg == 0) continue;
            ImU32 bg = sel ? th.selection
                           : (inv ? terminalColor(cell.fg, true, false) : terminalColor(cell.bg, false, false));
            float x = m_originX + float(c) * m_charW;
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + m_charW + 0.5f, y + m_lineH), bg);
        }

        // Text: batch ASCII runs with the same style; draw other glyphs one by one.
        int runStart = 0;
        uint32_t runFg = 0;
        uint8_t runAttr = 0;
        run.clear();
        auto flush = [&]() {
            if (run.empty()) return;
            ImU32 col = runFg;
            if (runAttr & Vt::AttrDim) col = (col & 0x00FFFFFF) | (0x99u << 24);
            float x = m_originX + float(runStart) * m_charW;
            dl->AddText(f, fs, ImVec2(x, y + textOffY), col, run.data(), run.data() + run.size());
            if (runAttr & Vt::AttrBold)  // faux bold: a second pass offset by a fraction of a pixel
                dl->AddText(f, fs, ImVec2(x + 0.6f, y + textOffY), col, run.data(), run.data() + run.size());
            if (runAttr & Vt::AttrUnderline)
                dl->AddLine(ImVec2(x, y + m_lineH - 2), ImVec2(x + float(run.size()) * m_charW, y + m_lineH - 2), col);
            run.clear();
        };
        for (int c = 0; c < n; ++c)
        {
            const VtCell& cell = cells[size_t(c)];
            if (cell.ch == 0) continue;
            bool inv = cell.attr & Vt::AttrInverse;
            ImU32 fg = inv ? terminalColor(cell.bg, false, false) : terminalColor(cell.fg, true, cell.attr & Vt::AttrBold);
            if (inv && cell.bg == 0) fg = th.termBg;
            if (cell.ch == ' ' && !(cell.attr & Vt::AttrUnderline))
            {
                flush();
                continue;
            }
            uint8_t attr = cell.attr & (Vt::AttrBold | Vt::AttrDim | Vt::AttrUnderline);
            if (cell.ch < 128)
            {
                if (run.empty() || fg != runFg || attr != runAttr || runStart + int(run.size()) != c)
                {
                    flush();
                    runStart = c;
                    runFg = fg;
                    runAttr = attr;
                }
                run.push_back(char(cell.ch));
            }
            else
            {
                flush();
                char buf[8];
                const char* s = ImTextCharToUtf8(buf, cell.ch);
                float x = m_originX + float(c) * m_charW;
                // Box drawing / block characters are drawn as shapes so they join seamlessly.
                if (cell.ch >= 0x2580 && cell.ch <= 0x2588)
                {
                    float top = y, bot = y + m_lineH;
                    if (cell.ch == 0x2580) bot = y + m_lineH * 0.5f;
                    else if (cell.ch == 0x2584) top = y + m_lineH * 0.5f;
                    else if (cell.ch != 0x2588) top = y + m_lineH * (1.0f - float(cell.ch - 0x2580) / 8.0f);
                    dl->AddRectFilled(ImVec2(x, top), ImVec2(x + m_charW + 0.5f, bot), fg);
                }
                else
                {
                    dl->AddText(f, fs, ImVec2(x, y + textOffY), fg, s);
                }
            }
        }
        flush();
    }

    // Cursor.
    if (m_scrollBack == 0 && m_vt.cursorVisible() && m_pty.running())
    {
        float x = m_originX + float(m_vt.cursorX()) * m_charW;
        float y = m_originY + float(m_vt.cursorY()) * m_lineH;
        if (m_focused)
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + m_charW, y + m_lineH), (th.cursor & 0x00FFFFFF) | (0xB0u << 24));
        else
            dl->AddRect(ImVec2(x, y), ImVec2(x + m_charW, y + m_lineH), th.textDim);
    }

    // Scroll-back indicator.
    if (m_scrollBack > 0)
    {
        char buf[48];
        std::snprintf(buf, sizeof buf, "  %d lines up  ", m_scrollBack);
        ImVec2 ts = ImGui::CalcTextSize(buf);
        ImVec2 p(win->InnerRect.Max.x - ts.x - 12, win->InnerRect.Min.y + 6);
        dl->AddRectFilled(p, ImVec2(p.x + ts.x, p.y + ts.y + 4), th.accent, 6.0f);
        dl->AddText(ImVec2(p.x, p.y + 2), th.accentText, buf);
    }
    dl->PopClipRect();

    if (!m_pty.running() && m_started)
    {
        ImGui::SetCursorScreenPos(ImVec2(win->InnerRect.Min.x + pad, win->InnerRect.Max.y - m_lineH - 4));
        ImGui::TextDisabled("[process exited with status %d]", m_pty.exitStatus());
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if (font) ImGui::PopFont();
    return m_focused;
}
