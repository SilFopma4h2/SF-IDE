#include "EditorView.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstring>

#include <imgui.h>
#include <imgui_internal.h>

#include "Syntax.h"
#include "Theme.h"
#include "Wake.h"

namespace {

bool isMac()
{
    return ImGui::GetIO().ConfigMacOSXBehaviors;
}

ImU32 tokenColor(syntax::Kind k)
{
    const Theme& t = theme();
    switch (k)
    {
    case syntax::Kind::Keyword:  return t.synKeyword;
    case syntax::Kind::Type:     return t.synType;
    case syntax::Kind::String:   return t.synString;
    case syntax::Kind::Number:   return t.synNumber;
    case syntax::Kind::Comment:  return t.synComment;
    case syntax::Kind::Preproc:  return t.synPreproc;
    case syntax::Kind::Function: return t.synFunction;
    case syntax::Kind::Punct:    return t.synPunct;
    default:                     return t.synDefault;
    }
}

std::string leadingWhitespace(std::string_view line)
{
    size_t n = 0;
    while (n < line.size() && (line[n] == ' ' || line[n] == '\t')) ++n;
    return std::string(line.substr(0, n));
}

bool usesTabs(const Document& doc)
{
    const std::string& t = doc.text();
    size_t tabs = 0, spaces = 0;
    for (size_t i = 0; i + 1 < t.size() && i < 200000; ++i)
    {
        if (t[i] != '\n') continue;
        if (t[i + 1] == '\t') ++tabs;
        else if (t[i + 1] == ' ') ++spaces;
    }
    return tabs > spaces;
}

const char* commentPrefix(const Document& doc)
{
    switch (doc.lang())
    {
    case Document::Lang::CFamily: return "//";
    case Document::Lang::Hash:    return "#";
    default:                      return nullptr;
    }
}

char lowerAscii(char c)
{
    return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
}

// Matching bracket for the one at `pos`, or npos.
size_t matchBracket(const std::string& t, size_t pos)
{
    if (pos >= t.size()) return std::string::npos;
    const char* open = "([{";
    const char* close = ")]}";
    char c = t[pos];
    const char* o = std::strchr(open, c);
    const char* cl = std::strchr(close, c);
    if (!c || (!o && !cl)) return std::string::npos;
    int depth = 0;
    const size_t limit = 50000;
    if (o)
    {
        char want = close[o - open];
        for (size_t i = pos, n = 0; i < t.size() && n < limit; ++i, ++n)
        {
            if (t[i] == c) ++depth;
            else if (t[i] == want && --depth == 0) return i;
        }
    }
    else
    {
        char want = open[cl - close];
        for (size_t i = pos + 1, n = 0; i-- > 0 && n < limit; ++n)
        {
            if (t[i] == c) ++depth;
            else if (t[i] == want && --depth == 0) return i;
        }
    }
    return std::string::npos;
}

std::string g_lineClipboard;  // text of the last whole-line copy

} // namespace

// --- Drawing ----------------------------------------------------------------------

