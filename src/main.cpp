// SF-IDE - minimal native C++ IDE for Windows.
// Phases 1-4: window, editor, build, debugger, terminal, file explorer.

#include <Windows.h>
#include <commdlg.h>
#include <d3d11.h>
#include <dxgi.h>
#include <shlobj.h>
#include <shellapi.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "resource.h"
#include "Editor.h"
#include "BuildRunner.h"
#include "DebugSession.h"
#include "Terminal.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

// Mark this binary as using the v6 common controls so the modern file
// dialog and Explorer tree look correct on Windows 11.
#pragma comment(linker, "/manifestdependency:\"type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// --- Direct3D 11 plumbing ---------------------------------------------------

static ID3D11Device*            g_pd3dDevice        = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain        = nullptr;
static ID3D11RenderTargetView*  g_pRenderTarget     = nullptr;

static bool CreateDeviceD3D(HWND hWnd);
static void CleanupDeviceD3D();
static void CreateRenderTarget();
static void CleanupRenderTarget();

static bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount       = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow      = hWnd;
    sd.SampleDesc.Count  = 1;
    sd.Windowed          = TRUE;
    sd.SwapEffect        = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        &featureLevel, 1, D3D11_SDK_VERSION,
        &sd, &g_pSwapChain, &g_pd3dDevice, nullptr, &g_pd3dDeviceContext);
    if (FAILED(hr))
        return false;

    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain)        { g_pSwapChain->Release();        g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice)        { g_pd3dDevice->Release();        g_pd3dDevice = nullptr; }
}

static void CreateRenderTarget()
{
    ID3D11Texture2D* pBackBuffer = nullptr;
    if (SUCCEEDED(g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer))))
    {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_pRenderTarget);
        pBackBuffer->Release();
    }
}

static void CleanupRenderTarget()
{
    if (g_pRenderTarget) { g_pRenderTarget->Release(); g_pRenderTarget = nullptr; }
}

// --- Win32 message handling -------------------------------------------------

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, LPARAM(lParam)))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED)
        {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam),
                                        DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// --- File dialog helpers ----------------------------------------------------

namespace fs = std::filesystem;

static std::string openFileDialog(HWND owner, const wchar_t* filter)
{
    wchar_t filename[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = owner;
    ofn.lpstrFile   = filename;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrFilter = filter;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&ofn))
    {
        int len = WideCharToMultiByte(CP_UTF8, 0, filename, -1, nullptr, 0,
                                      nullptr, nullptr);
        std::string out(static_cast<std::size_t>(len > 0 ? len - 1 : 0), '\0');
        if (len > 0)
            WideCharToMultiByte(CP_UTF8, 0, filename, -1, &out[0], len,
                                nullptr, nullptr);
        return out;
    }
    return {};
}

static std::string saveFileDialog(HWND owner, const wchar_t* filter, const wchar_t* defaultExt)
{
    wchar_t filename[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize   = sizeof(ofn);
    ofn.hwndOwner     = owner;
    ofn.lpstrFile     = filename;
    ofn.nMaxFile      = MAX_PATH;
    ofn.lpstrFilter   = filter;
    ofn.lpstrDefExt   = defaultExt;
    ofn.Flags         = OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY;
    if (GetSaveFileNameW(&ofn))
    {
        int len = WideCharToMultiByte(CP_UTF8, 0, filename, -1, nullptr, 0,
                                      nullptr, nullptr);
        std::string out(static_cast<std::size_t>(len > 0 ? len - 1 : 0), '\0');
        if (len > 0)
            WideCharToMultiByte(CP_UTF8, 0, filename, -1, &out[0], len,
                                nullptr, nullptr);
        return out;
    }
    return {};
}

static std::string pickFolderDialog(HWND owner)
{
    // Use the modern IFileOpenDialog so we get a proper Vista+ folder picker.
    IFileOpenDialog* p = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&p));
    if (FAILED(hr) || !p) return {};
    DWORD opts = 0;
    p->GetOptions(&opts);
    p->SetOptions(opts | FOS_PICKFOLDERS);
    hr = p->Show(owner);
    if (FAILED(hr)) { p->Release(); return {}; }
    IShellItem* item = nullptr;
    hr = p->GetResult(&item);
    std::string out;
    if (SUCCEEDED(hr) && item)
    {
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path)
        {
            int len = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0,
                                          nullptr, nullptr);
            out.assign(static_cast<std::size_t>(len > 0 ? len - 1 : 0), '\0');
            if (len > 0)
                WideCharToMultiByte(CP_UTF8, 0, path, -1, &out[0], len,
                                    nullptr, nullptr);
            CoTaskMemFree(path);
        }
        item->Release();
    }
    p->Release();
    return out;
}

// --- Syntax highlighting ----------------------------------------------------

