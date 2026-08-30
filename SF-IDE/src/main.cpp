// SF-IDE - minimal native C++ IDE for Windows.
// Phase 1: Win32 window + DirectX 11 + Dear ImGui + docking.

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

#include <string>

// --- Direct3D 11 plumbing ---------------------------------------------------

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

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

// --- Entry point ------------------------------------------------------------

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int)
{
    // Window class
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

    // ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // Main loop
    bool running = true;
    MSG msg = {};
    while (running)
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            if (msg.message == WM_QUIT)
                running = false;
            DispatchMessageW(&msg);
        }
        if (!running) break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Dock space
        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

        // Menu bar
        if (ImGui::BeginMainMenuBar())
        {
            if (ImGui::BeginMenu("File"))
            {
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

        // Welcome window so Phase 1 is visibly "alive"
        ImGui::Begin("Welcome");
        ImGui::Text("SF-IDE - Phase 1");
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                           "Win32 + DirectX 11 + Dear ImGui (docking) are running.");
        ImGui::Separator();
        ImGui::Text("Application average %.3f ms/frame (%.1f FPS)",
                    1000.0f / io.Framerate, io.Framerate);
        ImGui::End();

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

        g_pSwapChain->Present(1, 0); // vsync
    }

    // Shutdown
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(kClassName, hInstance);
    return 0;
}
