// SF-IDE - a small native C/C++ IDE for macOS (and Linux) with clil built in.
//
// GLFW + OpenGL 3.2 + Dear ImGui. The loop is event driven: it sleeps until
// there is input, process output or a timer, so an idle IDE uses no CPU.

#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#endif
#include <GLFW/glfw3.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <clocale>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "App.h"
#include "Theme.h"
#include "Util.h"
#include "Wake.h"

namespace fs = std::filesystem;

// --- wake-ups ---------------------------------------------------------------------

static std::atomic<int> g_pendingFrames{3};

void wakeUi(int frames)
{
    int cur = g_pendingFrames.load();
    while (cur < frames && !g_pendingFrames.compare_exchange_weak(cur, frames)) {}
    glfwPostEmptyEvent();
}

// Input callbacks keep frames coming for a short while: ImGui needs a few
// frames to settle (windows that appear are measured first, tooltips wait
// for the mouse to rest). ImGui's backend installs its own callbacks and
// chains to these.
static double g_activeUntil = 0;  // main thread only

static double nowSeconds()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}
static void onInput() { g_activeUntil = nowSeconds() + 0.75; }
static void onCursor(GLFWwindow*, double, double) { onInput(); }
static void onMouse(GLFWwindow*, int, int, int) { onInput(); }
static void onScroll(GLFWwindow*, double, double) { onInput(); }
static void onKey(GLFWwindow*, int, int, int, int) { onInput(); }
static void onChar(GLFWwindow*, unsigned int) { onInput(); }
static void onFocus(GLFWwindow*, int) { onInput(); }
static void onEnter(GLFWwindow*, int) { onInput(); }

// Cheap fingerprint of what was drawn, to notice frames that still change
// without input (a window measuring itself, a scroll settling).
static uint64_t drawFingerprint(const ImDrawData* dd)
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* p, size_t n) {
        const unsigned char* b = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    };
    for (const ImDrawList* l : dd->CmdLists)
    {
        mix(l->VtxBuffer.Data, size_t(l->VtxBuffer.Size) * sizeof(ImDrawVert));
        int n = l->CmdBuffer.Size;
        mix(&n, sizeof n);
    }
    return h;
}

static App* g_app = nullptr;
static std::vector<std::string> g_dropped;

static void onDrop(GLFWwindow*, int count, const char** paths)
{
    for (int i = 0; i < count; ++i) g_dropped.emplace_back(paths[i]);
    onInput();
}

static void onRefresh(GLFWwindow*) { wakeUi(1); }
static void onResize(GLFWwindow*, int w, int h)
{
    if (g_app && w > 0 && h > 0) { g_app->settings.winW = w; g_app->settings.winH = h; }
    wakeUi(2);
}
static void onClose(GLFWwindow* w)
{
    // Ask about unsaved files first; App sets quitConfirmed.
    glfwSetWindowShouldClose(w, GLFW_FALSE);
    if (g_app) g_app->requestQuit();
}

// --- fonts --------------------------------------------------------------------------

static const char* firstExisting(std::initializer_list<const char*> paths)
{
    for (const char* p : paths)
    {
        std::error_code ec;
        if (fs::exists(p, ec)) return p;
    }
    return nullptr;
}