bool EditorView::render(Document& doc)
{
    if (m_lastDoc != &doc)
    {
        m_lastDoc = &doc;
        m_dragging = false;
    }
    if (font) ImGui::PushFont(font);

    const Theme& th = theme();
    m_lineH = std::floor(ImGui::GetFontSize() * 1.5f);
    m_charW = ImGui::CalcTextSize("M").x;
    const int lineCount = doc.lineCount();
    int digits = 1;
    for (int n = lineCount; n >= 10; n /= 10) ++digits;
    digits = std::max(digits, 3);
    const float gutterW = m_charW * float(digits + 4);
    const float padTop = 6.0f;
    const float padLeft = m_charW * 1.0f;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(th.editorBg));
    char id[64];
    std::snprintf(id, sizeof id, "##editor_%p", static_cast<void*>(&doc));
    ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNavInputs |
                          ImGuiWindowFlags_NoNav);
    ImGuiWindow* win = ImGui::GetCurrentWindow();

    if (m_wantFocus)
    {
        ImGui::SetWindowFocus();
        m_wantFocus = false;
    }
    m_focused = ImGui::IsWindowFocused();

    const ImRect inner = win->InnerRect;
    const float viewH = inner.GetHeight();

    // Content size: scroll range for all lines plus room to scroll past the end.
    const float contentW = gutterW + padLeft + float(doc.maxColumns() + 8) * m_charW;
    const float contentH = padTop + float(lineCount) * m_lineH + std::max(0.0f, viewH - m_lineH * 4);
    ImGui::SetCursorPos(ImVec2(0, 0));
    ImGui::Dummy(ImVec2(contentW, contentH));

    // Input uses the geometry of the frame the user is looking at.
    if (m_focused) handleKeyboard(doc);
    handleMouse(doc);

    // Keep the cursor in view after keyboard actions.
    if (m_revealCursor)
    {
        m_revealCursor = false;
        int line = doc.lineOf(doc.cursor);
        float cy = padTop + float(line) * m_lineH;
        float cx = float(doc.columnOf(line, doc.cursor)) * m_charW;  // relative to the code start
        float codeW = inner.GetWidth() - gutterW - padLeft;
        float sy = win->Scroll.y, sx = win->Scroll.x;
        if (cy < sy + m_lineH) sy = cy - m_lineH * 2;
        else if (cy + m_lineH * 2 > sy + viewH) sy = cy + m_lineH * 3 - viewH;
        if (cx < sx + m_charW * 2) sx = cx - m_charW * 8;
        else if (cx > sx + codeW - m_charW * 4) sx = cx - codeW + m_charW * 8;
        // ImGui clamps to the scroll range on the next frame.
        win->Scroll.y = std::max(0.0f, sy);
        win->Scroll.x = std::max(0.0f, sx);
        wakeUi();
    }

    // Show a line changed by clil without moving the cursor.
    if (m_scrollLine > 0)
    {
        float ly = padTop + float(m_scrollLine - 1) * m_lineH;
        if (ly < win->Scroll.y || ly > win->Scroll.y + viewH - m_lineH * 2)
            win->Scroll.y = std::max(0.0f, ly - viewH * 0.35f);
        m_scrollLine = 0;
    }

    const float scrollX = win->Scroll.x, scrollY = win->Scroll.y;
    m_viewTop = inner.Min.y;
    m_viewBottom = inner.Max.y;
    m_viewLeft = inner.Min.x + gutterW;
    m_viewRight = inner.Max.x;
    m_gutterRight = inner.Min.x + gutterW;
    m_originX = inner.Min.x + gutterW + padLeft - scrollX;
    m_originY = inner.Min.y + padTop - scrollY;

    const int first = std::clamp(int((scrollY - padTop) / m_lineH), 0, std::max(0, lineCount - 1));
    const int last = std::clamp(int((scrollY + viewH) / m_lineH) + 1, 0, lineCount - 1);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int curLine = doc.lineOf(doc.cursor);
    const float textOffY = std::floor((m_lineH - ImGui::GetFontSize()) * 0.5f);
    auto lineY = [&](int l) { return m_originY + float(l) * m_lineH; };
    auto colX = [&](int c) { return m_originX + float(c) * m_charW; };

    // --- code area
    dl->PushClipRect(ImVec2(m_viewLeft, inner.Min.y), inner.Max, true);

    if (!doc.hasSelection())
        dl->AddRectFilled(ImVec2(inner.Min.x, lineY(curLine)), ImVec2(inner.Max.x, lineY(curLine) + m_lineH),
                          th.currentLine);
    // Lines just changed by clil fade out over a couple of seconds.
    {
        using namespace std::chrono;
        double t = duration<double>(steady_clock::now().time_since_epoch()).count();
        if (doc.flashUntil > t && doc.flashFrom > 0)
        {
            float alpha = float(std::min(1.0, (doc.flashUntil - t) / 1.5));
            ImU32 c = (th.accent & 0x00FFFFFF) | (ImU32(70 * alpha) << IM_COL32_A_SHIFT);
            int a = std::max(first, doc.flashFrom - 1), b = std::min(last, doc.flashTo - 1);
            for (int l = a; l <= b; ++l)
                dl->AddRectFilled(ImVec2(inner.Min.x, lineY(l)), ImVec2(inner.Max.x, lineY(l) + m_lineH), c);
            wakeUi(2);
        }
    }
    if (doc.execLine > 0)  // where the debugger stopped
        dl->AddRectFilled(ImVec2(inner.Min.x, lineY(doc.execLine - 1)), ImVec2(inner.Max.x, lineY(doc.execLine - 1) + m_lineH),
                          (th.ok & 0x00FFFFFF) | (0x38u << 24));

    // Matching bracket.
    {
        size_t a = matchBracket(doc.text(), doc.cursor);
        size_t at = doc.cursor;
        if (a == std::string::npos && doc.cursor > 0)
        {
            at = doc.cursor - 1;
            a = matchBracket(doc.text(), at);
        }
        if (a != std::string::npos)
        {
            for (size_t p : { at, a })
            {
                int l = doc.lineOf(p);
                if (l < first || l > last) continue;
                float x = colX(doc.columnOf(l, p));
                dl->AddRectFilled(ImVec2(x, lineY(l) + 2), ImVec2(x + m_charW, lineY(l) + m_lineH - 2), th.bracket, 3.0f);
            }
        }
    }

    // Selection.
    if (doc.hasSelection())
    {
        const size_t s0 = doc.selMin(), s1 = doc.selMax();
        const int l0 = std::max(first, doc.lineOf(s0));
        const int l1 = std::min(last, doc.lineOf(s1));
        for (int l = l0; l <= l1; ++l)
        {
            size_t b = std::max(s0, doc.lineStart(l));
            size_t e = std::min(s1, doc.lineEnd(l));
            float x0 = colX(doc.columnOf(l, b));
            float x1 = colX(doc.columnOf(l, e));
            if (s1 > doc.lineEnd(l)) x1 += m_charW * 0.6f;  // newline is selected too
            if (x1 > x0)
                dl->AddRectFilled(ImVec2(x0, lineY(l)), ImVec2(x1, lineY(l) + m_lineH), th.selection, 2.0f);
        }
    }

    // Find matches in the visible lines.
    if (!findText.empty())
    {
        for (int l = first; l <= last; ++l)
        {
            size_t b = doc.lineStart(l), e = doc.lineEnd(l);
            for (size_t p = b; p + findText.size() <= e; ++p)
            {
                if (!matchesAt(doc, p)) continue;
                float x0 = colX(doc.columnOf(l, p));
                float x1 = colX(doc.columnOf(l, p + findText.size()));
                dl->AddRectFilled(ImVec2(x0, lineY(l) + 1), ImVec2(x1, lineY(l) + m_lineH - 1), th.findMatch, 3.0f);
                p += findText.size() - 1;
            }
        }
    }

    // Text.
    const auto& blockState = doc.blockCommentState();
    std::vector<syntax::Token> tokens;
    std::string expanded;
    ImFont* f = ImGui::GetFont();
    const float fs = ImGui::GetFontSize();
    for (int l = first; l <= last; ++l)
    {
        std::string_view text = doc.lineText(l);
        syntax::tokenize(doc.lang(), text, blockState[size_t(l)] != 0, tokens);
        const float y = lineY(l) + textOffY;
        int col = 0;
        for (const auto& t : tokens)
        {
            // Expand tabs inside the token; columns are tracked across tokens.
            expanded.clear();
            int startCol = col;
            for (size_t i = t.begin; i < t.end; ++i)
            {
                char ch = text[i];
                if (ch == '\t')
                {
                    int next = (col / Document::kTabWidth + 1) * Document::kTabWidth;
                    expanded.append(size_t(next - col), ' ');
                    col = next;
                }
                else
                {
                    expanded.push_back(ch);
                    if ((static_cast<unsigned char>(ch) & 0xC0) != 0x80) ++col;
                }
            }
            float x = colX(startCol);
            if (x > m_viewRight) break;
            if (colX(col) < m_viewLeft) continue;
            dl->AddText(f, fs, ImVec2(x, y), tokenColor(t.kind), expanded.data(), expanded.data() + expanded.size());
        }
    }

    // Diagnostics: wavy underline from the reported column to the line end.
    for (const auto& d : doc.diagnostics)
    {
        int l = d.line - 1;
        if (l < first || l > last) continue;
        std::string_view text = doc.lineText(l);
        size_t startOff = doc.lineStart(l) + std::min<size_t>(size_t(std::max(0, d.col - 1)), text.size());
        if (d.col <= 0) startOff = doc.lineStart(l) + leadingWhitespace(text).size();
        float x0 = colX(doc.columnOf(l, startOff));
        float x1 = std::max(x0 + m_charW * 2, colX(doc.columnOf(l, doc.lineEnd(l))));
        float y = lineY(l) + m_lineH - 3;
        ImU32 col = d.error ? th.error : th.warning;
        for (float x = x0; x < x1; x += 4)
            dl->AddLine(ImVec2(x, y + ((int(x - x0) / 4) % 2 ? 1.5f : -0.5f)),
                        ImVec2(std::min(x + 4, x1), y + ((int(x - x0) / 4) % 2 ? -0.5f : 1.5f)), col, 1.2f);
    }

    // Cursor.
    {
        float x = colX(doc.columnOf(curLine, doc.cursor));
        float y = lineY(curLine);
        ImU32 c = m_focused ? th.cursor : (th.textDim & 0x80FFFFFF);
        dl->AddRectFilled(ImVec2(x - 0.5f, y + 2), ImVec2(x + 1.5f, y + m_lineH - 2), c);
    }
    dl->PopClipRect();

    // --- gutter (does not scroll horizontally)
    dl->PushClipRect(inner.Min, ImVec2(m_gutterRight, inner.Max.y), true);
    dl->AddRectFilled(inner.Min, ImVec2(m_gutterRight, inner.Max.y), th.gutterBg);
    std::vector<const Diagnostic*> lineDiag(size_t(last - first + 1), nullptr);
    for (const auto& d : doc.diagnostics)
    {
        int l = d.line - 1;
        if (l < first || l > last) continue;
        auto& slot = lineDiag[size_t(l - first)];
        if (!slot || (d.error && !slot->error)) slot = &d;
    }
    for (int l = first; l <= last; ++l)
    {
        char num[16];
        int len = std::snprintf(num, sizeof num, "%d", l + 1);
        float y = lineY(l);
        float right = m_gutterRight - m_charW * 1.5f;
        float x = right - float(len) * m_charW;
        bool bp = doc.breakpoints.count(l + 1) != 0;
        if (bp)
        {
            // Xcode-style breakpoint tag behind the line number.
            ImVec2 a(inner.Min.x + m_charW * 1.2f, y + 2), b(m_gutterRight - m_charW * 0.6f, y + m_lineH - 2);
            dl->AddRectFilled(a, ImVec2(b.x - m_lineH * 0.25f, b.y), th.breakpoint, 3.0f);
            ImVec2 tri[3] = { ImVec2(b.x - m_lineH * 0.25f - 1, a.y), b.x > a.x ? ImVec2(b.x, (a.y + b.y) * 0.5f) : b,
                              ImVec2(b.x - m_lineH * 0.25f - 1, b.y) };
            dl->AddTriangleFilled(tri[0], tri[1], tri[2], th.breakpoint);
        }
        ImU32 c = bp ? th.accentText : (l == curLine ? th.gutterTextActive : th.gutterText);
        dl->AddText(f, fs, ImVec2(x, y + textOffY), c, num, num + len);
        if (doc.execLine == l + 1)
        {
            float cx = inner.Min.x + m_charW * 0.9f, cy = y + m_lineH * 0.5f, r = m_lineH * 0.28f;
            dl->AddTriangleFilled(ImVec2(cx - r * 0.7f, cy - r), ImVec2(cx - r * 0.7f, cy + r), ImVec2(cx + r, cy), th.ok);
        }
        if (const Diagnostic* d = lineDiag[size_t(l - first)])
            dl->AddCircleFilled(ImVec2(inner.Min.x + m_charW * 0.7f, y + m_lineH * 0.5f), m_lineH * 0.17f,
                                d->error ? th.error : th.warning);
    }
    dl->PopClipRect();

    // Tooltip for diagnostics under the mouse.
    if (ImGui::IsWindowHovered() && !doc.diagnostics.empty())
    {
        ImVec2 m = ImGui::GetMousePos();
        int l = int(std::floor((m.y - m_originY) / m_lineH));
        bool any = false;
        for (const auto& d : doc.diagnostics)
        {
            if (d.line - 1 != l) continue;
            if (!any) { ImGui::BeginTooltip(); any = true; }
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(d.error ? th.error : th.warning));
            ImGui::TextUnformatted(d.error ? "error" : "warning");
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40);
            ImGui::TextUnformatted(d.message.c_str());
            ImGui::PopTextWrapPos();
        }
        if (any) ImGui::EndTooltip();
    }

    if (contextMenu)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
        if (font) ImGui::PopFont();  // menus use the UI font
        if (ImGui::BeginPopupContextWindow("##editor_ctx"))
        {
            contextMenu(doc);
            ImGui::EndPopup();
        }
        if (font) ImGui::PushFont(font);
        ImGui::PopStyleVar();
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if (font) ImGui::PopFont();
    return m_focused;
}

