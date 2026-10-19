#include "Theme.h"

#include <cstdint>

namespace {

constexpr ImU32 rgb(uint32_t hex, uint8_t a = 255)
{
    return IM_COL32((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF, a);
}

Theme makeDark()
{
    Theme t;
    t.dark       = true;
    t.bg         = rgb(0x1E1F22);
    t.panel      = rgb(0x232427);
    t.panelAlt   = rgb(0x2B2D31);
    t.border     = rgb(0x34363B);
    t.text       = rgb(0xDCDDDE);
    t.textDim    = rgb(0x8B8E95);
    t.accent     = rgb(0x3D7EFF);
    t.accentText = rgb(0xFFFFFF);
    t.selection  = rgb(0x2F4F86);
    t.hover      = rgb(0x31343A);

    t.editorBg         = rgb(0x1E1F22);
    t.gutterBg         = rgb(0x1E1F22);
    t.gutterText       = rgb(0x5C5F66);
    t.gutterTextActive = rgb(0xB4B6BB);
    t.currentLine      = rgb(0x26282C);
    t.cursor           = rgb(0x6FA8FF);
    t.findMatch        = rgb(0x8A6D1F, 150);
    t.bracket          = rgb(0x4B5263);

    t.synKeyword  = rgb(0xFC5FA3);
    t.synType     = rgb(0x5DD8FF);
    t.synString   = rgb(0xFC6A5D);
    t.synNumber   = rgb(0xD0BF69);
    t.synComment  = rgb(0x6C7986);
    t.synPreproc  = rgb(0xFD8F3F);
    t.synFunction = rgb(0x67B7A4);
    t.synPunct    = rgb(0xB8BAC0);
    t.synDefault  = rgb(0xDCDDDE);

    t.error      = rgb(0xFF5F57);
    t.warning    = rgb(0xFEBC2E);
    t.ok         = rgb(0x28C840);
    t.breakpoint = rgb(0x3D7EFF);

    const uint32_t ansi[16] = { 0x1E1F22, 0xFF6B68, 0x6BD968, 0xE5C07B, 0x61AFEF, 0xC678DD, 0x56B6C2, 0xC8CCD4,
                                0x5C6370, 0xFF8582, 0x8EE88B, 0xF2D08F, 0x82C1FF, 0xD99AEB, 0x7BD0DB, 0xFFFFFF };
    for (int i = 0; i < 16; ++i) t.ansi[i] = rgb(ansi[i]);
    t.termFg = rgb(0xD4D6DA);
    t.termBg = rgb(0x1A1B1E);
    return t;
}

Theme makeLight()
{
    Theme t;
    t.dark       = false;
    t.bg         = rgb(0xFFFFFF);
    t.panel      = rgb(0xF5F5F7);
    t.panelAlt   = rgb(0xEBEBEE);
    t.border     = rgb(0xD9D9DE);
    t.text       = rgb(0x1D1D1F);
    t.textDim    = rgb(0x6E6E73);
    t.accent     = rgb(0x0A64FF);
    t.accentText = rgb(0xFFFFFF);
    t.selection  = rgb(0xB3D3FF);
    t.hover      = rgb(0xE4E4E8);

    t.editorBg         = rgb(0xFFFFFF);
    t.gutterBg         = rgb(0xFFFFFF);
    t.gutterText       = rgb(0xA7A7AD);
    t.gutterTextActive = rgb(0x3A3A3C);
    t.currentLine      = rgb(0xF2F6FD);
    t.cursor           = rgb(0x0A64FF);
    t.findMatch        = rgb(0xFFE16B, 170);
    t.bracket          = rgb(0xD5DCE6);

    t.synKeyword  = rgb(0xAD3DA4);
    t.synType     = rgb(0x0B4F79);
    t.synString   = rgb(0xD12F1B);
    t.synNumber   = rgb(0x272AD8);
    t.synComment  = rgb(0x707F8C);
    t.synPreproc  = rgb(0x78492A);
    t.synFunction = rgb(0x326D74);
    t.synPunct    = rgb(0x3A3A3C);
    t.synDefault  = rgb(0x1D1D1F);

    t.error      = rgb(0xE0362C);
    t.warning    = rgb(0xC98A00);
    t.ok         = rgb(0x1E9E36);
    t.breakpoint = rgb(0x0A64FF);

    const uint32_t ansi[16] = { 0x1D1D1F, 0xC91B00, 0x00A600, 0xA68000, 0x0225C7, 0xB225B2, 0x00A6B2, 0x8E8E93,
                                0x5C5C61, 0xE73C2F, 0x1FB31F, 0xB39200, 0x3B5BDB, 0xC83BC8, 0x10B3BF, 0x1D1D1F };
    for (int i = 0; i < 16; ++i) t.ansi[i] = rgb(ansi[i]);
    t.termFg = rgb(0x1D1D1F);
    t.termBg = rgb(0xFBFBFD);
    return t;
}

Theme g_theme = makeDark();

ImVec4 v4(ImU32 c, float alpha = -1.0f)
{
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    if (alpha >= 0) v.w = alpha;
    return v;
}

} // namespace

const Theme& theme()
{
    return g_theme;
}

void setTheme(bool dark)
{
    g_theme = dark ? makeDark() : makeLight();
    const Theme& t = g_theme;

    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowPadding     = ImVec2(10, 8);
    s.FramePadding      = ImVec2(8, 4);
    s.ItemSpacing       = ImVec2(8, 6);
    s.ItemInnerSpacing  = ImVec2(6, 4);
    s.IndentSpacing     = 14;
    s.ScrollbarSize     = 10;
    s.GrabMinSize       = 8;
    s.WindowBorderSize  = 0;
    s.ChildBorderSize   = 0;
    s.PopupBorderSize   = 1;
    s.FrameBorderSize   = 0;
    s.TabBorderSize     = 0;
    s.WindowRounding    = 0;
    s.ChildRounding     = 0;
    s.FrameRounding     = 5;
    s.PopupRounding     = 8;
    s.ScrollbarRounding = 6;
    s.GrabRounding      = 4;
    s.TabRounding       = 5;
    s.SeparatorTextBorderSize = 1;

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]                 = v4(t.text);
    c[ImGuiCol_TextDisabled]         = v4(t.textDim);
    c[ImGuiCol_WindowBg]             = v4(t.panel);
    c[ImGuiCol_ChildBg]              = v4(t.panel, 0.0f);
    c[ImGuiCol_PopupBg]              = v4(t.panelAlt);
    c[ImGuiCol_Border]               = v4(t.border);
    c[ImGuiCol_BorderShadow]         = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg]              = v4(t.dark ? t.bg : t.panelAlt);
    c[ImGuiCol_FrameBgHovered]       = v4(t.hover);
    c[ImGuiCol_FrameBgActive]        = v4(t.hover);
    c[ImGuiCol_TitleBg]              = v4(t.panel);
    c[ImGuiCol_TitleBgActive]        = v4(t.panel);
    c[ImGuiCol_TitleBgCollapsed]     = v4(t.panel);
    c[ImGuiCol_MenuBarBg]            = v4(t.panel);
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]        = v4(t.textDim, 0.30f);
    c[ImGuiCol_ScrollbarGrabHovered] = v4(t.textDim, 0.50f);
    c[ImGuiCol_ScrollbarGrabActive]  = v4(t.textDim, 0.65f);
    c[ImGuiCol_CheckMark]            = v4(t.accent);
    c[ImGuiCol_SliderGrab]           = v4(t.accent);
    c[ImGuiCol_SliderGrabActive]     = v4(t.accent);
    c[ImGuiCol_Button]               = v4(t.panelAlt);
    c[ImGuiCol_ButtonHovered]        = v4(t.hover);
    c[ImGuiCol_ButtonActive]         = v4(t.border);
    c[ImGuiCol_Header]               = v4(t.selection, 0.55f);
    c[ImGuiCol_HeaderHovered]        = v4(t.hover);
    c[ImGuiCol_HeaderActive]         = v4(t.selection);
    c[ImGuiCol_Separator]            = v4(t.border);
    c[ImGuiCol_SeparatorHovered]     = v4(t.accent);
    c[ImGuiCol_SeparatorActive]      = v4(t.accent);
    c[ImGuiCol_ResizeGrip]           = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered]    = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripActive]     = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Tab]                  = v4(t.panel);
    c[ImGuiCol_TabHovered]           = v4(t.hover);
    c[ImGuiCol_TabSelected]          = v4(t.editorBg);
    c[ImGuiCol_TabSelectedOverline]  = v4(t.accent);
    c[ImGuiCol_TabDimmed]            = v4(t.panel);
    c[ImGuiCol_TabDimmedSelected]    = v4(t.editorBg);
    c[ImGuiCol_TabDimmedSelectedOverline] = v4(t.border);
    c[ImGuiCol_TableHeaderBg]        = v4(t.panelAlt);
    c[ImGuiCol_TableBorderStrong]    = v4(t.border);
    c[ImGuiCol_TableBorderLight]     = v4(t.border);
    c[ImGuiCol_TableRowBg]           = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]        = v4(t.panelAlt, 0.35f);
    c[ImGuiCol_TextSelectedBg]       = v4(t.selection);
    c[ImGuiCol_NavCursor]         = v4(t.accent);
    c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0, 0, 0, t.dark ? 0.45f : 0.25f);
}

ImU32 terminalColor(uint32_t packed, bool fg, bool bold)
{
    const Theme& t = g_theme;
    uint32_t type = packed >> 24;
    if (type == 0) return fg ? t.termFg : t.termBg;
    if (type == 2) return rgb(packed & 0xFFFFFF);
    int i = int(packed & 0xFF);
    if (i < 16)
    {
        if (fg && bold && i < 8) i += 8;
        return t.ansi[i];
    }
    if (i < 232)
    {
        i -= 16;
        static const int lv[6] = { 0, 95, 135, 175, 215, 255 };
        return IM_COL32(lv[(i / 36) % 6], lv[(i / 6) % 6], lv[i % 6], 255);
    }
    int g = 8 + (i - 232) * 10;
    return IM_COL32(g, g, g, 255);
}