// `dpiScale`: framebuffer pixels per window unit (2 on Retina). `uiScale`:
// extra scale for HiDPI screens where window units are pixels (Linux).
static void buildFonts(App& app, float dpiScale, float uiScale)
{
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    // Glyphs beyond Latin that the IDE, clil and lldb use: punctuation,
    // arrows, ⌘⇧⌥, box drawing, blocks, shapes, dingbats and braille spinners.
    static ImVector<ImWchar> ranges;
    if (ranges.empty())
    {
        ImFontGlyphRangesBuilder b;
        b.AddRanges(io.Fonts->GetGlyphRangesDefault());
        static const ImWchar extra[] = { 0x0100, 0x024F, 0x0370, 0x03FF, 0x2000, 0x206F, 0x2070, 0x209F, 0x20A0, 0x20CF,
                                         0x2100, 0x214F, 0x2190, 0x21FF, 0x2200, 0x22FF, 0x2300, 0x23FF, 0x2460, 0x24FF,
                                         0x2500, 0x259F, 0x25A0, 0x25FF, 0x2600, 0x26FF, 0x2700, 0x27BF, 0x2800, 0x28FF,
                                         0 };
        b.AddRanges(extra);
        b.BuildRanges(&ranges);
    }

#if defined(__APPLE__)
    const char* uiPath = firstExisting({ "/System/Library/Fonts/SFNS.ttf", "/System/Library/Fonts/SFNSText.ttf",
                                         "/System/Library/Fonts/Helvetica.ttc" });
    const char* monoPath = firstExisting({ "/System/Library/Fonts/SFNSMono.ttf",
                                           "/System/Applications/Utilities/Terminal.app/Contents/Resources/Fonts/SFMono-Regular.otf",
                                           "/Library/Fonts/SF-Mono-Regular.otf", "/System/Library/Fonts/Menlo.ttc",
                                           "/System/Library/Fonts/Monaco.ttf" });
    const char* fallbacks[] = { "/System/Library/Fonts/Menlo.ttc", "/System/Library/Fonts/Apple Symbols.ttf",
                                "/System/Library/Fonts/Supplemental/Arial Unicode.ttf" };
#else
    const char* uiPath = firstExisting({ "/usr/share/fonts/truetype/inter/Inter-Regular.ttf",
                                         "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf",
                                         "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
                                         "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                         "/usr/share/fonts/TTF/DejaVuSans.ttf" });
    const char* monoPath = firstExisting({ "/usr/share/fonts/truetype/jetbrains-mono/JetBrainsMono-Regular.ttf",
                                           "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
                                           "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
                                           "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
                                           "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf" });
    const char* fallbacks[] = { "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
                                "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                "/usr/share/fonts/truetype/noto/NotoSansSymbols2-Regular.ttf" };
#endif

    auto addFont = [&](const char* path, float size) -> ImFont* {
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        cfg.OversampleV = 1;
        cfg.PixelSnapH = false;
        ImFont* f = nullptr;
        if (path) f = io.Fonts->AddFontFromFileTTF(path, size * dpiScale, &cfg, ranges.Data);
        if (!f)
        {
            ImFontConfig def;
            def.SizePixels = size * dpiScale;
            f = io.Fonts->AddFontDefault(&def);
            return f;
        }
        // Merge fallback fonts for symbols the main font lacks (first font wins).
        for (const char* fb : fallbacks)
        {
            if (!fb || (path && std::string(fb) == path)) continue;
            std::error_code ec;
            if (!fs::exists(fb, ec)) continue;
            ImFontConfig m;
            m.MergeMode = true;
            m.OversampleH = 2;
            m.PixelSnapH = false;
            io.Fonts->AddFontFromFileTTF(fb, size * dpiScale, &m, ranges.Data);
        }
        return f;
    };

    app.uiFont = addFont(uiPath, app.settings.uiFontSize * uiScale);
    app.monoFont = addFont(monoPath, app.settings.editorFontSize * uiScale);
    io.FontDefault = app.uiFont;
    // Fonts are rasterised at the Retina pixel size and drawn at 1/scale,
    // which keeps text sharp on HiDPI screens.
    io.FontGlobalScale = 1.0f / dpiScale;
    io.Fonts->Build();
}

// --- main ----------------------------------------------------------------------------

int main(int argc, char** argv)
{
    std::setlocale(LC_CTYPE, "");
    signal(SIGPIPE, SIG_IGN);

    // Apps started from Finder get a minimal PATH; add the usual tool dirs so
    // cmake, clil, lms and Homebrew compilers are found.
    {
        std::string path = std::getenv("PATH") ? std::getenv("PATH") : "/usr/bin:/bin";
        for (const char* d : { "/usr/local/bin", "/opt/homebrew/bin" })
            if (path.find(d) == std::string::npos) path = std::string(d) + ":" + path;
        std::string local = homeDir() + "/.local/bin";
        if (path.find(local) == std::string::npos) path += ":" + local;
        setenv("PATH", path.c_str(), 1);
    }

    if (!glfwInit()) return 1;

#if defined(__APPLE__)
    const char* glsl = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);
    glfwWindowHint(GLFW_COCOA_GRAPHICS_SWITCHING, GLFW_TRUE);  // stay on the integrated GPU
#else
    const char* glsl = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);

    App* appPtr = new App();
    App& app = *appPtr;
    g_app = appPtr;

    GLFWwindow* window = glfwCreateWindow(std::max(800, app.settings.winW), std::max(500, app.settings.winH),
                                          "SF-IDE", nullptr, nullptr);
    if (!window)
    {
        delete appPtr;
        glfwTerminate();
        return 1;
    }
    glfwSetWindowSizeLimits(window, 800, 500, GLFW_DONT_CARE, GLFW_DONT_CARE);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    glfwSetCursorPosCallback(window, onCursor);
    glfwSetMouseButtonCallback(window, onMouse);
    glfwSetScrollCallback(window, onScroll);
    glfwSetKeyCallback(window, onKey);
    glfwSetCharCallback(window, onChar);
    glfwSetWindowFocusCallback(window, onFocus);
    glfwSetCursorEnterCallback(window, onEnter);
    glfwSetDropCallback(window, onDrop);
    glfwSetWindowRefreshCallback(window, onRefresh);
    glfwSetWindowSizeCallback(window, onResize);
    glfwSetWindowCloseCallback(window, onClose);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   // fixed layout: nothing to remember in imgui.ini
    io.LogFilename = nullptr;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.ConfigInputTextCursorBlink = false;
    io.ConfigDragClickToInputText = true;
#if defined(__APPLE__)
    io.ConfigMacOSXBehaviors = true;
#endif

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl);

    float xs = 1, ys = 1;
    glfwGetWindowContentScale(window, &xs, &ys);
#if defined(__APPLE__)
    // On macOS window coordinates are points; the framebuffer has the pixels.
    int fbw = 0, ww = 0, h = 0;
    glfwGetFramebufferSize(window, &fbw, &h);
    glfwGetWindowSize(window, &ww, &h);
    float dpi = ww > 0 ? float(fbw) / float(ww) : xs;
#else
    float dpi = 1.0f;  // GLFW_SCALE_TO_MONITOR already sized the window in pixels
#endif
#if defined(__APPLE__)
    const float uiScale = 1.0f;
#else
    const float uiScale = xs;
#endif
    setTheme(app.settings.dark);
#if !defined(__APPLE__)
    ImGui::GetStyle().ScaleAllSizes(xs);
#endif
    buildFonts(app, dpi, uiScale);
    float fontDpi = dpi;

    // Open what was passed on the command line, or the last project.
    bool opened = false;
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a.rfind("-psn_", 0) == 0) continue;  // Finder process serial number
        std::error_code ec;
        if (fs::exists(a, ec))
        {
            app.openPath(fs::absolute(a).string());
            opened = true;
        }
    }
    if (!opened && !app.settings.lastProject.empty() && fs::is_directory(app.settings.lastProject))
        app.openFolder(app.settings.lastProject);

    double lastTick = nowSeconds();
    uint64_t lastPrint = 0;
    int settleFrames = 0;
    while (!app.quitConfirmed)
    {
        // Sleep until something happens. Process output and builds wake us
        // through wakeUi(); right after input we draw at up to 60 fps for a
        // moment; otherwise the timeout only drives the 1 s housekeeping tick.
        double t = nowSeconds();
        if (g_pendingFrames.load() > 0) glfwPollEvents();
        else if (t < g_activeUntil) glfwWaitEventsTimeout(1.0 / 60.0);
        else glfwWaitEventsTimeout(app.busy() ? 0.25 : 1.0);

        t = nowSeconds();
        if (t - lastTick >= 1.0)
        {
            lastTick = t;
            app.tick();
            if (app.busy()) wakeUi(1);  // keep the build timer moving
        }
        app.pump();

        if (!g_dropped.empty())
        {
            auto dropped = std::move(g_dropped);
            g_dropped.clear();
            for (auto& p : dropped) app.openPath(p);
        }

        // Monitor change (e.g. Retina laptop <-> external display).
        int fw = 0, fh = 0, wwid = 0, whei = 0;
        glfwGetFramebufferSize(window, &fw, &fh);
        glfwGetWindowSize(window, &wwid, &whei);
#if defined(__APPLE__)
        float curDpi = wwid > 0 ? float(fw) / float(wwid) : fontDpi;
#else
        float curDpi = fontDpi;
#endif
        if (std::abs(curDpi - fontDpi) > 0.01f || app.fontsDirty)
        {
            app.settings.uiFontSize = std::clamp(app.settings.uiFontSize, 10.0f, 24.0f);
            app.settings.editorFontSize = std::clamp(app.settings.editorFontSize, 9.0f, 32.0f);
            fontDpi = curDpi;
            app.fontsDirty = false;
            buildFonts(app, fontDpi, uiScale);
            ImGui_ImplOpenGL3_DestroyFontsTexture();
            ImGui_ImplOpenGL3_CreateFontsTexture();
            wakeUi(3);
        }

        bool draw = g_pendingFrames.load() > 0 || t < g_activeUntil || settleFrames > 0;
        if (!draw) continue;
        if (g_pendingFrames.load() > 0) g_pendingFrames.fetch_sub(1);
        if (settleFrames > 0) --settleFrames;

        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED))
        {
            g_pendingFrames = 0;
            settleFrames = 0;
            continue;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        app.frame();
        ImGui::Render();

        // If this frame looks different from the last one, draw a few more
        // so ImGui can settle even without new input.
        uint64_t print = drawFingerprint(ImGui::GetDrawData());
        if (print != lastPrint) settleFrames = std::max(settleFrames, 2);
        lastPrint = print;

        glViewport(0, 0, fw, fh);
        const ImVec4 bg = ImGui::ColorConvertU32ToFloat4(theme().panel);
        glClearColor(bg.x, bg.y, bg.z, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);

        // Keep drawing while the mouse is held (drags, scrollbars, selection).
        if (ImGui::IsAnyMouseDown()) onInput();

        if (app.quitRequested) break;
    }

    int ww2 = 0, wh2 = 0;
    glfwGetWindowSize(window, &ww2, &wh2);
    app.settings.winW = ww2;
    app.settings.winH = wh2;
    app.saveSettings();

    g_app = nullptr;
    delete appPtr;  // stops shell / clil / builds before the GL context goes away

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