namespace syntax {

enum class Kind { Default, Keyword, String, CharLiteral, Comment, Number, Preproc };

struct Token { std::size_t begin; std::size_t end; Kind kind; };

static bool isIdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
static bool isIdentChar (char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

static bool isKeyword(std::string_view w)
{
    static const char* kws[] = {
        "alignas","alignof","auto","bool","break","case","catch","char","class",
        "const","constexpr","continue","decltype","default","delete","do","double",
        "else","enum","explicit","export","extern","false","float","for","friend",
        "goto","if","inline","int","long","mutable","namespace","new","noexcept",
        "nullptr","operator","private","protected","public","register","reinterpret_cast",
        "return","short","signed","sizeof","static","static_assert","static_cast",
        "struct","switch","template","this","thread_local","throw","true","try",
        "typedef","typeid","typename","union","unsigned","using","virtual","void",
        "volatile","wchar_t","while"
    };
    for (auto* k : kws) if (w == k) return true;
    return false;
}

// Tokenise a single line (no embedded newlines; multi-line comments handled
// by tracking `inBlockComment` across lines).
static void tokeniseLine(std::string_view line,
                         std::vector<Token>& out,
                         bool& inBlockComment)
{
    std::size_t i = 0;
    const std::size_t n = line.size();

    if (inBlockComment)
    {
        std::size_t end = std::string_view::npos;
        for (std::size_t k = 0; k + 1 < n; ++k)
        {
            if (line[k] == '*' && line[k + 1] == '/') { end = k + 2; break; }
        }
        if (end == std::string_view::npos) end = n;
        out.push_back({0, end, Kind::Comment});
        if (end < n) { inBlockComment = false; i = end; }
        else return;
    }

    while (i < n)
    {
        char c = line[i];
        if (c == '/' && i + 1 < n && line[i + 1] == '/')
        {
            out.push_back({i, n, Kind::Comment});
            return;
        }
        if (c == '/' && i + 1 < n && line[i + 1] == '*')
        {
            std::size_t j = i + 2;
            while (j + 1 < n && !(line[j] == '*' && line[j + 1] == '/')) ++j;
            std::size_t end = (j + 1 < n) ? j + 2 : n;
            out.push_back({i, end, Kind::Comment});
            if (end == n) { inBlockComment = true; return; }
            i = end; continue;
        }
        if (c == '#')
        {
            std::size_t j = i + 1;
            while (j < n && std::isspace(static_cast<unsigned char>(line[j]))) ++j;
            std::size_t k = j;
            while (k < n && isIdentChar(line[k])) ++k;
            out.push_back({i, j, Kind::Preproc});
            if (k > j) out.push_back({j, k, Kind::Keyword});
            i = k; continue;
        }
        if (c == '"')
        {
            std::size_t j = i + 1;
            while (j < n && line[j] != '"')
            {
                if (line[j] == '\\' && j + 1 < n) ++j;
                ++j;
            }
            std::size_t end = (j < n) ? j + 1 : n;
            out.push_back({i, end, Kind::String});
            i = end; continue;
        }
        if (c == '\'')
        {
            std::size_t j = i + 1;
            while (j < n && line[j] != '\'')
            {
                if (line[j] == '\\' && j + 1 < n) ++j;
                ++j;
            }
            std::size_t end = (j < n) ? j + 1 : n;
            out.push_back({i, end, Kind::CharLiteral});
            i = end; continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)))
        {
            std::size_t j = i;
            while (j < n && (std::isalnum(static_cast<unsigned char>(line[j])) || line[j] == '.' || line[j] == 'x' || line[j] == 'X'))
                ++j;
            out.push_back({i, j, Kind::Number});
            i = j; continue;
        }
        if (isIdentStart(c))
        {
            std::size_t j = i + 1;
            while (j < n && isIdentChar(line[j])) ++j;
            std::string_view word = line.substr(i, j - i);
            if (isKeyword(word)) out.push_back({i, j, Kind::Keyword});
            i = j; continue;
        }
        ++i;
    }
}

static ImVec4 colorFor(Kind k)
{
    switch (k)
    {
    case Kind::Keyword:    return ImVec4(0.45f, 0.78f, 1.00f, 1.0f);
    case Kind::String:     return ImVec4(1.00f, 0.78f, 0.45f, 1.0f);
    case Kind::CharLiteral:return ImVec4(1.00f, 0.78f, 0.45f, 1.0f);
    case Kind::Comment:    return ImVec4(0.55f, 0.60f, 0.55f, 1.0f);
    case Kind::Number:     return ImVec4(0.90f, 0.65f, 1.00f, 1.0f);
    case Kind::Preproc:    return ImVec4(0.80f, 0.45f, 0.80f, 1.0f);
    default:               return ImVec4(0.90f, 0.90f, 0.90f, 1.0f);
    }
}

} // namespace syntax

// --- Editor UI --------------------------------------------------------------

struct EditorUI {
    TextEdit edit;
    // Debug integration: when set, the editor gutter becomes clickable so
    // the user can toggle a breakpoint by clicking on a line number. The
    // gutter also renders a small red dot next to lines that already have
    // a breakpoint.
    DebugSession* debugger = nullptr;

    bool openDialog(HWND owner) {
        std::string path = openFileDialog(owner,
            L"C/C++ source (*.cpp;*.c;*.h;*.hpp)\0*.cpp;*.c;*.h;*.hpp\0All files (*.*)\0*.*\0");
        if (path.empty()) return false;
        if (!edit.loadFromFile(path))
        {
            MessageBoxA(owner, ("Failed to open: " + path).c_str(), "SF-IDE", MB_OK | MB_ICONERROR);
            return false;
        }
        return true;
    }

    bool saveAsDialog(HWND owner) {
        std::string path = saveFileDialog(owner,
            L"C/C++ source (*.cpp;*.c;*.h;*.hpp)\0*.cpp;*.c;*.h;*.hpp\0All files (*.*)\0*.*\0",
            L"cpp");
        if (path.empty()) return false;
        if (!edit.saveAs(path))
        {
            MessageBoxA(owner, ("Failed to save: " + path).c_str(), "SF-IDE", MB_OK | MB_ICONERROR);
            return false;
        }
        return true;
    }

    // Variant for callers that already opened an ImGui::Begin("...") with
    // the editor's window name; renders just the text area inside it.
    void renderTextAreaOnly()
    {
        renderTextArea();
    }

private:
    // Render the editor. The Editor is a single child window; we draw the
    // gutter (line numbers + breakpoint dot) and the code area ourselves
    // using ImDrawList, then handle keyboard / mouse input from the same
    // window. Layout: [ gutter | code ]. Both columns scroll together
    // because they live in a single horizontal child.
    void renderTextArea()
    {
        const float lineHeight = ImGui::GetTextLineHeight();
        const float charW      = ImGui::CalcTextSize("0").x;
        const int   lc         = edit.lineCount();
        const std::string lcStr = std::to_string(lc);
        int digits = (lcStr.size() > 3) ? static_cast<int>(lcStr.size()) : 3;

        // Reserve a column on the left for the breakpoint dot.
        const float bpColW    = lineHeight * 0.8f;
        const float gutterW   = bpColW + charW * (digits + 2);
        const float padY      = 2.0f;
        // Empty file still shows one line.
        const int   visibleLines = (lc > 0) ? lc : 1;
        const float totalH      = lineHeight * visibleLines + padY * 2.0f;
        // Pick a generous code width: assume the file is mostly ASCII and
        // ~120 columns wide; if the file has a longer line the horizontal
        // scrollbar will let the user reach it. A min of 80 keeps short
        // files from looking cramped.
        auto lines = edit.lines();
        std::size_t maxLineLen = 0;
        for (const auto& ln : lines) if (ln.size() > maxLineLen) maxLineLen = ln.size();
        const float codeCols = static_cast<float>((maxLineLen + 4 > 80) ? (maxLineLen + 4) : 80);
        const float codeW    = charW * codeCols;
        const float totalW   = gutterW + codeW;

        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,  ImVec2(0, 0));