// --- Mouse -------------------------------------------------------------------------

size_t EditorView::posFromMouse(Document& doc) const
{
    ImVec2 m = ImGui::GetMousePos();
    int line = int(std::floor((m.y - m_originY) / m_lineH));
    if (line < 0) return 0;
    if (line >= doc.lineCount()) return doc.text().size();
    int col = std::max(0, int(std::floor((m.x - m_originX) / m_charW + 0.5f)));
    return doc.offsetAtColumn(line, col);
}

void EditorView::handleMouse(Document& doc)
{
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 m = ImGui::GetMousePos();
    const bool hovered = ImGui::IsWindowHovered() && m.y >= m_viewTop && m.y < m_viewBottom &&
                         m.x < m_viewRight;
    const bool inGutter = m.x < m_gutterRight;

    if (hovered && !inGutter) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        if (inGutter)
        {
            int line = int(std::floor((m.y - m_originY) / m_lineH));
            if (line >= 0 && line < doc.lineCount()) doc.toggleBreakpoint(line + 1);
            return;
        }
        size_t pos = posFromMouse(doc);
        int clicks = io.MouseClickedCount[ImGuiMouseButton_Left];
        doc.preferredCol = -1;
        if (clicks == 2)
        {
            size_t b, e;
            doc.wordAt(pos, b, e);
            doc.anchor = b;
            doc.cursor = e;
            m_dragWords = true;
            m_dragOrigin = b;
        }
        else if (clicks >= 3)
        {
            int l = doc.lineOf(pos);
            doc.anchor = doc.lineStart(l);
            doc.cursor = std::min(doc.text().size(), doc.lineEnd(l) + 1);
            m_dragWords = false;
        }
        else
        {
            doc.setCursor(pos, io.KeyShift);
            m_dragWords = false;
        }
        m_dragOrigin = doc.anchor;
        m_dragging = true;
    }

    if (m_dragging)
    {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            m_dragging = false;
            return;
        }
        // Auto-scroll while dragging outside the view.
        ImGuiWindow* win = ImGui::GetCurrentWindow();
        if (m.y < m_viewTop) win->Scroll.y = std::max(0.0f, win->Scroll.y - m_lineH);
        else if (m.y > m_viewBottom) win->Scroll.y = std::min(win->ScrollMax.y, win->Scroll.y + m_lineH);
        if (m.y < m_viewTop || m.y > m_viewBottom) wakeUi();

        size_t pos = posFromMouse(doc);
        if (m_dragWords)
        {
            size_t b, e;
            doc.wordAt(pos, b, e);
            size_t ob, oe;
            doc.wordAt(m_dragOrigin, ob, oe);
            if (pos < m_dragOrigin) { doc.anchor = oe; doc.cursor = b; }
            else                    { doc.anchor = ob; doc.cursor = e; }
        }
        else if (io.MouseClickedCount[ImGuiMouseButton_Left] < 3)
        {
            doc.cursor = pos;
        }
    }
}

