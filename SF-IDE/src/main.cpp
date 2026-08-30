// SF-IDE - minimal native C++ IDE for Windows.
// Phase 2: text editor with line numbers, syntax highlighting, open/save.

#include <Windows.h>
#include <commdlg.h>
#include <d3d11.h>
#include <dxgi.h>

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "Editor.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "comdlg32.lib")

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
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED)
        {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
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
        int len = WideCharToMultiByte(CP_UTF8, 0, filename, -1, nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<std::size_t>(len > 0 ? len - 1 : 0), '\0');
        if (len > 0)
            WideCharToMultiByte(CP_UTF8, 0, filename, -1, &out[0], len, nullptr, nullptr);
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
        int len = WideCharToMultiByte(CP_UTF8, 0, filename, -1, nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<std::size_t>(len > 0 ? len - 1 : 0), '\0');
        if (len > 0)
            WideCharToMultiByte(CP_UTF8, 0, filename, -1, &out[0], len, nullptr, nullptr);
        return out;
    }
    return {};
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
            // Preprocessor: only colorise the leading '#' plus the include/define
            // directive keyword. Keeps the rest as default.
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
    bool showSaveConfirm = false;
    std::string pendingClosePath;

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

    void renderEditorWindow()
    {
        std::string title = "Editor - ";
        if (edit.filePath.empty()) title += "(untitled)";
        else                        title += edit.filePath;
        if (edit.dirty)             title += " *";

        ImGui::Begin(title.c_str());

        // Custom text input (no InputTextMultiline so we can render line
        // numbers and syntax highlighting alongside the code).
        renderTextArea();

        ImGui::End();
    }