        // Single scrollable child. Border + horizontal scrollbar so very
        // long lines are still reachable.
        ImGui::BeginChild("##editor_scroll", avail, true,
                          ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::Dummy(ImVec2(totalW, totalH));   // defines scroll range

        // ---- All draw work below is in the foreground draw list (no
        // nested children), so positions are absolute screen coords. ----
        ImDrawList* dl = ImGui::GetWindowDrawList();
        // Top-left of the dummy = top-left of the editor canvas.
        ImVec2 canvas = ImGui::GetItemRectMin();
        ImVec2 codeTL = ImVec2(canvas.x + gutterW, canvas.y);

        // Tokenise once per frame.
        std::vector<std::vector<syntax::Token>> perLine(visibleLines);
        bool inBlock = false;
        for (int i = 0; i < visibleLines; ++i)
            syntax::tokeniseLine(lines[i], perLine[i], inBlock);

        // Selection background.
        if (edit.hasSelection())
        {
            int aLine, aCol, bLine, bCol;
            edit.offsetToLineCol(edit.selectionMin(), aLine, aCol);
            edit.offsetToLineCol(edit.selectionMax(), bLine, bCol);
            for (int ln = aLine; ln <= bLine; ++ln)
            {
                int c0 = (ln == aLine) ? aCol : 1;
                int c1;
                if (ln == bLine) c1 = bCol;
                else             c1 = static_cast<int>(lines[ln - 1].size()) + 1;
                if (c0 == c1) continue;
                float x0 = codeTL.x + (c0 - 1) * charW;
                float x1 = codeTL.x + (c1 - 1) * charW;
                float y0 = codeTL.y + (ln - 1) * lineHeight;
                float y1 = y0 + lineHeight;
                dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1),
                                  IM_COL32(60, 100, 160, 110));
            }
        }

        // Gutter: line numbers + breakpoint dots.
        const std::string srcFile = edit.filePath;
        for (int i = 0; i <= visibleLines; ++i)
        {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%*d", digits, i + 1);
            ImVec2 p(canvas.x + bpColW, canvas.y + i * lineHeight);
            dl->AddText(p, IM_COL32(120, 130, 140, 255), buf);
            if (debugger && !srcFile.empty() && i < visibleLines &&
                debugger->hasBreakpoint(srcFile, i + 1))
            {
                float cx = canvas.x + bpColW * 0.5f;
                float cy = canvas.y + i * lineHeight + lineHeight * 0.5f;
                float r  = lineHeight * 0.25f;
                dl->AddCircleFilled(ImVec2(cx, cy), r, IM_COL32(220, 80, 80, 255));
            }
        }

        // Code: tokens.
        for (int i = 0; i < visibleLines; ++i)
        {
            const float y = codeTL.y + i * lineHeight;
            // Plain text baseline first so un-tokenised regions (whitespace
            // etc.) are still visible.
            dl->AddText(ImVec2(codeTL.x, y), IM_COL32(220, 220, 220, 255),
                        std::string(lines[i]).c_str());
            for (const auto& t : perLine[i])
            {
                if (t.kind == syntax::Kind::Default) continue;
                if (t.begin >= lines[i].size()) continue;
                std::size_t len = (t.end < lines[i].size() ? t.end : lines[i].size()) - t.begin;
                if (len == 0) continue;
                std::string piece(lines[i].substr(t.begin, len));
                ImVec2 p(codeTL.x + t.begin * charW, y);
                dl->AddText(p, ImGui::GetColorU32(syntax::colorFor(t.kind)),
                            piece.c_str());
            }
        }

        // Caret.
        int curLine, curCol;
        edit.offsetToLineCol(edit.cursor, curLine, curCol);
        float cx = codeTL.x + (curCol - 1) * charW;
        float cy = codeTL.y + (curLine - 1) * lineHeight;
        dl->AddLine(ImVec2(cx, cy), ImVec2(cx, cy + lineHeight),
                    IM_COL32(220, 220, 220, 255), 1.0f);

        // Cache geometry for input handling.
        m_editorGutterL  = canvas.x;
        m_editorGutterR  = canvas.x + gutterW;
        m_editorCodeL    = codeTL.x;
        m_editorCodeR    = codeTL.x + codeW;
        m_editorTop      = canvas.y;
        m_editorBottom   = canvas.y + totalH;
        m_editorLineH    = lineHeight;
        m_editorCharW    = charW;
        m_editorMaxLine  = visibleLines;

        handleInput();

        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleVar();
    }

    // Geometry captured during renderTextArea so handleInput can map mouse
    // coordinates back to line / column.
    float m_editorGutterL  = 0.0f;
    float m_editorGutterR  = 0.0f;
    float m_editorCodeL    = 0.0f;
    float m_editorCodeR    = 0.0f;
    float m_editorTop      = 0.0f;
    float m_editorBottom   = 0.0f;
    float m_editorLineH    = 0.0f;
    float m_editorCharW    = 0.0f;
    int   m_editorMaxLine  = 1;

    void handleInput()
    {
        ImGuiIO& io = ImGui::GetIO();
        // Focus = the editor scroll child is the last clicked item.
        const bool focused = ImGui::IsWindowHovered();

        // ---- Keyboard ----
        if (focused)
        {
            const bool ctrl  = io.KeyCtrl;
            const bool shift = io.KeyShift;
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow,  false)) moveCursorKey(-1,  0, shift);
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) moveCursorKey(+1,  0, shift);
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow,    false)) moveCursorKey( 0, -1, shift);
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow,  false)) moveCursorKey( 0, +1, shift);
            if (ImGui::IsKeyPressed(ImGuiKey_Home,       false)) moveToLineStart(shift);
            if (ImGui::IsKeyPressed(ImGuiKey_End,        false)) moveToLineEnd(shift);
            if (ImGui::IsKeyPressed(ImGuiKey_PageUp,     false)) moveCursorKey(0, -20, shift);
            if (ImGui::IsKeyPressed(ImGuiKey_PageDown,   false)) moveCursorKey(0, +20, shift);
            if (ImGui::IsKeyPressed(ImGuiKey_Backspace,  false)) edit.deleteBackspace();
            if (ImGui::IsKeyPressed(ImGuiKey_Delete,     false)) edit.deleteForward();
            if (ImGui::IsKeyPressed(ImGuiKey_Enter,      false)) edit.newLine();
            if (ImGui::IsKeyPressed(ImGuiKey_Tab,        false)) edit.insertText("    ");
            if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z,  false))
            {
                if (shift) edit.redoStep(); else edit.undoStep();
            }
            if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y,  false)) edit.redoStep();
            if (ctrl && ImGui::IsKeyPressed(ImGuiKey_A,  false))
            {
                edit.moveCursor(0, false);
                edit.moveCursor(edit.buffer.size(), true);
            }
            if (ctrl && ImGui::IsKeyPressed(ImGuiKey_C,  false) && edit.hasSelection())
            {
                std::string sel = edit.buffer.substr(
                    edit.selectionMin(), edit.selectionMax() - edit.selectionMin());
                ImGui::SetClipboardText(sel.c_str());
            }
            if (ctrl && ImGui::IsKeyPressed(ImGuiKey_X,  false) && edit.hasSelection())
            {
                std::string sel = edit.buffer.substr(
                    edit.selectionMin(), edit.selectionMax() - edit.selectionMin());
                ImGui::SetClipboardText(sel.c_str());
                edit.deleteSelection();
            }
            if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V,  false))
            {
                const char* clip = ImGui::GetClipboardText();
                if (clip && *clip) edit.insertText(clip);
            }
            if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
            {
                if (edit.filePath.empty()) {} // host handles Save As
                else edit.save();
            }

            for (int i = 0; i < io.InputQueueCharacters.Size; ++i)
            {
                char c = static_cast<char>(io.InputQueueCharacters[i]);
                if (c >= 32 && c < 127)
                    edit.insertText(std::string(1, c));
            }
        }

        // ---- Mouse ----
        // Always do click handling (not just on focus) so the user can
        // click into the editor to position the cursor.
        const bool mouseInside =
            ImGui::IsMouseHoveringRect(ImVec2(m_editorGutterL, m_editorTop),
                                        ImVec2(m_editorCodeR,   m_editorBottom));
        if (ImGui::IsMouseClicked(0) && mouseInside)
        {
            ImVec2 m = ImGui::GetMousePos();
            if (m.x >= m_editorGutterL && m.x <= m_editorGutterR)
            {
                int line = static_cast<int>((m.y - m_editorTop) / m_editorLineH) + 1;
                if (line < 1) line = 1;
                if (line > m_editorMaxLine) line = m_editorMaxLine;
                if (debugger && !edit.filePath.empty())
                    debugger->toggleBreakpoint(edit.filePath, line);
            }
            else if (m.x >= m_editorCodeL && m.x <= m_editorCodeR)
            {
                int line = static_cast<int>((m.y - m_editorTop) / m_editorLineH) + 1;
                int col  = static_cast<int>((m.x - m_editorCodeL) / m_editorCharW) + 1;
                if (line < 1) line = 1;
                if (line > m_editorMaxLine) line = m_editorMaxLine;
                std::size_t off = edit.lineColToOffset(line, col);
                edit.moveCursor(off, io.KeyShift);
            }
        }
    }

    void moveCursorKey(int dc, int dl, bool extend)
    {
        int line, col;
        edit.offsetToLineCol(edit.cursor, line, col);
        col += dc;
        line += dl;
        if (col < 1) col = 1;
        if (line < 1) line = 1;
        if (line > edit.lineCount()) line = edit.lineCount();
        edit.moveCursor(edit.lineColToOffset(line, col), extend);
    }

    void moveToLineStart(bool extend)
    {
        int line, col;
        edit.offsetToLineCol(edit.cursor, line, col);
        edit.moveCursor(edit.lineColToOffset(line, 1), extend);
    }

    void moveToLineEnd(bool extend)
    {
        int line, col;
        edit.offsetToLineCol(edit.cursor, line, col);
        auto lines = edit.lines();
        int endCol = static_cast<int>(lines[line - 1].size()) + 1;
        edit.moveCursor(edit.lineColToOffset(line, endCol), extend);
    }
};