// --- Keyboard ------------------------------------------------------------------------

void EditorView::moveVertical(Document& doc, int lines, bool extend)
{
    int line = doc.lineOf(doc.cursor);
    if (doc.preferredCol < 0) doc.preferredCol = doc.columnOf(line, doc.cursor);
    int target = line + lines;
    size_t pos;
    if (target < 0) pos = 0;
    else if (target >= doc.lineCount()) pos = doc.text().size();
    else pos = doc.offsetAtColumn(target, doc.preferredCol);
    int keep = doc.preferredCol;
    doc.setCursor(pos, extend);
    doc.preferredCol = keep;
}

void EditorView::newline(Document& doc)
{
    int line = doc.lineOf(doc.cursor);
    std::string_view text = doc.lineText(line);
    size_t col = doc.selMin() - doc.lineStart(line);
    std::string indent = leadingWhitespace(text.substr(0, std::min(col, text.size())));
    std::string unit = usesTabs(doc) ? "\t" : std::string(Document::kTabWidth, ' ');

    // Character before the cursor (ignoring spaces) and the one after it.
    size_t p = doc.selMin();
    while (p > doc.lineStart(line) && (doc.text()[p - 1] == ' ' || doc.text()[p - 1] == '\t')) --p;
    char before = p > doc.lineStart(line) ? doc.text()[p - 1] : 0;
    char after = doc.selMax() < doc.text().size() ? doc.text()[doc.selMax()] : 0;

    bool opens = (before == '{' || before == '(' || before == '[') ||
                 (before == ':' && doc.lang() == Document::Lang::Hash);
    if (opens && doc.lang() != Document::Lang::Plain && doc.lang() != Document::Lang::Markdown)
    {
        bool closes = (before == '{' && after == '}') || (before == '(' && after == ')') ||
                      (before == '[' && after == ']');
        if (closes)
        {
            std::string s = "\n" + indent + unit;
            doc.insert(s + "\n" + indent, Document::EditKind::Other);
            doc.setCursor(doc.cursor - indent.size() - 1, false);
            return;
        }
        indent += unit;
    }
    doc.insert("\n" + indent, Document::EditKind::Other);
}