private:
    // The actual code-area rendering. Splits into a line-number column and
    // a code column; both share the same vertical scroll.
    void renderTextArea()
    {
        const float lineHeight = ImGui::GetTextLineHeight();
        const float charW      = ImGui::CalcTextSize("0").x;
        const int   lc         = edit.lineCount();
        const std::string lcStr = std::to_string(lc);
        int digits = (lcStr.size() > 3) ? static_cast<int>(lcStr.size()) : 3;
        const float gutterW    = charW * (digits + 2);

        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));

        // Outer child acts as the scrollable container for both columns.
        ImGui::BeginChild("##editor_scroll", avail, true,
                          ImGuiWindowFlags_HorizontalScrollbar);

        // Total height: lineCount lines plus one for trailing newline.
        const float totalH = lineHeight * (lc + 1);
        ImGui::Dummy(ImVec2(1.0f, totalH));

        // Use a child window inside the scroller for absolute-positioned
        // rendering of each line so line numbers and code share the same
        // scroll origin.
        ImGui::SetCursorPos(ImVec2(0, 0));
        ImGui::BeginGroup();

        // Gutter
        ImGui::BeginChild("##gutter", ImVec2(gutterW, totalH), false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMouseInputs);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 origin = ImGui::GetCursorScreenPos();
        for (int i = 0; i <= lc; ++i)
        {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%*d", digits, i + 1);
            ImVec2 p(origin.x, origin.y + i * lineHeight);
            dl->AddText(p, IM_COL32(120, 130, 140, 255), buf);
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // Code area
        ImGui::BeginChild("##code", ImVec2(0, totalH), false,
                          ImGuiWindowFlags_HorizontalScrollbar);

        // Tokenise and render each line with colours.
        auto lines = edit.lines();
        std::vector<std::vector<syntax::Token>> perLine(lc);
        bool inBlock = false;
        for (int i = 0; i < lc; ++i)
            syntax::tokeniseLine(lines[i], perLine[i], inBlock);

        // Selection background
        if (edit.hasSelection())
        {
            int aLine, aCol, bLine, bCol;
            edit.offsetToLineCol(edit.selectionMin(), aLine, aCol);
            edit.offsetToLineCol(edit.selectionMax(), bLine, bCol);
            ImVec2 codeOrigin = ImGui::GetCursorScreenPos();
            for (int ln = aLine; ln <= bLine; ++ln)
            {
                int c0 = (ln == aLine) ? aCol : 1;
                int c1 = (ln == bLine) ? bCol : static_cast<int>(lines[ln - 1].size()) + 1;
                float x0 = codeOrigin.x + (c0 - 1) * charW;
                float x1 = codeOrigin.x + (c1 - 1) * charW;
                float y0 = codeOrigin.y + (ln - 1) * lineHeight;
                float y1 = y0 + lineHeight;
                dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1),
                                  IM_COL32(60, 100, 160, 110));
            }
        }

        // Draw tokens
        for (int i = 0; i < lc; ++i)
        {
            ImVec2 base = ImGui::GetCursorScreenPos();
            base.y += i * lineHeight;
            for (const auto& t : perLine[i])
            {
                std::string_view piece = lines[i].substr(t.begin, t.end - t.begin);
                ImVec2 p(base.x + t.begin * charW, base.y);
                dl->AddText(p, ImGui::GetColorU32(syntax::colorFor(t.kind)),
                            std::string(piece).c_str());
            }
        }

        // Caret
        int curLine, curCol;
        edit.offsetToLineCol(edit.cursor, curLine, curCol);
        ImVec2 codeOrigin = ImGui::GetCursorScreenPos();
        float cx = codeOrigin.x + (curCol - 1) * charW;
        float cy = codeOrigin.y + (curLine - 1) * lineHeight;
        dl->AddLine(ImVec2(cx, cy), ImVec2(cx, cy + lineHeight),
                    IM_COL32(220, 220, 220, 255), 1.0f);

        // Mouse / keyboard interaction (kept simple: click to position, type
        // to insert, Backspace/Delete handled here, arrows move cursor).
        handleInput();

        ImGui::EndChild();
        ImGui::EndGroup();
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }

    void handleInput()
    {
        ImGuiIO& io = ImGui::GetIO();
        const bool focused = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);

        if (focused)
        {
            // Arrow keys
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow,  false)) moveCursorKey(-1, 0, io.KeyShift);
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false)) moveCursorKey(+1, 0, io.KeyShift);
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow,    false)) moveCursorKey(0, -1, io.KeyShift);
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow,  false)) moveCursorKey(0, +1, io.KeyShift);
            if (ImGui::IsKeyPressed(ImGuiKey_Home,       false)) moveToLineStart(io.KeyShift);
            if (ImGui::IsKeyPressed(ImGuiKey_End,        false)) moveToLineEnd(io.KeyShift);
            if (ImGui::IsKeyPressed(ImGuiKey_Backspace,  false)) edit.deleteBackspace();
            if (ImGui::IsKeyPressed(ImGuiKey_Delete,     false)) edit.deleteForward();
            if (ImGui::IsKeyPressed(ImGuiKey_Enter,      false)) edit.newLine();
            if (ImGui::IsKeyPressed(ImGuiKey_Z, false) && io.KeyCtrl)
            {
                if (io.KeyShift) edit.redoStep();
                else             edit.undoStep();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Y, false) && io.KeyCtrl) edit.redoStep();

            // Plain character input
            for (int i = 0; i < io.InputQueueCharacters.Size; ++i)
            {
                char c = static_cast<char>(io.InputQueueCharacters[i]);
                if (c >= 32 && c < 127)
                    edit.insertText(std::string(1, c));
            }
        }

        // Mouse click to position the cursor
        if (ImGui::IsMouseClicked(0) && ImGui::IsWindowHovered())
        {
            ImVec2 m = ImGui::GetMousePos();
            ImVec2 o = ImGui::GetItemRectMin();
            const float lineHeight = ImGui::GetTextLineHeight();
            const float charW      = ImGui::CalcTextSize("0").x;
            int line = static_cast<int>((m.y - o.y) / lineHeight) + 1;
            int col  = static_cast<int>((m.x - o.x) / charW) + 1;
            std::size_t off = edit.lineColToOffset(line, col);
            edit.moveCursor(off, io.KeyShift);
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

// --- Entry point ------------------------------------------------------------

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int)
{
    const wchar_t* kClassName = L"SFIDEWindowClass";
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_CLASSDC;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = kClassName;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

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

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    EditorUI editor;
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

        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

        if (ImGui::BeginMainMenuBar())
        {
            if (ImGui::BeginMenu("File"))
            {
                if (ImGui::MenuItem("Open...", "Ctrl+O")) editor.openDialog(hwnd);
                if (ImGui::MenuItem("Save",   "Ctrl+S"))
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
            if (ImGui::BeginMenu("Build"))
            {
                ImGui::MenuItem("(no project)");
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Debug"))
            {
                ImGui::MenuItem("(no session)");
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }

        // Status line
        if (ImGui::Begin("Status"))
        {
            if (editor.edit.filePath.empty()) ImGui::Text("No file");
            else
            {
                std::string name = editor.edit.filePath;
                auto p = name.find_last_of("/\\");
                if (p != std::string::npos) name = name.substr(p + 1);
                ImGui::Text("File: %s%s", name.c_str(), editor.edit.dirty ? " *" : "");
            }
            ImGui::SameLine(ImGui::GetWindowWidth() - 220);
            ImGui::Text("Lines: %d  Cursor: %zu",
                        editor.edit.lineCount(), editor.edit.cursor);
            ImGui::End();
        }

        editor.renderEditorWindow();

        ImGui::Render();
        const float clear[4] = { 0.10f, 0.10f, 0.12f, 1.00f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_pRenderTarget, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_pRenderTarget, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }

        g_pSwapChain->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(kClassName, hInstance);
    return 0;
}