// --- File explorer pane -----------------------------------------------------

struct FileExplorer {
    std::string root;     // absolute path of the project root
    std::string selected; // most recently clicked file (just for display)

    void setRoot(const std::string& path) { root = path; selected.clear(); }

    // Returns the absolute path of the file the user double-clicked, or an
    // empty string if nothing was activated this frame.
    std::string render()
    {
        std::string activated;
        ImGui::BeginChild("##explorer_scroll", ImVec2(0, 0), true);
        if (root.empty())
        {
            ImGui::TextDisabled("No project open");
            ImGui::TextDisabled("File -> Open Folder...");
        }
        else
        {
            // Show the root path
            ImGui::TextDisabled("%s", root.c_str());
            ImGui::Separator();
            renderNode(root, 0, activated);
        }
        ImGui::EndChild();
        return activated;
    }

private:
    static bool isSourceFile(const fs::path& p)
    {
        if (!p.has_extension()) return false;
        auto ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return ext == ".cpp" || ext == ".c" || ext == ".h" || ext == ".hpp"
            || ext == ".cc"  || ext == ".cxx" || ext == ".hxx";
    }

    static bool isProjectFile(const fs::path& p)
    {
        if (!p.has_extension()) return false;
        auto ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return ext == ".txt" || ext == ".md" || ext == ".json"
            || ext == ".cmake" || ext == ".rc";
    }