void EditorView::handleKeyboard(Document& doc)
{
    ImGuiIO& io = ImGui::GetIO();
    const bool mac = isMac();
    const bool cmd = io.KeyCtrl;               // Cmd on macOS, Ctrl elsewhere
    const bool shift = io.KeyShift;
    const bool alt = io.KeyAlt;
    const bool word = mac ? alt : cmd;
    auto pressed = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };
    bool moved = false, edited = false;

    auto horizontal = [&](size_t pos) {
        doc.preferredCol = -1;
        doc.setCursor(pos, shift);
        moved = true;
    };
    auto lineHome = [&]() {
        int l = doc.lineOf(doc.cursor);
        size_t b = doc.lineStart(l);
        size_t ind = b + leadingWhitespace(doc.lineText(l)).size();
        return doc.cursor == ind ? b : ind;  // smart home: indentation first
    };

    if (pressed(ImGuiKey_LeftArrow))
    {
        if (doc.hasSelection() && !shift && !word && !(mac && cmd)) horizontal(doc.selMin());
        else if (mac && cmd) horizontal(lineHome());
        else if (word) horizontal(doc.wordLeft(doc.cursor));
        else horizontal(doc.prevChar(doc.cursor));
    }
    if (pressed(ImGuiKey_RightArrow))
    {
        if (doc.hasSelection() && !shift && !word && !(mac && cmd)) horizontal(doc.selMax());
        else if (mac && cmd) horizontal(doc.lineEnd(doc.lineOf(doc.cursor)));
        else if (word) horizontal(doc.wordRight(doc.cursor));
        else horizontal(doc.nextChar(doc.cursor));
    }
    if (pressed(ImGuiKey_UpArrow))
    {
        if (mac && cmd) horizontal(0);
        else { moveVertical(doc, -1, shift); moved = true; }
    }
    if (pressed(ImGuiKey_DownArrow))
    {
        if (mac && cmd) horizontal(doc.text().size());
        else { moveVertical(doc, +1, shift); moved = true; }
    }
    if (pressed(ImGuiKey_Home)) horizontal(cmd ? 0 : lineHome());
    if (pressed(ImGuiKey_End)) horizontal(cmd ? doc.text().size() : doc.lineEnd(doc.lineOf(doc.cursor)));
    const int page = std::max(1, int((m_viewBottom - m_viewTop) / m_lineH) - 2);
    if (pressed(ImGuiKey_PageUp))
    {
        moveVertical(doc, -page, shift);
        ImGui::SetScrollY(std::max(0.0f, ImGui::GetScrollY() - float(page) * m_lineH));
        moved = true;
    }
    if (pressed(ImGuiKey_PageDown))
    {
        moveVertical(doc, page, shift);
        ImGui::SetScrollY(ImGui::GetScrollY() + float(page) * m_lineH);
        moved = true;
    }

    if (pressed(ImGuiKey_Backspace))
    {
        if (doc.hasSelection()) doc.backspace();
        else if (mac && cmd) doc.deleteRange(doc.lineStart(doc.lineOf(doc.cursor)), doc.cursor);
        else if (word) doc.deleteRange(doc.wordLeft(doc.cursor), doc.cursor);
        else
        {
            // Inside leading spaces: delete back to the previous tab stop.
            int l = doc.lineOf(doc.cursor);
            size_t b = doc.lineStart(l);
            std::string_view before(doc.text().data() + b, doc.cursor - b);
            if (!before.empty() && before.find_first_not_of(' ') == std::string_view::npos)
            {
                size_t n = before.size() % Document::kTabWidth;
                if (n == 0) n = Document::kTabWidth;
                doc.replace(doc.cursor - n, doc.cursor, "", Document::EditKind::Delete);
            }
            else
            {
                doc.backspace();
            }
        }
        edited = true;
    }
    if (pressed(ImGuiKey_Delete))
    {
        if (word && !doc.hasSelection()) doc.deleteRange(doc.cursor, doc.wordRight(doc.cursor));
        else doc.deleteForward();
        edited = true;
    }
    if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter))
    {
        if (!cmd) { newline(doc); edited = true; }
    }
    if (pressed(ImGuiKey_Tab) && !cmd)
    {
        bool multiLine = doc.hasSelection() && doc.lineOf(doc.selMin()) != doc.lineOf(doc.selMax());
        if (shift || multiLine) indent(doc, shift);
        else if (usesTabs(doc)) doc.insert("\t");
        else
        {
            int col = doc.columnOf(doc.lineOf(doc.cursor), doc.selMin());
            doc.insert(std::string(size_t(Document::kTabWidth - col % Document::kTabWidth), ' '));
        }
        edited = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && doc.hasSelection())
    {
        doc.setCursor(doc.cursor, false);
    }

    if (cmd && !alt)
    {
        if (ImGui::IsKeyPressed(ImGuiKey_A, false)) { doc.selectAll(); }
        if (ImGui::IsKeyPressed(ImGuiKey_C, false)) copy(doc);
        if (ImGui::IsKeyPressed(ImGuiKey_X, false)) { cut(doc); edited = true; }
        if (ImGui::IsKeyPressed(ImGuiKey_V, true)) { paste(doc); edited = true; }
        if (ImGui::IsKeyPressed(ImGuiKey_Z, true)) { shift ? doc.redo() : doc.undo(); edited = true; }
        if (!mac && ImGui::IsKeyPressed(ImGuiKey_Y, true)) { doc.redo(); edited = true; }
        if (ImGui::IsKeyPressed(ImGuiKey_D, false)) { duplicateLine(doc); edited = true; }
        if (ImGui::IsKeyPressed(ImGuiKey_Slash, false)) { toggleComment(doc); edited = true; }
        if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, true)) { indent(doc, false); edited = true; }
        if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, true)) { indent(doc, true); edited = true; }
    }

    // Typed text. Cmd / Ctrl combinations are shortcuts, not text.
    if (!cmd && !(mac && io.KeySuper) && io.InputQueueCharacters.Size > 0)
    {
        std::string typed;
        for (ImWchar c : io.InputQueueCharacters)
        {
            if (c < 32 || c == 127) continue;
            char buf[8];
            typed += ImTextCharToUtf8(buf, c);
        }
        if (!typed.empty())
        {
            int l = doc.lineOf(doc.cursor);
            size_t b = doc.lineStart(l);
            std::string_view before(doc.text().data() + b, doc.selMin() - b);
            bool onlySpace = !before.empty() && before.find_first_not_of(" \t") == std::string_view::npos;
            if (typed == "}" && onlySpace && !doc.hasSelection() && doc.lang() == Document::Lang::CFamily)
            {
                // Outdent a closing brace typed on an indented empty line.
                size_t cut = before.back() == '\t' ? 1 : std::min<size_t>(Document::kTabWidth, before.size());
                doc.replace(doc.cursor - cut, doc.cursor, "}", Document::EditKind::Insert);
            }
            else
            {
                doc.insert(typed, Document::EditKind::Insert);
            }
            edited = true;
        }
        io.InputQueueCharacters.resize(0);
    }

    if (moved || edited) m_revealCursor = true;
}

