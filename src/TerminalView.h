// TerminalView - a Pty + Vt pair drawn as an ImGui panel.
//
// One instance is the integrated shell, one runs clil, and lldb sessions
// get their own. The grid follows the panel size; keys are translated to
// the escape sequences a terminal would send.

#pragma once

#include <string>
#include <vector>

#include "Pty.h"
#include "Vt.h"

struct ImFont;

class TerminalView {
public:
    ImFont* font = nullptr;

    bool start(const std::vector<std::string>& argv, const std::string& cwd,
               const std::vector<std::string>& env = {});
    void stop() { m_pty.stop(); }
    bool running() const { return m_pty.running(); }
    int  exitStatus() const { return m_pty.exitStatus(); }
    bool started() const { return m_started; }

    // Send text as if typed (bracketed when the program asked for it).
    void sendText(const std::string& text, bool paste = true);
    void sendRaw(const std::string& bytes) { m_pty.write(bytes); }

    // Drain process output into the emulator; returns true if anything changed.
    bool pump();

    // Draw filling the remaining region. Returns true while focused.
    bool render(const char* id);
    void focus() { m_wantFocus = true; }
    void clear();

    const std::string& title() const { return m_vt.title(); }
    // Plain text (escape sequences removed) of the most recent output,
    // at most a few KB. Used to follow the debugger's current line.
    const std::string& recentText() const { return m_recent; }
    void clearRecentText() { m_recent.clear(); }

    // Text of the row the cursor is on (used to see whether clil waits at its prompt).
    std::string cursorLineText() const;

private:
    void handleKeys();
    void copySelection();
    bool cellSelected(int line, int col) const;

    Pty  m_pty;
    Vt   m_vt{ 80, 24 };
    bool m_started = false;
    bool m_wantFocus = false;
    bool m_focused = false;
    int  m_scrollBack = 0;     // lines scrolled up from the live screen
    float m_wheelAccum = 0;

    // Selection in absolute line / column coordinates.
    bool m_selecting = false, m_hasSel = false;
    int  m_selA[2] = { 0, 0 }, m_selB[2] = { 0, 0 };

    float m_originX = 0, m_originY = 0, m_charW = 8, m_lineH = 16;
    int   m_firstLine = 0;
    std::string m_recent;
};