    void renderNode(const fs::path& path, int depth, std::string& activated)
    {
        std::error_code ec;
        if (!fs::exists(path, ec) || !fs::is_directory(path, ec)) return;

        // Skip dotfiles / dotdirs to keep the tree readable.
        auto name = path.filename().string();
        if (!name.empty() && name[0] == '.' && depth > 0) return;

        // Common large directories to skip.
        static const char* kSkipDirs[] = {
            "build", "build_test_project", "vcpkg_installed", ".git",
            "Debug", "Release", "RelWithDebInfo", "MinSizeRel", "x64"
        };
        if (depth > 0)
        {
            for (auto* s : kSkipDirs)
                if (name == s) return;
        }

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
                                 | ImGuiTreeNodeFlags_OpenOnDoubleClick
                                 | ImGuiTreeNodeFlags_SpanFullWidth;
        if (depth == 0) flags |= ImGuiTreeNodeFlags_DefaultOpen;

        bool open = ImGui::TreeNodeEx(name.empty() ? path.string().c_str() : name.c_str(), flags);
        if (open)
        {
            std::vector<fs::path> entries;
            for (auto& e : fs::directory_iterator(path, ec))
                entries.push_back(e.path());
            std::sort(entries.begin(), entries.end(),
                      [](const fs::path& a, const fs::path& b) {
                          bool ad = fs::is_directory(a);
                          bool bd = fs::is_directory(b);
                          if (ad != bd) return ad; // dirs first
                          return a.filename().string() < b.filename().string();
                      });
            for (const auto& e : entries)
            {
                if (fs::is_directory(e, ec))
                {
                    renderNode(e, depth + 1, activated);
                }
                else
                {
                    // Only list source / project files to keep noise down.
                    if (!isSourceFile(e) && !isProjectFile(e)) continue;
                    auto label = e.filename().string();
                    ImGui::Bullet();
                    ImGui::SameLine();
                    if (ImGui::Selectable(label.c_str(), false,
                                          ImGuiSelectableFlags_AllowDoubleClick))
                    {
                        selected = e.string();
                        if (ImGui::IsMouseDoubleClicked(0))
                            activated = selected;
                    }
                }
            }
            ImGui::TreePop();
        }
    }
};