// --- Commands ---------------------------------------------------------------------------

void EditorView::gotoLine(Document& doc, int line1, int col1)
{
    int l = std::clamp(line1 - 1, 0, doc.lineCount() - 1);
    size_t pos = std::min(doc.lineStart(l) + size_t(std::max(0, col1 - 1)), doc.lineEnd(l));
    doc.preferredCol = -1;
    doc.setCursor(pos, false);
    m_revealCursor = true;
    m_wantFocus = true;
    wakeUi();
}

void EditorView::copy(Document& doc)
{
    if (doc.hasSelection())
    {
        ImGui::SetClipboardText(doc.selectedText().c_str());
        g_lineClipboard.clear();
        return;
    }
    int l = doc.lineOf(doc.cursor);
    g_lineClipboard = std::string(doc.lineText(l)) + "\n";
    ImGui::SetClipboardText(g_lineClipboard.c_str());
}

void EditorView::cut(Document& doc)
{
    if (doc.hasSelection())
    {
        copy(doc);
        doc.replace(doc.selMin(), doc.selMax(), "");
        return;
    }
    copy(doc);
    int l = doc.lineOf(doc.cursor);
    size_t b = doc.lineStart(l);
    size_t e = std::min(doc.text().size(), doc.lineEnd(l) + 1);
    doc.replace(b, e, "");
}

