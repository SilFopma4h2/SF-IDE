// Theme - colours shared by the ImGui style, the editor and the terminal.

#pragma once

#include <cstdint>

#include <imgui.h>

struct Theme {
    bool dark = true;

    // Chrome
    ImU32 bg, panel, panelAlt, border, text, textDim, accent, accentText, selection, hover;
    // Editor
    ImU32 editorBg, gutterBg, gutterText, gutterTextActive, currentLine, cursor, findMatch, bracket;
    ImU32 synKeyword, synType, synString, synNumber, synComment, synPreproc, synFunction, synPunct, synDefault;
    ImU32 error, warning, ok, breakpoint;
    // Terminal: 16 ANSI colours + default fg / bg
    ImU32 ansi[16];
    ImU32 termFg, termBg;
};

const Theme& theme();
void setTheme(bool dark);   // also restyles ImGui
ImU32 terminalColor(uint32_t packed, bool fg, bool bold);