// --- Entry point ------------------------------------------------------------

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int)
{
    // COM is needed for the Vista+ folder picker (IFileOpenDialog).
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    // Make ourselves per-monitor DPI aware so a 4K display scales the
    // window + ImGui font correctly.
    typedef BOOL (WINAPI *SetAwarenessCtxFn)(HANDLE);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32)
    {
        auto fn = reinterpret_cast<SetAwarenessCtxFn>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (fn) fn(reinterpret_cast<HANDLE>(-4)); // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    }

    const wchar_t* kClassName = L"SFIDEWindowClass";
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_CLASSDC;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = kClassName;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);

    // Resolve the application icon. Prefer the icon embedded in app.rc;
    // fall back to the system app icon if the resource is missing so the
    // window class still registers with a valid HICON.
    HICON hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_ICON1));
    if (!hIcon)
    {
        hIcon = LoadIconA(nullptr, IDI_APPLICATION);
    }
    // Always assign the resolved icon before RegisterClassExW.
    wc.hIcon   = hIcon;
    wc.hIconSm = hIcon;
    RegisterClassExW(&wc);

    // Start maximized so the IDE uses the full screen on first launch.
    HWND hwnd = CreateWindowExW(
        0, kClassName, L"SF-IDE",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        1280, 800, nullptr, nullptr, hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        UnregisterClassW(kClassName, hInstance);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWMAXIMIZED);
    UpdateWindow(hwnd);

    // Apply the icon to the window explicitly so the title bar shows it even
    // on systems where WM_SETICON was missed during creation.
    if (hIcon)
    {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG,   reinterpret_cast<LPARAM>(hIcon));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hIcon));
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // Viewports can cause odd behaviour in single-window IDEs; keep the
    // docking experience simple and disable the multi-viewport code path.
    // io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    // Apply a slightly cleaner dark style: nicer rounding, accent colour,
    // and a less aggressive title-bar gradient.
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding    = 4.0f;
    style.FrameRounding     = 3.0f;
    style.GrabRounding      = 3.0f;
    style.TabRounding       = 3.0f;
    style.ScrollbarSize     = 12.0f;
    style.WindowBorderSize  = 1.0f;
    style.FrameBorderSize   = 0.0f;
    style.Colors[ImGuiCol_WindowBg]        = ImVec4(0.13f, 0.14f, 0.16f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive]   = ImVec4(0.10f, 0.18f, 0.28f, 1.0f);
    style.Colors[ImGuiCol_MenuBarBg]       = ImVec4(0.10f, 0.12f, 0.14f, 1.0f);
    style.Colors[ImGuiCol_Header]          = ImVec4(0.20f, 0.32f, 0.45f, 1.0f);
    style.Colors[ImGuiCol_HeaderHovered]   = ImVec4(0.25f, 0.40f, 0.55f, 1.0f);
    style.Colors[ImGuiCol_HeaderActive]    = ImVec4(0.18f, 0.30f, 0.45f, 1.0f);
    style.Colors[ImGuiCol_Tab]             = ImVec4(0.18f, 0.20f, 0.23f, 1.0f);
    style.Colors[ImGuiCol_TabHovered]      = ImVec4(0.28f, 0.36f, 0.50f, 1.0f);
    style.Colors[ImGuiCol_TabActive]       = ImVec4(0.20f, 0.32f, 0.45f, 1.0f);
    style.Colors[ImGuiCol_Button]          = ImVec4(0.22f, 0.30f, 0.40f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered]   = ImVec4(0.28f, 0.38f, 0.50f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive]    = ImVec4(0.20f, 0.30f, 0.42f, 1.0f);
    style.Colors[ImGuiCol_Separator]       = ImVec4(0.30f, 0.32f, 0.36f, 1.0f);
    style.Colors[ImGuiCol_FrameBg]         = ImVec4(0.16f, 0.18f, 0.22f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered]  = ImVec4(0.20f, 0.24f, 0.30f, 1.0f);
    style.Colors[ImGuiCol_FrameBgActive]   = ImVec4(0.22f, 0.30f, 0.40f, 1.0f);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    EditorUI editor;
    editor.debugger = nullptr;  // wired after `debugger` is constructed
    BuildRunner builder;
    FileExplorer explorer;
    Terminal terminal;

    DebugSession debugger;
    editor.debugger = &debugger;
    std::atomic<DebugSession::State> m_state{DebugSession::State::Idle};
    std::vector<DebugSession::LocalVar> locals;
    std::vector<DebugSession::StackFrame> frames;
    std::vector<std::string> debugEvents;
    std::string lastBuiltExe;     // set when the build pipeline succeeds
    std::string builderProjectDir;

    // Console pane: a simple rolling buffer of build output lines.
    struct ConsoleLine { std::string text; bool isError; };
    std::vector<ConsoleLine> consoleLines;
    auto appendConsole = [&](const std::string& line, bool isError) {
        consoleLines.push_back({line, isError});
        if (consoleLines.size() > 5000)
            consoleLines.erase(consoleLines.begin(),
                               consoleLines.begin() + 1000);
    };
    auto builderCallback = [&](const std::string& line, bool isError) {
        appendConsole(line, isError);
    };

    // Dock layout persistence: on first run (no imgui.ini yet) we build a
    // default layout. Once DockBuilderFinish() has run, ImGui serialises the
    // layout to imgui.ini and on subsequent launches it is restored from
    // there, so the user can rearrange / undock panels and have the change
    // stick across runs.
    bool dockLayoutBuilt = false;

    // Pane visibility flags toggled from the View menu.
    bool showExplorer   = true;
    bool showTerminal   = true;
    bool showOutput     = true;
    bool showDebugPane  = true;

    // When the editor is not yet associated with a file, use this default
    // title so the editor window still has a name on first launch. The
    // actual title shown in the docked Editor window is recomputed every
    // frame from the current file path.
    std::string defaultEditorTitle = "Editor - (untitled)";

    // Try to open the parent project by default so the explorer / terminal
    // are useful immediately.
    {
        char exePath[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        fs::path p = exePath;
        for (int i = 0; i < 4; ++i) p = p.parent_path();
        if (fs::exists(p / "CMakeLists.txt"))
        {
            std::string root = p.string();
            explorer.setRoot(root);
            terminal.start(root);
            // Preload a small file so the editor is populated on first
            // launch. Pick main.cpp from the project root if it exists;
            // otherwise fall back to this very file.
            fs::path toLoad = p / "src" / "main.cpp";
            if (!fs::exists(toLoad)) toLoad = p / "CMakeLists.txt";
            if (fs::exists(toLoad)) editor.edit.loadFromFile(toLoad.string());
        }
    }

    bool running = true;
    MSG msg = {};
    while (running)
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            if (msg.message == WM_QUIT) running = false;
            DispatchMessageW(&msg);
        }
        if (!running) break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Full-viewport, non-dockable host window. The actual DockSpace()
        // is submitted inside this window so we own its flags and have a
        // place to put the menu bar / status bar without them being part
        // of the docking tree. Use WorkPos/WorkSize so the window is placed
        // *below* the main menu bar (and outside the OS title bar) instead
        // of overlapping them.
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::SetNextWindowViewport(vp->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("SF-IDE DockSpace Host", nullptr,
            ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoNavFocus |
            ImGuiWindowFlags_NoBackground);
        ImGui::PopStyleVar(3);

        ImGuiID dockspaceId = ImGui::GetID("MainDockSpace");
        // ImGui::DockSpace() asserts that the DockSpace flag is NOT passed
        // (it is set internally). Only PassthruCentralNode (and similar
        // shared flags) belong here.
        const ImGuiDockNodeFlags dockSpaceFlags = ImGuiDockNodeFlags_PassthruCentralNode;
        // DockBuilderAddNode, on the other hand, REQUIRES the DockSpace
        // flag so the resulting node is treated as a dockspace root.
        const ImGuiDockNodeFlags rootFlags =
            static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_DockSpace |
                                            ImGuiDockNodeFlags_PassthruCentralNode);
        ImGui::DockSpace(dockspaceId, ImVec2(0, 0), dockSpaceFlags);

        // On the very first frame after NewFrame() there is no root dock
        // node yet (imgui.ini either does not exist or has not been
        // applied to this DockSpace id). Build the default layout in that
        // case; otherwise leave imgui.ini's saved layout alone.
        if (!dockLayoutBuilt && ImGui::DockBuilderGetNode(dockspaceId) == nullptr)
        {
            // DockBuilder uses its own id namespace, so use the same id
            // the DockSpace registered as the root.
            ImGui::DockBuilderRemoveNode(dockspaceId);
            ImGui::DockBuilderAddNode(dockspaceId, rootFlags);
            ImGui::DockBuilderSetNodeSize(dockspaceId, vp->WorkSize);

            //   +---------+----------------+---------+
            //   |Explorer |     Editor     |Debugger |
            //   +---------+----------------+---------+
            //   |       Output / Terminal          |
            //   +----------------------------------+
            // Split the root into a bottom strip first; the remainder
            // (mainId) is the central/upper region that we then split into
            // left/center/right. We only dock into the child node ids —
            // never into dockspaceId itself, which still carries the
            // DockSpace flag and would trip an ImGui assert.
            ImGuiID mainId   = dockspaceId;
            ImGuiID leftId   = 0;
            ImGuiID rightId  = 0;
            ImGuiID bottomId = ImGui::DockBuilderSplitNode(mainId, ImGuiDir_Down,  0.25f, nullptr, &mainId);
            ImGui::DockBuilderSplitNode(mainId, ImGuiDir_Left,  0.20f, &leftId,  &mainId);
            ImGui::DockBuilderSplitNode(mainId, ImGuiDir_Right, 0.25f, &rightId, &mainId);

            ImGui::DockBuilderDockWindow("Explorer", leftId);
            ImGui::DockBuilderDockWindow("Editor",   mainId);
            ImGui::DockBuilderDockWindow("Debugger", rightId);
            // Output + Terminal share the bottom node; ImGui will show them
            // as tabs in the same dock.
            ImGui::DockBuilderDockWindow("Output",   bottomId);
            ImGui::DockBuilderDockWindow("Terminal", bottomId);
            ImGui::DockBuilderFinish(dockspaceId);
            dockLayoutBuilt = true;
        }

        ImGui::End(); // SF-IDE DockSpace Host

        if (ImGui::BeginMainMenuBar())
        {
            if (ImGui::BeginMenu("File"))
            {
                if (ImGui::MenuItem("Open File...", "Ctrl+O")) editor.openDialog(hwnd);
                if (ImGui::MenuItem("Open Folder..."))
                {
                    std::string folder = pickFolderDialog(hwnd);
                    if (!folder.empty())
                    {
                        explorer.setRoot(folder);
                        // Start a terminal in the new project root.
                        if (terminal.running()) terminal.stop();
                        terminal.start(folder);
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Save", "Ctrl+S"))
                {
                    if (editor.edit.filePath.empty()) editor.saveAsDialog(hwnd);
                    else if (!editor.edit.save())
                        MessageBoxA(hwnd, "Failed to save file", "SF-IDE", MB_OK | MB_ICONERROR);
                }
                if (ImGui::MenuItem("Save As...")) editor.saveAsDialog(hwnd);
                ImGui::Separator();
                if (ImGui::MenuItem("Exit")) running = false;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Edit"))
            {
                if (ImGui::MenuItem("Undo", "Ctrl+Z", false, false)) {}
                if (ImGui::MenuItem("Redo", "Ctrl+Y", false, false)) {}
                ImGui::Separator();
                if (ImGui::MenuItem("Cut",   "Ctrl+X", false, false)) {}
                if (ImGui::MenuItem("Copy",  "Ctrl+C", false, false)) {}
                if (ImGui::MenuItem("Paste", "Ctrl+V", false, false)) {}
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View"))
            {
                ImGui::MenuItem("Explorer",        nullptr, &showExplorer);
                ImGui::MenuItem("Terminal",        nullptr, &showTerminal);
                ImGui::MenuItem("Output",          nullptr, &showOutput);
                ImGui::MenuItem("Debugger",        nullptr, &showDebugPane);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Build"))
            {
                std::string projectDir;
                if (!editor.edit.filePath.empty())
                {
                    projectDir = editor.edit.filePath;
                    auto p = projectDir.find_last_of("/\\");
                    if (p != std::string::npos) projectDir = projectDir.substr(0, p);
                }
                if (projectDir.empty() && !explorer.root.empty()) projectDir = explorer.root;
                bool canBuild = !projectDir.empty() && builder.state() == BuildRunner::State::Idle;
                if (ImGui::MenuItem("Build",  nullptr, false, canBuild))
                {
                    consoleLines.clear();
                    appendConsole("[SF-IDE] Building " + projectDir, false);
                    builder.start(projectDir, builderCallback);
                    builderProjectDir = projectDir;
                }
                if (ImGui::MenuItem("Cancel", nullptr, false,
                                    builder.state() == BuildRunner::State::Configuring ||
                                    builder.state() == BuildRunner::State::Building))
                {
                    builder.cancel();
                    appendConsole("[SF-IDE] Build cancelled", true);
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Debug"))
            {
                bool canStart = (debugger.state() == DebugSession::State::Idle ||
                                 debugger.state() == DebugSession::State::Stopped);
                bool isRunning = (debugger.state() == DebugSession::State::Running ||
                                  debugger.state() == DebugSession::State::Stepping);
                bool isBreak   = (debugger.state() == DebugSession::State::Break);
                if (ImGui::MenuItem("Start debugging", nullptr, false, canStart))
                {
                    std::string exe = lastBuiltExe;
                    if (exe.empty() && !editor.edit.filePath.empty())
                        exe = editor.edit.filePath;
                    if (!exe.empty())
                    {
                        std::string dir;
                        if (exe.find_last_of("/\\") != std::string::npos)
                            dir = exe.substr(0, exe.find_last_of("/\\"));
                        debugger.start(exe, dir,
                            [&](const std::string& s) { appendConsole(s, false); },
                            [&](DebugSession::State s) { m_state.store(s); },
                            [&](const std::vector<DebugSession::LocalVar>& v) { locals = v; },
                            [&](const std::vector<DebugSession::StackFrame>& f) { frames = f; },
                            [&]() { /* stopped */ });
                    }
                }
                if (ImGui::MenuItem("Continue", "F5", false, isBreak))
                    debugger.requestContinue();
                if (ImGui::MenuItem("Step Over", "F10", false, isBreak))
                    debugger.requestStepOver();
                if (ImGui::MenuItem("Step Into", "F11", false, isBreak))
                    debugger.requestStepInto();
                if (ImGui::MenuItem("Stop", "Shift+F5", false, isRunning || isBreak))
                    debugger.requestStop();
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Help"))
            {
                ImGui::MenuItem("About SF-IDE");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("SF-IDE - a minimal native C++ IDE for Windows.");
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }

        // Explorer pane (left)
        if (showExplorer)
        {
            ImGui::Begin("Explorer");
            std::string activated = explorer.render();
            ImGui::End();
            if (!activated.empty())
                editor.edit.loadFromFile(activated);
        }

        // Editor (center)
        {
            // Use a stable window name so ImGui keeps the dock slot. The
            // current file is shown in a small header line inside the
            // editor itself.
            ImGui::Begin("Editor");
            std::string title = defaultEditorTitle;
            if (!editor.edit.filePath.empty()) title = "Editor - " + editor.edit.filePath;
            if (editor.edit.dirty) title += " *";
            ImGui::TextUnformatted(title.c_str());
            ImGui::Separator();
            editor.renderTextAreaOnly();
            ImGui::End();
        }

        // Output pane (bottom)
        if (showOutput)
        {
            ImGui::Begin("Output");
            const char* stateText = "Idle";
            ImVec4 stateColor(0.7f, 0.7f, 0.7f, 1.0f);
            switch (builder.state())
            {
            case BuildRunner::State::Idle:        stateText = "Idle";        stateColor = ImVec4(0.7f, 0.7f, 0.7f, 1.0f); break;
            case BuildRunner::State::Configuring:stateText = "Configuring..."; stateColor = ImVec4(0.9f, 0.9f, 0.4f, 1.0f); break;
            case BuildRunner::State::Building:    stateText = "Building..."; stateColor = ImVec4(0.9f, 0.9f, 0.4f, 1.0f); break;
            case BuildRunner::State::Succeeded:
                stateText = "Build succeeded";
                stateColor = ImVec4(0.4f, 1.0f, 0.4f, 1.0f);
                if (!builderProjectDir.empty())
                {
                    WIN32_FIND_DATAA fd = {};
                    std::string search = builderProjectDir + "\\build\\Debug\\*.exe";
                    HANDLE h = FindFirstFileA(search.c_str(), &fd);
                    if (h != INVALID_HANDLE_VALUE)
                    {
                        lastBuiltExe = builderProjectDir + "\\build\\Debug\\" + fd.cFileName;
                        FindClose(h);
                    }
                }
                break;
            case BuildRunner::State::Failed:      stateText = "Build failed";    stateColor = ImVec4(1.0f, 0.4f, 0.4f, 1.0f); break;
            case BuildRunner::State::Error:       stateText = "Error";           stateColor = ImVec4(1.0f, 0.4f, 0.4f, 1.0f); break;
            }
            ImGui::TextColored(stateColor, "Status: %s", stateText);
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear")) consoleLines.clear();
            ImGui::Separator();

            ImGui::BeginChild("##console_scroll", ImVec2(0, 0), true,
                              ImGuiWindowFlags_HorizontalScrollbar);
            for (const auto& cl : consoleLines)
            {
                ImVec4 c = cl.isError ? ImVec4(1.0f, 0.55f, 0.55f, 1.0f)
                                      : ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, c);
                ImGui::TextUnformatted(cl.text.c_str());
                ImGui::PopStyleColor();
            }
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f)
                ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
            ImGui::End();
        }

        // Terminal pane (bottom)
        if (showTerminal)
        {
            ImGui::Begin("Terminal");
            terminal.render();
            ImGui::End();
        }

        // Debugger pane (bottom)
        if (showDebugPane)
        {
            ImGui::Begin("Debugger");
            const char* dbgText = "Idle";
            ImVec4 dbgColor(0.7f, 0.7f, 0.7f, 1.0f);
            switch (m_state.load())
            {
            case DebugSession::State::Idle:      dbgText = "Idle";      dbgColor = ImVec4(0.7f, 0.7f, 0.7f, 1.0f); break;
            case DebugSession::State::Running:   dbgText = "Running";   dbgColor = ImVec4(0.9f, 0.9f, 0.4f, 1.0f); break;
            case DebugSession::State::Stepping:  dbgText = "Stepping";  dbgColor = ImVec4(0.9f, 0.9f, 0.4f, 1.0f); break;
            case DebugSession::State::Break:     dbgText = "Break";     dbgColor = ImVec4(0.4f, 1.0f, 0.4f, 1.0f); break;
            case DebugSession::State::Stopped:   dbgText = "Stopped";   dbgColor = ImVec4(0.7f, 0.7f, 0.7f, 1.0f); break;
            case DebugSession::State::Error:     dbgText = "Error";     dbgColor = ImVec4(1.0f, 0.4f, 0.4f, 1.0f); break;
            }
            ImGui::TextColored(dbgColor, "Debugger: %s", dbgText);

            if (ImGui::Button("Start")) {
                if (!lastBuiltExe.empty()) {
                    std::string dir = lastBuiltExe.substr(0, lastBuiltExe.find_last_of("/\\"));
                    debugger.start(lastBuiltExe, dir,
                        [&](const std::string& s) { debugEvents.push_back(s); },
                        [&](DebugSession::State s) { m_state.store(s); },
                        [&](const std::vector<DebugSession::LocalVar>& v) { locals = v; },
                        [&](const std::vector<DebugSession::StackFrame>& f) { frames = f; },
                        [](){});
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Continue")) debugger.requestContinue();
            ImGui::SameLine();
            if (ImGui::Button("Step Over")) debugger.requestStepOver();
            ImGui::SameLine();
            if (ImGui::Button("Step Into")) debugger.requestStepInto();
            ImGui::SameLine();
            if (ImGui::Button("Stop")) debugger.requestStop();
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear events")) debugEvents.clear();

            ImGui::Separator();

            // Stack / Locals / Events are vertically stacked child regions
            // so the right-hand debugger dock can scale them with its
            // height. Each child claims an equal share of the remaining
            // space (Events is shorter to keep the recent activity visible
            // at a glance).
            ImVec2 avail = ImGui::GetContentRegionAvail();
            float h = avail.y;
            // Split the column into three: Stack 40%, Locals 40%, Events 20%.
            float stackH = h * 0.40f;
            float locH   = h * 0.40f;
            float evtH   = h - stackH - locH;

            ImGui::Text("Stack");
            ImGui::BeginChild("##dbg_stack", ImVec2(0, stackH - ImGui::GetTextLineHeight() - 4), true);
            for (size_t i = 0; i < frames.size(); ++i)
            {
                const auto& f = frames[i];
                ImGui::Text("%s  (%s:%d)",
                            f.function.c_str(),
                            f.sourceFile.empty() ? "?" : f.sourceFile.c_str(),
                            f.sourceLine);
            }
            ImGui::EndChild();

            ImGui::Text("Locals");
            ImGui::BeginChild("##dbg_locals", ImVec2(0, locH - ImGui::GetTextLineHeight() - 4), true);
            for (const auto& l : locals)
            {
                ImGui::Text("%s = %s  [%s]",
                            l.name.c_str(), l.value.c_str(),
                            l.type.empty() ? "?" : l.type.c_str());
            }
            ImGui::EndChild();

            ImGui::Text("Events");
            ImGui::BeginChild("##dbg_events", ImVec2(0, evtH - ImGui::GetTextLineHeight() - 4), true);
            for (const auto& s : debugEvents) ImGui::TextUnformatted(s.c_str());
            ImGui::EndChild();

            ImGui::End();
        }

        // Status bar at the very bottom of the main viewport.
        {
            const ImGuiViewport* sbVp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(ImVec2(sbVp->Pos.x, sbVp->Pos.y + sbVp->Size.y - 22));
            ImGui::SetNextWindowSize(ImVec2(sbVp->Size.x, 22));
            ImGui::Begin("##statusbar", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoSavedSettings);
            if (editor.edit.filePath.empty()) ImGui::Text("No file");
            else
            {
                std::string name = editor.edit.filePath;
                auto p = name.find_last_of("/\\");
                if (p != std::string::npos) name = name.substr(p + 1);
                ImGui::Text("File: %s%s", name.c_str(), editor.edit.dirty ? " *" : "");
            }
            float w = ImGui::GetContentRegionAvail().x;
            if (w > 240) ImGui::SameLine(w - 220);
            ImGui::Text("Lines: %d  Cursor: %zu",
                        editor.edit.lineCount(), editor.edit.cursor);
            ImGui::End();
        }

        ImGui::Render();
        const float clear[4] = { 0.10f, 0.10f, 0.12f, 1.00f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_pRenderTarget, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_pRenderTarget, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_pSwapChain->Present(1, 0);
    }

    terminal.stop();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(kClassName, hInstance);
    CoUninitialize();
    return 0;
}