void EditorView::paste(Document& doc)
{
    const char* clip = ImGui::GetClipboardText();
    if (!clip || !*clip) return;
    std::string s;
    for (const char* p = clip; *p; ++p)
    {
        if (*p == '\r') { s.push_back('\n'); if (p[1] == '\n') ++p; }
        else s.push_back(*p);
    }
    if (!doc.hasSelection() && !g_lineClipboard.empty() && s == g_lineClipboard)
    {
        // A whole line copied without a selection is pasted above the current line.
        size_t col = doc.cursor - doc.lineStart(doc.lineOf(doc.cursor));
        size_t b = doc.lineStart(doc.lineOf(doc.cursor));
        doc.replace(b, b, s);
        doc.setCursor(doc.cursor + col, false);
        return;
    }
    doc.insert(s, Document::EditKind::Other);
}

void EditorView::indent(Document& doc, bool outdent)
{
    size_t s0 = doc.selMin(), s1 = doc.selMax();
    int l0 = doc.lineOf(s0), l1 = doc.lineOf(s1);
    if (l1 > l0 && s1 == doc.lineStart(l1)) --l1;
    const bool tabs = usesTabs(doc);
    const std::string unit = tabs ? "\t" : std::string(Document::kTabWidth, ' ');
    bool hadSelection = doc.hasSelection();
    size_t cursorCol = doc.cursor - doc.lineStart(doc.lineOf(doc.cursor));

    std::string out;
    int firstDelta = 0;
    for (int l = l0; l <= l1; ++l)
    {
        std::string_view t = doc.lineText(l);
        std::string line(t);
        if (outdent)
        {
            size_t n = 0;
            if (!line.empty() && line[0] == '\t') n = 1;
            else while (n < line.size() && n < size_t(Document::kTabWidth) && line[n] == ' ') ++n;
            line.erase(0, n);
            if (l == l0) firstDelta = -int(n);
        }
        else if (!line.empty() || l0 == l1)
        {
            line = unit + line;
            if (l == l0) firstDelta = int(unit.size());
        }
        out += line;
        if (l < l1) out += "\n";
    }
    size_t b = doc.lineStart(l0), e = doc.lineEnd(l1);
    doc.replace(b, e, out);
    if (hadSelection)
    {
        doc.anchor = b;
        doc.cursor = b + out.size();
    }
    else
    {
        doc.setCursor(b + size_t(std::max(0, int(cursorCol) + firstDelta)), false);
    }
}

void EditorView::toggleComment(Document& doc)
{
    const char* prefix = commentPrefix(doc);
    if (!prefix) return;
    const std::string pre = prefix;
    size_t s0 = doc.selMin(), s1 = doc.selMax();
    int l0 = doc.lineOf(s0), l1 = doc.lineOf(s1);
    if (l1 > l0 && s1 == doc.lineStart(l1)) --l1;

    bool allCommented = true;
    size_t minIndent = std::string::npos;
    for (int l = l0; l <= l1; ++l)
    {
        std::string_view t = doc.lineText(l);
        size_t ind = t.find_first_not_of(" \t");
        if (ind == std::string_view::npos) continue;
        minIndent = std::min(minIndent, ind);
        if (t.substr(ind, pre.size()) != pre) allCommented = false;
    }
    if (minIndent == std::string::npos) return;

    std::string out;
    for (int l = l0; l <= l1; ++l)
    {
        std::string line(doc.lineText(l));
        size_t ind = line.find_first_not_of(" \t");
        if (ind != std::string::npos)
        {
            if (allCommented)
            {
                size_t n = pre.size() + (ind + pre.size() < line.size() && line[ind + pre.size()] == ' ' ? 1 : 0);
                line.erase(ind, n);
            }
            else
            {
                line.insert(minIndent, pre + " ");
            }
        }
        out += line;
        if (l < l1) out += "\n";
    }
    size_t b = doc.lineStart(l0), e = doc.lineEnd(l1);
    bool hadSelection = doc.hasSelection();
    doc.replace(b, e, out);
    if (hadSelection) { doc.anchor = b; doc.cursor = b + out.size(); }
    else doc.setCursor(std::min(doc.lineEnd(l0), b + out.size()), false);
}

void EditorView::duplicateLine(Document& doc)
{
    if (doc.hasSelection())
    {
        std::string s = doc.selectedText();
        size_t e = doc.selMax();
        doc.replace(e, e, s);
        doc.anchor = e;
        doc.cursor = e + s.size();
        return;
    }
    int l = doc.lineOf(doc.cursor);
    size_t col = doc.cursor - doc.lineStart(l);
    std::string line(doc.lineText(l));
    size_t e = doc.lineEnd(l);
    doc.replace(e, e, "\n" + line);
    doc.setCursor(doc.lineStart(l + 1) + col, false);
}

// --- Find --------------------------------------------------------------------------------

bool EditorView::matchesAt(const Document& doc, size_t pos) const
{
    const std::string& t = doc.text();
    if (findText.empty() || pos + findText.size() > t.size()) return false;
    if (findCaseSensitive) return t.compare(pos, findText.size(), findText) == 0;
    for (size_t i = 0; i < findText.size(); ++i)
        if (lowerAscii(t[pos + i]) != lowerAscii(findText[i])) return false;
    return true;
}

size_t EditorView::findFrom(const Document& doc, size_t from, bool backwards) const
{
    const size_t n = doc.text().size();
    if (findText.empty() || findText.size() > n) return std::string::npos;
    const size_t lastStart = n - findText.size();
    if (!backwards)
    {
        for (size_t i = 0; i <= lastStart; ++i)
        {
            size_t p = (from + i) % (lastStart + 1);
            if (matchesAt(doc, p)) return p;
        }
    }
    else
    {
        for (size_t i = 1; i <= lastStart + 1; ++i)
        {
            size_t p = (from + (lastStart + 1) * 2 - i) % (lastStart + 1);
            if (matchesAt(doc, p)) return p;
        }
    }
    return std::string::npos;
}

bool EditorView::findNext(Document& doc, bool backwards)
{
    size_t from = backwards ? doc.selMin() : doc.selMax();
    size_t p = findFrom(doc, from, backwards);
    if (p == std::string::npos) return false;
    doc.anchor = p;
    doc.cursor = p + findText.size();
    doc.preferredCol = -1;
    m_revealCursor = true;
    wakeUi();
    return true;
}

int EditorView::countMatches(const Document& doc) const
{
    if (findText.empty()) return 0;
    int n = 0;
    const size_t size = doc.text().size();
    for (size_t p = 0; p + findText.size() <= size; ++p)
    {
        if (matchesAt(doc, p))
        {
            ++n;
            p += findText.size() - 1;
        }
    }
    return n;
}

void EditorView::replaceCurrent(Document& doc, const std::string& with)
{
    if (doc.hasSelection() && doc.selMax() - doc.selMin() == findText.size() && matchesAt(doc, doc.selMin()))
        doc.insert(with, Document::EditKind::Other);
    findNext(doc, false);
}

int EditorView::replaceAll(Document& doc, const std::string& with)
{
    if (findText.empty()) return 0;
    std::string out;
    const std::string& t = doc.text();
    int n = 0;
    size_t i = 0;
    while (i < t.size())
    {
        if (matchesAt(doc, i))
        {
            out += with;
            i += findText.size();
            ++n;
        }
        else
        {
            out.push_back(t[i++]);
        }
    }
    if (n > 0)
    {
        size_t keep = doc.cursor;
        doc.replace(0, t.size(), out);
        doc.setCursor(std::min(keep, doc.text().size()), false);
    }
    return n;
}
