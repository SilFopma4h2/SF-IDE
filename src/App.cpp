#include "App.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <unistd.h>

#include <imgui.h>
#include <imgui_internal.h>

#include "Theme.h"
#include "IdeUtil.h"
#include "Wake.h"

namespace fs = std::filesystem;

namespace {

double now()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool isMac()
{
#if defined(__APPLE__)
    return true;
#else
    return false;
#endif
}

// Shortcut label for menus: "⌘⇧S" on macOS, "Ctrl+Shift+S" elsewhere.
std::string sc(const char* key, bool shift = false, bool alt = false)
{
    std::string s;
    if (isMac())
    {
        if (alt) s += "\xE2\x8C\xA5";    // ⌥
        if (shift) s += "\xE2\x87\xA7";  // ⇧
        s += "\xE2\x8C\x98";             // ⌘
        s += key;
    }
    else
    {
        s = "Ctrl+";
        if (shift) s += "Shift+";
        if (alt) s += "Alt+";
        s += key;
    }
    return s;
}

bool chord(ImGuiKey key, bool shift = false, bool alt = false, bool repeat = false)
{
    ImGuiIO& io = ImGui::GetIO();
    return io.KeyCtrl && io.KeyShift == shift && io.KeyAlt == alt && ImGui::IsKeyPressed(key, repeat);
}

std::string trim(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::vector<std::string> splitArgs(const std::string& s)
{
    // Whitespace separated, with '...' and "..." quoting.
    std::vector<std::string> out;
    std::string cur;
    bool any = false;
    char q = 0;
    for (char c : s)
    {
        if (q)
        {
            if (c == q) q = 0;
            else cur.push_back(c);
        }
        else if (c == '\'' || c == '"') { q = c; any = true; }
        else if (c == ' ' || c == '\t')
        {
            if (any || !cur.empty()) out.push_back(cur);
            cur.clear();
            any = false;
        }
        else cur.push_back(c);
    }
    if (any || !cur.empty()) out.push_back(cur);
    return out;
}

[[maybe_unused]] std::string appleScriptString(const std::string& s)
{
    std::string out = "\"";
    for (char c : s)
    {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out + "\"";
}

// Native file dialogs without Objective-C: AppleScript on macOS, zenity on
// Linux. `ok` is false when no dialog program is available (the caller
// then asks for a path inside the IDE).
enum class Dialog { Folder, File, Save };
std::string nativeDialog(Dialog kind, const std::string& startDir, const std::string& defaultName, bool& ok)
{
    ok = true;
    std::string cmd;
#if defined(__APPLE__)
    std::string loc = startDir.empty() ? "" : " default location (POSIX file " + appleScriptString(startDir) + ")";
    std::string script;
    if (kind == Dialog::Folder) script = "POSIX path of (choose folder with prompt \"Open Folder\"" + loc + ")";
    else if (kind == Dialog::File) script = "POSIX path of (choose file with prompt \"Open File\"" + loc + ")";
    else script = "POSIX path of (choose file name with prompt \"Save As\" default name " +
                  appleScriptString(defaultName.empty() ? "untitled.cpp" : defaultName) + loc + ")";
    cmd = "osascript -e " + shellQuote(script) + " 2>/dev/null";
#else
    if (findProgram("zenity").empty())
    {
        ok = false;
        return {};
    }
    std::string start = startDir.empty() ? homeDir() : startDir;
    if (kind == Dialog::Folder) cmd = "zenity --file-selection --directory --filename=" + shellQuote(start + "/");
    else if (kind == Dialog::File) cmd = "zenity --file-selection --filename=" + shellQuote(start + "/");
    else cmd = "zenity --file-selection --save --confirm-overwrite --filename=" +
               shellQuote(start + "/" + (defaultName.empty() ? "untitled.cpp" : defaultName));
    cmd += " 2>/dev/null";
#endif
    std::string out = trim(runCapture(cmd));
    if (kind == Dialog::Folder && out.size() > 1 && out.back() == '/') out.pop_back();
    return out;
}

int fuzzyScore(const std::string& hay, const std::string& needle)
{
    // Subsequence match; consecutive characters and basename hits score higher.
    if (needle.empty()) return 1;
    size_t base = hay.find_last_of('/');
    base = base == std::string::npos ? 0 : base + 1;
    int score = 0, run = 0;
    size_t j = 0;
    for (size_t i = 0; i < hay.size() && j < needle.size(); ++i)
    {
        char a = char(std::tolower((unsigned char)hay[i]));
        char b = char(std::tolower((unsigned char)needle[j]));
        if (a == b)
        {
            ++j;
            ++run;
            score += 1 + run * 2 + (i >= base ? 3 : 0);
        }
        else
        {
            run = 0;
        }
    }
    if (j < needle.size()) return 0;
    return score * 100 / int(hay.size() + 10);
}

ImVec4 col4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

// A flat, borderless button used in headers and the toolbar.
bool flatButton(const char* label, bool enabled = true, const char* tooltip = nullptr)
{
    ImGui::BeginDisabled(!enabled);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(7, 3));
    bool r = ImGui::Button(label);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tooltip);
    return r;
}

enum class Icon { Play, Stop, Build, Bug };

// Toolbar button with a drawn icon (no icon font needed).
bool iconButton(const char* id, Icon icon, const char* label, bool enabled, const char* tooltip, ImU32 tint)
{
    const Theme& t = theme();
    const float h = ImGui::GetFrameHeight();
    const float iconW = h * 0.9f;
    ImVec2 ts = ImGui::CalcTextSize(label);
    ImVec2 size(iconW + ts.x + 12, h);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    bool pressed = ImGui::InvisibleButton(id, size);
    bool hovered = ImGui::IsItemHovered();
    bool held = ImGui::IsItemActive();
    ImGui::EndDisabled();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (enabled && (hovered || held)) dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), held ? t.border : t.hover, 5.0f);
    ImU32 c = enabled ? tint : t.textDim;
    ImVec2 ctr(p.x + 6 + iconW * 0.5f, p.y + h * 0.5f);
    float r = h * 0.22f;
    switch (icon)
    {
    case Icon::Play:
        dl->AddTriangleFilled(ImVec2(ctr.x - r * 0.8f, ctr.y - r), ImVec2(ctr.x - r * 0.8f, ctr.y + r),
                              ImVec2(ctr.x + r, ctr.y), c);
        break;
    case Icon::Stop:
        dl->AddRectFilled(ImVec2(ctr.x - r * 0.85f, ctr.y - r * 0.85f), ImVec2(ctr.x + r * 0.85f, ctr.y + r * 0.85f), c, 2.0f);
        break;
    case Icon::Build:  // hammer: head + handle
        dl->AddRectFilled(ImVec2(ctr.x - r, ctr.y - r), ImVec2(ctr.x + r * 0.6f, ctr.y - r * 0.25f), c, 1.5f);
        dl->AddRectFilled(ImVec2(ctr.x - r * 0.35f, ctr.y - r * 0.3f), ImVec2(ctr.x + r * 0.05f, ctr.y + r * 1.1f), c, 1.0f);
        break;
    case Icon::Bug:
        dl->AddCircleFilled(ImVec2(ctr.x, ctr.y + r * 0.15f), r * 0.75f, c);
        dl->AddCircleFilled(ImVec2(ctr.x, ctr.y - r * 0.75f), r * 0.38f, c);
        for (int s = -1; s <= 1; s += 2)
            for (int k = 0; k < 3; ++k)
                dl->AddLine(ImVec2(ctr.x + s * r * 0.6f, ctr.y - r * 0.3f + k * r * 0.45f),
                            ImVec2(ctr.x + s * r * 1.15f, ctr.y - r * 0.5f + k * r * 0.55f), c, 1.2f);
        break;
    }
    dl->AddText(ImVec2(p.x + 6 + iconW + 2, p.y + (h - ts.y) * 0.5f), enabled ? t.text : t.textDim, label);
    if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", tooltip);
    return pressed;
}

// Small rounded label (status pills).
void pill(const char* text, ImU32 bg, ImU32 fg)
{
    ImVec2 ts = ImGui::CalcTextSize(text);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetFrameHeight();
    ImVec2 size(ts.x + 14, h - 6);
    p.y += 3;
    ImGui::Dummy(ImVec2(size.x, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), bg, size.y * 0.5f);
    dl->AddText(ImVec2(p.x + 7, p.y + (size.y - ts.y) * 0.5f), fg, text);
}

ImU32 withAlpha(ImU32 c, int a)
{
    return (c & 0x00FFFFFF) | (ImU32(a) << IM_COL32_A_SHIFT);
}

} // namespace

// --- Settings -----------------------------------------------------------------------

void Settings::load()
{
    std::string text;
    if (!readTextFile(configDir() + "/settings.ini", text)) return;
    std::istringstream in(text);
    std::string line;
    recent.clear();
    while (std::getline(in, line))
    {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        auto f = [&](float& x) { x = std::strtof(v.c_str(), nullptr); };
        auto b = [&](bool& x) { x = v == "1"; };
        if (k == "ui_font_size") f(uiFontSize);
        else if (k == "editor_font_size") f(editorFontSize);
        else if (k == "dark") b(dark);
        else if (k == "clil_path") clilPath = v;
        else if (k == "clil_args") clilArgs = v;
        else if (k == "lm_host") lmHost = v;
        else if (k == "win_w") winW = std::atoi(v.c_str());
        else if (k == "win_h") winH = std::atoi(v.c_str());
        else if (k == "left_w") f(leftW);
        else if (k == "right_w") f(rightW);
        else if (k == "bottom_h") f(bottomH);
        else if (k == "show_left") b(showLeft);
        else if (k == "show_right") b(showRight);
        else if (k == "show_bottom") b(showBottom);
        else if (k == "save_before_build") b(saveBeforeBuild);
        else if (k == "recent") recent.push_back(v);
        else if (k == "last_project") lastProject = v;
    }
    uiFontSize = std::clamp(uiFontSize, 10.0f, 24.0f);
    editorFontSize = std::clamp(editorFontSize, 9.0f, 32.0f);
}

void Settings::save() const
{
    std::ostringstream o;
    o << "ui_font_size=" << uiFontSize << "\n"
      << "editor_font_size=" << editorFontSize << "\n"
      << "dark=" << (dark ? 1 : 0) << "\n"
      << "clil_path=" << clilPath << "\n"
      << "clil_args=" << clilArgs << "\n"
      << "lm_host=" << lmHost << "\n"
      << "win_w=" << winW << "\n"
      << "win_h=" << winH << "\n"
      << "left_w=" << leftW << "\n"
      << "right_w=" << rightW << "\n"
      << "bottom_h=" << bottomH << "\n"
      << "show_left=" << (showLeft ? 1 : 0) << "\n"
      << "show_right=" << (showRight ? 1 : 0) << "\n"
      << "show_bottom=" << (showBottom ? 1 : 0) << "\n"
      << "save_before_build=" << (saveBeforeBuild ? 1 : 0) << "\n"
      << "last_project=" << lastProject << "\n";
    for (const auto& r : recent) o << "recent=" << r << "\n";
    // Called every second from App::tick(); only touch the disk on changes.
    static std::string lastWritten;
    if (o.str() == lastWritten) return;
    if (writeTextFile(configDir() + "/settings.ini", o.str())) lastWritten = o.str();
}

// --- App lifecycle ------------------------------------------------------------------

App::App()
{
    settings.load();
    m_editor.contextMenu = [this](Document& doc) {
        if (ImGui::MenuItem("Cut", sc("X").c_str(), false, true)) m_editor.cut(doc);
        if (ImGui::MenuItem("Copy", sc("C").c_str())) m_editor.copy(doc);
        if (ImGui::MenuItem("Paste", sc("V").c_str())) m_editor.paste(doc);
        ImGui::Separator();
        if (ImGui::MenuItem("Toggle Comment", sc("/").c_str())) m_editor.toggleComment(doc);
        if (ImGui::MenuItem("Toggle Breakpoint", "F9")) doc.toggleBreakpoint(doc.lineOf(doc.cursor) + 1);
        ImGui::Separator();
        if (ImGui::MenuItem("Explain with clil")) askClilAboutSelection("Explain");
        if (ImGui::MenuItem("Improve with clil")) askClilAboutSelection("Improve");
        if (ImGui::MenuItem("Find bugs with clil")) askClilAboutSelection("Find bugs in");
    };
}

App::~App()
{
    m_shell.stop();
    m_clil.stop();
    m_run.stop();
    m_debug.stop();
    m_build.cancel();
    saveSettings();
}

void App::saveSettings()
{
    settings.lastProject = m_root;
    settings.save();
}

std::string App::rel(const std::string& path) const
{
    return relativePath(path, m_root);
}

void App::showMessage(const std::string& title, const std::string& text)
{
    m_messageTitle = title;
    m_messageText = text;
    m_openMessage = true;
}

void App::addRecent(const std::string& path)
{
    auto& r = settings.recent;
    r.erase(std::remove(r.begin(), r.end(), path), r.end());
    r.insert(r.begin(), path);
    if (r.size() > 10) r.resize(10);
}

void App::openFolder(const std::string& path)
{
    std::error_code ec;
    fs::path p = fs::canonical(path, ec);
    if (ec || !fs::is_directory(p))
    {
        showMessage("Open Folder", "Not a folder: " + path);
        return;
    }
    m_root = p.string();
    m_tree.setRoot(m_root);
    m_cmds = detectProjectCommands(m_root);
    loadProjectConfig();
    m_lastExe.clear();
    m_problems.clear();
    addRecent(m_root);
    saveSettings();
    refreshGitBranch();

    // Restart the shell and clil in the new folder.
    m_shell.stop();
    if (m_clil.started())
    {
        m_clil.stop();
        m_clilStarted = false;
        if (settings.showRight) startClil();
    }
    if (settings.showBottom && m_bottomTab == TabTerminal) startShell();

    // Open a sensible first file when nothing is open yet.
    if (m_docs.empty())
    {
        for (const char* f : { "src/main.cpp", "main.cpp", "src/main.c", "main.c", "README.md", "CMakeLists.txt" })
        {
            if (fs::exists(fs::path(m_root) / f))
            {
                openFile((fs::path(m_root) / f).string());
                break;
            }
        }
    }
}

void App::openFile(const std::string& path, int line, int col)
{
    std::error_code ec;
    fs::path p = fs::weakly_canonical(path, ec);
    std::string full = ec ? path : p.string();
    for (size_t i = 0; i < m_docs.size(); ++i)
    {
        if (m_docs[i]->path() == full)
        {
            m_active = int(i);
            m_selectTab = int(i);
            if (line > 0) m_editor.gotoLine(*m_docs[i], line, std::max(1, col));
            else m_editor.focus();
            return;
        }
    }
    auto doc = std::make_unique<Document>();
    if (!doc->load(full))
    {
        showMessage("Open File", "Cannot open " + full);
        return;
    }
    m_docs.push_back(std::move(doc));
    m_active = int(m_docs.size()) - 1;
    m_selectTab = m_active;
    applyDiagnostics();
    if (line > 0) m_editor.gotoLine(*m_docs.back(), line, std::max(1, col));
    else m_editor.focus();
    m_tree.reveal(full);
}

void App::openPath(const std::string& path)
{
    std::error_code ec;
    if (fs::is_directory(path, ec)) openFolder(path);
    else openFile(path);
}

Document* App::activeDoc()
{
    if (m_active < 0 || m_active >= int(m_docs.size())) return nullptr;
    return m_docs[size_t(m_active)].get();
}

void App::newFile()
{
    m_docs.push_back(std::make_unique<Document>());
    m_active = int(m_docs.size()) - 1;
    m_selectTab = m_active;
    m_editor.focus();
}

bool App::saveDoc(Document& doc, bool forceDialog)
{
    if (!doc.path().empty() && !forceDialog)
    {
        if (!doc.save())
        {
            showMessage("Save", "Cannot write " + doc.path());
            return false;
        }
        m_conflicts.erase(std::remove(m_conflicts.begin(), m_conflicts.end(), &doc), m_conflicts.end());
        m_status = "Saved " + doc.displayName();
        m_statusUntil = now() + 3;
        return true;
    }
    bool ok = true;
    std::string dir = doc.path().empty() ? m_root : fs::path(doc.path()).parent_path().string();
    std::string path = nativeDialog(Dialog::Save, dir, doc.path().empty() ? "untitled.cpp" : doc.displayName(), ok);
    if (!ok)
    {
        m_saveAsDoc = &doc;
        m_prompt = PromptSaveAs;
        m_promptBase = dir;
        std::snprintf(m_promptBuf, sizeof m_promptBuf, "%s/%s", dir.c_str(), "untitled.cpp");
        m_openPrompt = true;
        return false;
    }
    if (path.empty()) return false;
    if (!doc.saveAs(path))
    {
        showMessage("Save As", "Cannot write " + path);
        return false;
    }
    m_tree.refresh();
    return true;
}

void App::saveAll()
{
    for (auto& d : m_docs)
        if (d->dirty() && !d->path().empty()) saveDoc(*d);
}

void App::closeDoc(int index, bool force)
{
    if (index < 0 || index >= int(m_docs.size())) return;
    Document* d = m_docs[size_t(index)].get();
    if (d->dirty() && !force)
    {
        m_pendingCloseIndex = index;
        m_openCloseConfirm = true;
        return;
    }
    m_conflicts.erase(std::remove(m_conflicts.begin(), m_conflicts.end(), d), m_conflicts.end());
    m_docs.erase(m_docs.begin() + index);
    if (m_active >= int(m_docs.size())) m_active = int(m_docs.size()) - 1;
    else if (m_active > index) --m_active;
    m_selectTab = m_active;
}

void App::requestQuit()
{
    bool dirty = std::any_of(m_docs.begin(), m_docs.end(), [](const auto& d) { return d->dirty(); });
    if (!dirty) quitConfirmed = true;
    else m_openQuitConfirm = true;
    wakeUi();
}

// --- Project config -------------------------------------------------------------------

void App::loadProjectConfig()
{
    m_buildCmd.clear();
    m_runCmd.clear();
    m_runArgs.clear();
    std::string text;
    if (!readTextFile(m_root + "/.sfide", text)) return;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (k == "build") m_buildCmd = v;
        else if (k == "run") m_runCmd = v;
        else if (k == "args") m_runArgs = v;
    }
}

void App::saveProjectConfig()
{
    if (m_root.empty()) return;
    std::string path = m_root + "/.sfide";
    if (m_buildCmd.empty() && m_runCmd.empty() && m_runArgs.empty())
    {
        std::error_code ec;
        fs::remove(path, ec);
        return;
    }
    std::string out = "# SF-IDE project settings\n";
    if (!m_buildCmd.empty()) out += "build=" + m_buildCmd + "\n";
    if (!m_runCmd.empty()) out += "run=" + m_runCmd + "\n";
    if (!m_runArgs.empty()) out += "args=" + m_runArgs + "\n";
    writeTextFile(path, out);
}

void App::refreshGitBranch()
{
    m_gitBranch.clear();
    if (m_root.empty()) return;
    // Walk up to find .git/HEAD; no need to spawn git for this.
    fs::path p = m_root;
    for (int i = 0; i < 8 && !p.empty(); ++i)
    {
        std::string head;
        if (readTextFile((p / ".git" / "HEAD").string(), head))
        {
            head = trim(head);
            const std::string ref = "ref: refs/heads/";
            m_gitBranch = head.rfind(ref, 0) == 0 ? head.substr(ref.size()) : head.substr(0, 7);
            return;
        }
        if (p == p.parent_path()) break;
        p = p.parent_path();
    }
}

// --- Build / run / debug ------------------------------------------------------------

void App::build(int then)
{
    if (m_build.running()) return;
    if (settings.saveBeforeBuild) saveAll();

    std::string cmd = !m_buildCmd.empty() ? m_buildCmd : m_cmds.build;
    std::string dir = m_root;
    m_singleFileBuild = false;
    Document* doc = activeDoc();
    if (cmd.empty() && doc && !doc->path().empty())
    {
        // No project build: compile the current C / C++ file on its own.
        std::string ext = fs::path(doc->path()).extension().string();
        bool c = ext == ".c";
        bool cpp = ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".mm";
        if (c || cpp)
        {
            std::string out = "/tmp/sf-ide-" + std::to_string(getuid());
            std::string exe = out + "/" + fs::path(doc->path()).stem().string();
            cmd = "mkdir -p " + shellQuote(out) + " && " + (c ? "cc -std=c17" : "c++ -std=c++20") +
                  " -g -O0 -Wall -Wextra " + shellQuote(doc->path()) + " -o " + shellQuote(exe);
            dir = fs::path(doc->path()).parent_path().string();
            m_lastExe = exe;
            m_singleFileBuild = true;
        }
    }
    if (cmd.empty())
    {
        showMessage("Build", "No build command for this folder.\n\nSF-IDE detects CMake, Make, Swift and Meson "
                             "projects, and builds a single open .c / .cpp file on its own. You can set a custom "
                             "command in Project Settings.");
        return;
    }
    if (dir.empty()) dir = ".";

    m_buildLines.clear();
    m_problems.clear();
    for (auto& d : m_docs) d->diagnostics.clear();
    m_buildLines.push_back({ "$ " + cmd, 3 });
    m_after = After(then);
    m_build.start(cmd, dir);
    m_buildScroll = true;
    m_selectBottomTab = TabOutput;
    settings.showBottom = true;
}

std::string App::resolvedRunCommand() const
{
    std::string args = m_runArgs.empty() ? "" : " " + m_runArgs;
    if (!m_runCmd.empty()) return m_runCmd + args;
    if (!m_cmds.run.empty()) return m_cmds.run + args;
    if (!m_lastExe.empty()) return shellQuote(m_lastExe) + args;
    return {};
}

void App::runProgram()
{
    if (!m_singleFileBuild && m_lastExe.empty() && m_runCmd.empty() && !m_root.empty())
    {
        std::string dir = fs::exists(m_root + "/build") ? m_root + "/build" : m_root;
        m_lastExe = findNewestExecutable(dir);
    }
    std::string cmd = resolvedRunCommand();
    if (cmd.empty())
    {
        showMessage("Run", "Nothing to run yet. Build the project first, or set a run command in Project Settings.");
        return;
    }
    m_run.stop();
    std::string dir = m_root.empty() ? fs::path(m_lastExe).parent_path().string() : m_root;
    m_run.start({ userShell(), "-l", "-c", cmd }, dir);
    m_selectBottomTab = TabRun;
    settings.showBottom = true;
    m_run.focus();
}

void App::debugProgram()
{
    if (!m_singleFileBuild && m_lastExe.empty() && !m_root.empty())
    {
        std::string dir = fs::exists(m_root + "/build") ? m_root + "/build" : m_root;
        m_lastExe = findNewestExecutable(dir);
    }
    std::string exe = m_lastExe;
    if (!m_runCmd.empty())
    {
        auto parts = splitArgs(m_runCmd);
        if (!parts.empty())
        {
            fs::path p = parts[0];
            if (p.is_relative()) p = fs::path(m_root) / p;
            if (fs::exists(p)) exe = p.string();
        }
    }
    if (exe.empty() || !fs::exists(exe))
    {
        showMessage("Debug", "No executable found. Build the project first.");
        return;
    }
    std::vector<std::string> argv;
    std::string lldb = findProgram("lldb", { "/usr/bin", "/Library/Developer/CommandLineTools/usr/bin" });
    std::string gdb = lldb.empty() ? findProgram("gdb") : "";
    if (!lldb.empty())
    {
        argv = { lldb };
        for (auto& d : m_docs)
            for (int line : d->breakpoints)
                argv.insert(argv.end(), { "-o", "breakpoint set --file " + shellQuote(d->path()) + " --line " +
                                                     std::to_string(line) });
        argv.insert(argv.end(), { "-o", "run", "--", exe });
    }
    else if (!gdb.empty())
    {
        argv = { gdb, "-q" };
        for (auto& d : m_docs)
            for (int line : d->breakpoints)
                argv.insert(argv.end(), { "-ex", "break " + d->path() + ":" + std::to_string(line) });
        argv.insert(argv.end(), { "-ex", "run", "--args", exe });
    }
    else
    {
        showMessage("Debug", "Neither lldb nor gdb was found. On macOS install the Xcode command line tools: "
                             "xcode-select --install");
        return;
    }
    for (auto& a : splitArgs(m_runArgs)) argv.push_back(a);
    m_debug.stop();
    m_debug.start(argv, m_root.empty() ? fs::path(exe).parent_path().string() : m_root);
    m_selectBottomTab = TabDebug;
    settings.showBottom = true;
    m_debug.focus();
}

void App::stopAll()
{
    if (m_build.running()) m_build.cancel();
    if (m_run.running()) m_run.stop();
    if (m_debug.running()) m_debug.stop();
}

// Follow lldb / gdb: "frame #0: ... at main.cpp:6:16" or "Breakpoint 1, f () at /p/main.cpp:6"
// moves the green execution marker in the editor.
void App::followDebugger()
{
    const std::string& text = m_debug.recentText();
    size_t frame = text.rfind("frame #0:");
    size_t gdbFrame = text.rfind("Breakpoint ");
    size_t gdb0 = text.rfind("\n#0 ");
    size_t start = std::string::npos;
    for (size_t c : { frame, gdbFrame, gdb0 })
        if (c != std::string::npos && (start == std::string::npos || c > start)) start = c;
    bool resumed = text.rfind("resuming") != std::string::npos && (start == std::string::npos || text.rfind("resuming") > start);
    bool exited = text.rfind(" exited with status") != std::string::npos &&
                  (start == std::string::npos || text.rfind(" exited with status") > start);
    if (resumed || exited || start == std::string::npos)
    {
        if (resumed || exited) for (auto& d : m_docs) d->execLine = 0;
        return;
    }
    size_t eol = text.find('\n', start);
    std::string line = text.substr(start, eol == std::string::npos ? std::string::npos : eol - start);
    size_t at = line.rfind(" at ");
    if (at == std::string::npos) return;
    std::string loc = line.substr(at + 4);
    // loc = file:line[:col]
    size_t c1 = loc.find(':');
    if (c1 == std::string::npos) return;
    std::string file = loc.substr(0, c1);
    int lineNo = std::atoi(loc.c_str() + c1 + 1);
    if (lineNo <= 0) return;

    // lldb prints the base name; find the matching file.
    std::string full;
    if (!file.empty() && file[0] == '/') full = file;
    else
    {
        for (auto& d : m_docs)
            if (fs::path(d->path()).filename() == file) { full = d->path(); break; }
        if (full.empty())
            for (const auto& f : m_tree.allFiles())
                if (fs::path(f).filename() == file) { full = f; break; }
    }
    if (full.empty()) return;
    m_debug.clearRecentText();
    for (auto& d : m_docs) d->execLine = 0;
    openFile(full, lineNo);
    if (Document* d = activeDoc()) d->execLine = lineNo;
    m_debug.focus();
}

void App::applyDiagnostics()
{
    for (auto& d : m_docs)
    {
        d->diagnostics.clear();
        for (const auto& p : m_problems)
            if (p.file == d->path()) d->diagnostics.push_back({ p.line, p.col, p.error, p.message });
    }
}

// --- Terminals / clil -----------------------------------------------------------------

void App::startShell(const std::string& cwd)
{
    std::string dir = !cwd.empty() ? cwd : (m_root.empty() ? homeDir() : m_root);
    m_shell.start({ userShell(), "-l" }, dir);
}

std::string App::findClil() const
{
    if (!settings.clilPath.empty()) return findProgram(settings.clilPath);
    // Bundled next to the IDE (SF-IDE.app/Contents/MacOS/clil or build dir), then PATH.
    std::string exeDir = executableDir();
    for (const std::string& p : { exeDir + "/clil", exeDir + "/../Resources/clil", exeDir + "/external/clil/clil" })
        if (access(p.c_str(), X_OK) == 0) return p;
    return findProgram("clil");
}

void App::startClil()
{
    std::string clil = findClil();
    m_clilStarted = true;
    if (clil.empty())
    {
        m_clilMissingReason = settings.clilPath.empty()
                                  ? "clil was not found next to SF-IDE or on your PATH."
                                  : "clil was not found at " + settings.clilPath + ".";
        return;
    }
    m_clilMissingReason.clear();
    std::vector<std::string> argv = { clil };
    for (auto& a : splitArgs(settings.clilArgs)) argv.push_back(a);
    std::vector<std::string> env;
    if (!settings.lmHost.empty()) env.push_back("LMSTUDIO_HOST=" + settings.lmHost);
    m_clil.start(argv, m_root.empty() ? homeDir() : m_root, env);
}

void App::ensureClil()
{
    settings.showRight = true;
    if (!m_clilStarted || (!m_clil.running() && m_clilMissingReason.empty() && !m_clil.started())) startClil();
}

void App::sendToClil(const std::string& prompt, bool submit)
{
    ensureClil();
    if (!m_clil.running())
    {
        m_status = "clil is not running";
        m_statusUntil = now() + 4;
        return;
    }
    // Only type into clil while it waits at its "›" prompt; otherwise the
    // text would land in the middle of a reply or an approval question.
    std::string line = m_clil.cursorLineText();
    bool atPrompt = line.rfind("\xE2\x80\xBA", 0) == 0;  // "›"
    if (!atPrompt)
    {
        m_status = "clil is busy - wait until it shows its prompt";
        m_statusUntil = now() + 4;
        m_clil.focus();
        return;
    }
    m_clil.sendText(prompt, true);
    if (submit) m_clil.sendRaw("\r");
    m_clil.focus();
}

void App::askClilAboutSelection(const char* verb)
{
    Document* doc = activeDoc();
    if (!doc) return;
    std::string file = doc->path().empty() ? doc->displayName() : rel(doc->path());
    std::string prompt;
    if (doc->hasSelection())
    {
        int a = doc->lineOf(doc->selMin()) + 1, b = doc->lineOf(doc->selMax()) + 1;
        prompt = std::string(verb) + " this code from " + file + " (lines " + std::to_string(a) + "-" +
                 std::to_string(b) + "):\n```\n" + doc->selectedText() + "\n```";
    }
    else
    {
        prompt = std::string(verb) + " @" + file;
    }
    sendToClil(prompt, true);
}

void App::askClilFixProblems()
{
    if (m_problems.empty() && m_build.state() != BuildRunner::State::Failed)
    {
        sendToClil("Build the project and fix any errors.", true);
        return;
    }
    std::string list;
    int n = 0;
    for (const auto& p : m_problems)
    {
        if (!p.error && n > 10) continue;
        list += rel(p.file) + ":" + std::to_string(p.line) + ":" + std::to_string(p.col) + ": " +
                (p.error ? "error: " : "warning: ") + p.message + "\n";
        if (++n >= 30) break;
    }
    if (list.empty())
    {
        // No parsed diagnostics: use the tail of the build output.
        size_t start = m_buildLines.size() > 40 ? m_buildLines.size() - 40 : 0;
        for (size_t i = start; i < m_buildLines.size(); ++i) list += m_buildLines[i].text + "\n";
    }
    std::string cmd = !m_buildCmd.empty() ? m_buildCmd : m_cmds.build;
    sendToClil("The build fails" + (cmd.empty() ? std::string() : " (`" + cmd + "`)") + ":\n```\n" + list +
                   "```\nFix these errors.",
               true);
}

// --- Periodic work ---------------------------------------------------------------------

void App::pump()
{
    bool changed = false;
    changed |= m_shell.pump();
    changed |= m_clil.pump();
    changed |= m_run.pump();
    if (m_debug.pump())
    {
        changed = true;
        followDebugger();
    }
    else if (!m_debug.running())
    {
        for (auto& d : m_docs) d->execLine = 0;
    }

    static BuildRunner::State lastState = BuildRunner::State::Idle;
    size_t before = m_problems.size();
    if (m_build.poll(m_buildLines, m_problems))
    {
        changed = true;
        m_buildScroll = true;
        if (m_buildLines.size() > 20000) m_buildLines.erase(m_buildLines.begin(), m_buildLines.begin() + 5000);
    }
    if (m_problems.size() != before) applyDiagnostics();

    BuildRunner::State st = m_build.state();
    if (st != lastState)
    {
        changed = true;
        if (lastState == BuildRunner::State::Running)
        {
            if (st == BuildRunner::State::Succeeded)
            {
                if (!m_singleFileBuild && m_runCmd.empty() && m_cmds.run.empty())
                {
                    std::string dir = fs::exists(m_root + "/build") ? m_root + "/build" : m_root;
                    std::string exe = findNewestExecutable(dir);
                    if (!exe.empty()) m_lastExe = exe;
                }
                if (m_after == AfterRun) runProgram();
                else if (m_after == AfterDebug) debugProgram();
            }
            else if (st == BuildRunner::State::Failed && !m_problems.empty())
            {
                m_selectBottomTab = TabProblems;
            }
            m_after = AfterNone;
            m_tree.refresh();
        }
        lastState = st;
    }
    if (changed) wakeUi();
}

bool App::busy() const
{
    return m_build.running();
}

void App::tick()
{
    saveSettings();
    m_tree.refresh();
    refreshGitBranch();
    // Files changed outside the IDE (clil, git, another editor): reload clean
    // documents, ask before touching ones with unsaved edits.
    for (auto& d : m_docs)
    {
        if (!d->changedOnDisk()) continue;
        if (!d->dirty())
        {
            d->reloadFromDisk();
            applyDiagnostics();
            m_status = "Reloaded " + d->displayName() + " (changed on disk)";
            m_statusUntil = now() + 4;
            wakeUi();
        }
        else if (std::find(m_conflicts.begin(), m_conflicts.end(), d.get()) == m_conflicts.end())
        {
            m_conflicts.push_back(d.get());
            wakeUi();
        }
    }
}

// --- Frame -------------------------------------------------------------------------------

bool App::splitter(const char* id, bool vertical, float x, float y, float len, float& value, float sign,
                   float minV, float maxV)
{
    const Theme& t = theme();
    const float thick = 5.0f;
    ImGui::SetCursorPos(ImVec2(x, y));
    ImGui::InvisibleButton(id, vertical ? ImVec2(thick, len) : ImVec2(len, thick));
    bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    if (hovered || active) ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
    if (active)
    {
        ImVec2 d = ImGui::GetIO().MouseDelta;
        value = std::clamp(value + sign * (vertical ? d.x : d.y), minV, maxV);
    }
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 c = (hovered || active) ? t.accent : t.border;
    if (vertical) dl->AddLine(ImVec2((a.x + b.x) * 0.5f, a.y), ImVec2((a.x + b.x) * 0.5f, b.y), c, (hovered || active) ? 2.0f : 1.0f);
    else dl->AddLine(ImVec2(a.x, (a.y + b.y) * 0.5f), ImVec2(b.x, (a.y + b.y) * 0.5f), c, (hovered || active) ? 2.0f : 1.0f);
    return active;
}

void App::panelHeader(const char* title)
{
    const Theme& t = theme();
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, col4(t.textDim));
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
}

void App::frame()
{
    const Theme& t = theme();
    handleShortcuts();
    drawMenuBar();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, col4(t.panel));
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    const float W = vp->WorkSize.x, H0 = vp->WorkSize.y;
    const float statusH = std::floor(ImGui::GetFrameHeight() + 4);
    const float H = H0 - statusH;
    const float G = 5.0f;

    // Fixed layout: explorer | editor (+ bottom panel) | clil. Sizes are
    // adjustable with the splitters; panels never float or re-dock.
    float leftW = settings.showLeft ? std::clamp(settings.leftW, 160.0f, std::max(160.0f, W * 0.35f)) : 0.0f;
    float rightW = settings.showRight ? std::clamp(settings.rightW, 300.0f, std::max(300.0f, W * 0.5f)) : 0.0f;
    float minCenter = 320.0f;
    if (W - leftW - rightW - 2 * G < minCenter && rightW > 0) rightW = std::max(0.0f, W - leftW - 2 * G - minCenter);
    float centerX = leftW + (leftW > 0 ? G : 0);
    float centerW = W - centerX - rightW - (rightW > 0 ? G : 0);
    float bottomH = settings.showBottom ? std::clamp(settings.bottomH, 100.0f, std::max(100.0f, H * 0.75f)) : 0.0f;
    float editorH = H - bottomH - (bottomH > 0 ? G : 0);

    if (leftW > 0)
    {
        ImGui::SetCursorPos(ImVec2(0, 0));
        drawExplorer(leftW, H);
        splitter("##split_left", true, leftW, 0, H, settings.leftW, +1, 160, W * 0.35f);
    }
    ImGui::SetCursorPos(ImVec2(centerX, 0));
    drawEditorArea(centerW, editorH);
    if (bottomH > 0)
    {
        splitter("##split_bottom", false, centerX, editorH, centerW, settings.bottomH, -1, 100, H * 0.75f);
        ImGui::SetCursorPos(ImVec2(centerX, H - bottomH));
        drawBottomPanel(centerW, bottomH);
    }
    if (rightW > 0)
    {
        splitter("##split_right", true, W - rightW - G, 0, H, settings.rightW, -1, 300, W * 0.5f);
        ImGui::SetCursorPos(ImVec2(W - rightW, 0));
        drawAssistant(rightW, H);
    }
    ImGui::SetCursorPos(ImVec2(0, H));
    drawStatusBar(W, statusH);

    ImGui::End();
    drawPopups();
}

// --- Menu bar ------------------------------------------------------------------------------

void App::drawMenuBar()
{
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 7));
    if (!ImGui::BeginMainMenuBar())
    {
        ImGui::PopStyleVar();
        return;
    }
    ImGui::PopStyleVar();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 7));
    Document* doc = activeDoc();
    if (ImGui::BeginMenu("File"))
    {
        if (ImGui::MenuItem("New File", sc("N").c_str())) newFile();
        if (ImGui::MenuItem("Open File...", sc("O").c_str()))
        {
            bool ok = true;
            std::string p = nativeDialog(Dialog::File, m_root, "", ok);
            if (!ok) { m_prompt = PromptOpenFile; m_promptBuf[0] = 0; m_openPrompt = true; }
            else if (!p.empty()) openFile(p);
        }
        if (ImGui::MenuItem("Open Folder...", sc("O", true).c_str()))
        {
            bool ok = true;
            std::string p = nativeDialog(Dialog::Folder, m_root, "", ok);
            if (!ok) { m_prompt = PromptOpenFolder; m_promptBuf[0] = 0; m_openPrompt = true; }
            else if (!p.empty()) openFolder(p);
        }
        if (ImGui::BeginMenu("Open Recent", !settings.recent.empty()))
        {
            for (const auto& r : settings.recent)
                if (ImGui::MenuItem(r.c_str())) { openFolder(r); break; }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Quick Open...", sc("P").c_str(), false, !m_root.empty())) m_openQuick = true;
        ImGui::Separator();
        if (ImGui::MenuItem("Save", sc("S").c_str(), false, doc != nullptr)) saveDoc(*doc);
        if (ImGui::MenuItem("Save As...", sc("S", true).c_str(), false, doc != nullptr)) saveDoc(*doc, true);
        if (ImGui::MenuItem("Save All", sc("S", false, true).c_str())) saveAll();
        ImGui::Separator();
        if (ImGui::MenuItem("Close Tab", sc("W").c_str(), false, doc != nullptr)) closeDoc(m_active);
        if (ImGui::MenuItem("Project Settings...", nullptr, false, !m_root.empty())) m_openSettings = true;
        if (ImGui::MenuItem("Settings...", sc(",").c_str())) m_openSettings = true;
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", sc("Q").c_str())) requestQuit();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit"))
    {
        bool e = doc != nullptr;
        if (ImGui::MenuItem("Undo", sc("Z").c_str(), false, e && doc->canUndo())) doc->undo();
        if (ImGui::MenuItem("Redo", sc("Z", true).c_str(), false, e && doc->canRedo())) doc->redo();
        ImGui::Separator();
        if (ImGui::MenuItem("Cut", sc("X").c_str(), false, e)) m_editor.cut(*doc);
        if (ImGui::MenuItem("Copy", sc("C").c_str(), false, e)) m_editor.copy(*doc);
        if (ImGui::MenuItem("Paste", sc("V").c_str(), false, e)) m_editor.paste(*doc);
        if (ImGui::MenuItem("Select All", sc("A").c_str(), false, e)) doc->selectAll();
        ImGui::Separator();
        if (ImGui::MenuItem("Find", sc("F").c_str(), false, e)) { m_findOpen = true; m_focusFind = true; }
        if (ImGui::MenuItem("Replace", sc("F", false, true).c_str(), false, e)) { m_findOpen = m_replaceOpen = true; m_focusFind = true; }
        if (ImGui::MenuItem("Find Next", sc("G").c_str(), false, e && m_findBuf[0])) m_editor.findNext(*doc, false);
        if (ImGui::MenuItem("Go to Line...", sc("L").c_str(), false, e)) m_openGoto = true;
        ImGui::Separator();
        if (ImGui::MenuItem("Toggle Comment", sc("/").c_str(), false, e)) m_editor.toggleComment(*doc);
        if (ImGui::MenuItem("Indent", sc("]").c_str(), false, e)) m_editor.indent(*doc, false);
        if (ImGui::MenuItem("Outdent", sc("[").c_str(), false, e)) m_editor.indent(*doc, true);
        if (ImGui::MenuItem("Duplicate Line", sc("D").c_str(), false, e)) m_editor.duplicateLine(*doc);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View"))
    {
        ImGui::MenuItem("Explorer", sc("1").c_str(), &settings.showLeft);
        ImGui::MenuItem("Bottom Panel", sc("J").c_str(), &settings.showBottom);
        if (ImGui::MenuItem("clil Assistant", sc("I").c_str(), &settings.showRight) && settings.showRight) ensureClil();
        ImGui::Separator();
        if (ImGui::MenuItem("Terminal", isMac() ? "\xE2\x8C\x83`" : "Ctrl+`")) { m_selectBottomTab = TabTerminal; settings.showBottom = true; m_shell.focus(); }
        if (ImGui::MenuItem("Problems")) { m_selectBottomTab = TabProblems; settings.showBottom = true; }
        if (ImGui::MenuItem("Build Output")) { m_selectBottomTab = TabOutput; settings.showBottom = true; }
        ImGui::Separator();
        if (ImGui::MenuItem("Dark Theme", nullptr, settings.dark)) { settings.dark = !settings.dark; setTheme(settings.dark); }
        if (ImGui::MenuItem("Bigger Text", sc("+").c_str())) { settings.editorFontSize += 1; settings.uiFontSize += 0.5f; fontsDirty = true; }
        if (ImGui::MenuItem("Smaller Text", sc("-").c_str())) { settings.editorFontSize -= 1; settings.uiFontSize -= 0.5f; fontsDirty = true; }
        if (ImGui::MenuItem("Reset Text Size", sc("0").c_str())) { settings.editorFontSize = 13; settings.uiFontSize = 13.5f; fontsDirty = true; }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Build"))
    {
        bool can = !m_build.running();
        if (ImGui::MenuItem("Build", sc("B").c_str(), false, can)) build(AfterNone);
        if (ImGui::MenuItem("Build & Run", sc("R").c_str(), false, can)) build(AfterRun);
        if (ImGui::MenuItem("Run Without Building", sc("R", true).c_str())) runProgram();
        if (ImGui::MenuItem("Stop", sc(".").c_str(), false, m_build.running() || m_run.running() || m_debug.running())) stopAll();
        ImGui::Separator();
        if (ImGui::MenuItem("Clean Build Folder", nullptr, false, can && !m_root.empty() && fs::exists(m_root + "/build")))
        {
            std::error_code ec;
            fs::remove_all(m_root + "/build", ec);
            m_lastExe.clear();
            m_tree.refresh();
        }
        if (ImGui::MenuItem("Project Settings...", nullptr, false, !m_root.empty())) m_openSettings = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Debug"))
    {
        bool can = !m_build.running();
        if (ImGui::MenuItem("Build & Debug", "F5", false, can)) build(AfterDebug);
        if (ImGui::MenuItem("Debug Without Building", "\xE2\x87\xA7" "F5")) debugProgram();
        if (ImGui::MenuItem("Toggle Breakpoint", "F9", false, doc != nullptr)) doc->toggleBreakpoint(doc->lineOf(doc->cursor) + 1);
        if (ImGui::MenuItem("Remove All Breakpoints")) for (auto& d : m_docs) d->breakpoints.clear();
        ImGui::Separator();
        bool dbg = m_debug.running();
        if (ImGui::MenuItem("Continue", nullptr, false, dbg)) m_debug.sendText("continue\n", false);
        if (ImGui::MenuItem("Step Over", "F10", false, dbg)) m_debug.sendText("next\n", false);
        if (ImGui::MenuItem("Step Into", "F11", false, dbg)) m_debug.sendText("step\n", false);
        if (ImGui::MenuItem("Step Out", "\xE2\x87\xA7" "F11", false, dbg)) m_debug.sendText("finish\n", false);
        if (ImGui::MenuItem("Stop Debugging", nullptr, false, dbg)) m_debug.stop();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("clil"))
    {
        bool e = doc != nullptr;
        if (ImGui::MenuItem("Show Assistant", sc("I").c_str())) { ensureClil(); m_clil.focus(); }
        if (ImGui::MenuItem("Explain Selection / File", nullptr, false, e)) askClilAboutSelection("Explain");
        if (ImGui::MenuItem("Find Bugs in Selection / File", nullptr, false, e)) askClilAboutSelection("Find bugs in");
        if (ImGui::MenuItem("Fix Build Errors", nullptr, false, !m_root.empty())) askClilFixProblems();
        if (ImGui::MenuItem("Write Commit Message", nullptr, false, !m_root.empty()))
            sendToClil("Look at `git diff` and write a short commit message for these changes.", true);
        ImGui::Separator();
        if (ImGui::MenuItem("New Conversation")) sendToClil("/clear", true);
        if (ImGui::MenuItem("Models...")) sendToClil("/models", true);
        if (ImGui::MenuItem("Restart clil")) { m_clil.stop(); startClil(); }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help"))
    {
        if (ImGui::MenuItem("About SF-IDE")) m_openAbout = true;
        ImGui::EndMenu();
    }
    ImGui::PopStyleVar(2);

    drawToolbarButtons();
    ImGui::EndMainMenuBar();
}

void App::drawToolbarButtons()
{
    const Theme& t = theme();
    // Project name centred, run controls on the right.
    std::string title = m_root.empty() ? "SF-IDE" : fs::path(m_root).filename().string();
    if (!m_gitBranch.empty()) title += "  \xC2\xB7  " + m_gitBranch;
    ImVec2 ts = ImGui::CalcTextSize(title.c_str());
    float w = ImGui::GetWindowWidth();
    float saveX = ImGui::GetCursorPosX();
    if (saveX < w * 0.5f - ts.x * 0.5f - 20)
    {
        ImGui::SetCursorPosX(w * 0.5f - ts.x * 0.5f);
        ImGui::PushStyleColor(ImGuiCol_Text, col4(t.textDim));
        ImGui::TextUnformatted(title.c_str());
        ImGui::PopStyleColor();
    }

    const bool building = m_build.running();
    const bool runningAny = building || m_run.running() || m_debug.running();
    float buttonsW = 290;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX() + 10, w - buttonsW));
    if (iconButton("##tb_build", Icon::Build, "Build", !building, ("Build  " + sc("B")).c_str(), t.text)) build(AfterNone);
    if (iconButton("##tb_run", Icon::Play, "Run", !building, ("Build & Run  " + sc("R")).c_str(), t.ok)) build(AfterRun);
    if (iconButton("##tb_debug", Icon::Bug, "Debug", !building, "Build & Debug with lldb  F5", t.warning)) build(AfterDebug);
    if (iconButton("##tb_stop", Icon::Stop, "Stop", runningAny, ("Stop  " + sc(".")).c_str(), t.error)) stopAll();
}

// --- Shortcuts -----------------------------------------------------------------------------

void App::handleShortcuts()
{
    ImGuiIO& io = ImGui::GetIO();
    Document* doc = activeDoc();
    if (chord(ImGuiKey_N)) newFile();
    if (chord(ImGuiKey_O))
    {
        bool ok = true;
        std::string p = nativeDialog(Dialog::File, m_root, "", ok);
        if (!ok) { m_prompt = PromptOpenFile; m_promptBuf[0] = 0; m_openPrompt = true; }
        else if (!p.empty()) openFile(p);
    }
    if (chord(ImGuiKey_O, true))
    {
        bool ok = true;
        std::string p = nativeDialog(Dialog::Folder, m_root, "", ok);
        if (!ok) { m_prompt = PromptOpenFolder; m_promptBuf[0] = 0; m_openPrompt = true; }
        else if (!p.empty()) openFolder(p);
    }
    if (chord(ImGuiKey_P) && !m_root.empty()) m_openQuick = true;
    if (doc && chord(ImGuiKey_S)) saveDoc(*doc);
    if (doc && chord(ImGuiKey_S, true)) saveDoc(*doc, true);
    if (chord(ImGuiKey_S, false, true)) saveAll();
    if (doc && chord(ImGuiKey_W)) closeDoc(m_active);
    if (chord(ImGuiKey_Q)) requestQuit();
    if (doc && chord(ImGuiKey_F)) { m_findOpen = true; m_focusFind = true; if (doc->hasSelection() && doc->selMax() - doc->selMin() < 200) { std::snprintf(m_findBuf, sizeof m_findBuf, "%s", doc->selectedText().c_str()); m_editor.findText = m_findBuf; } }
    if (doc && (chord(ImGuiKey_F, false, true) || (!isMac() && chord(ImGuiKey_H)))) { m_findOpen = m_replaceOpen = true; m_focusFind = true; }
    if (doc && chord(ImGuiKey_G, false, false, true)) m_editor.findNext(*doc, false);
    if (doc && chord(ImGuiKey_G, true, false, true)) m_editor.findNext(*doc, true);
    if (doc && chord(ImGuiKey_L)) m_openGoto = true;
    if (chord(ImGuiKey_B) && !m_build.running()) build(AfterNone);
    if (chord(ImGuiKey_R) && !m_build.running()) build(AfterRun);
    if (chord(ImGuiKey_R, true)) runProgram();
    if (chord(ImGuiKey_Period)) stopAll();
    if (chord(ImGuiKey_1)) settings.showLeft = !settings.showLeft;
    if (chord(ImGuiKey_J)) settings.showBottom = !settings.showBottom;
    if (chord(ImGuiKey_I))
    {
        if (!settings.showRight) { ensureClil(); m_clil.focus(); }
        else m_clil.focus();
    }
    if (chord(ImGuiKey_Comma)) m_openSettings = true;
    if (chord(ImGuiKey_Equal) || chord(ImGuiKey_Equal, true) || chord(ImGuiKey_KeypadAdd)) { settings.editorFontSize += 1; settings.uiFontSize += 0.5f; fontsDirty = true; }
    if (chord(ImGuiKey_Minus) || chord(ImGuiKey_KeypadSubtract)) { settings.editorFontSize -= 1; settings.uiFontSize -= 0.5f; fontsDirty = true; }
    if (chord(ImGuiKey_0)) { settings.editorFontSize = 13; settings.uiFontSize = 13.5f; fontsDirty = true; }
    // Ctrl+` (physical Control on macOS) toggles the terminal.
    bool physCtrl = isMac() ? io.KeySuper : io.KeyCtrl;
    if (physCtrl && ImGui::IsKeyPressed(ImGuiKey_GraveAccent, false))
    {
        if (settings.showBottom && m_bottomTab == TabTerminal) settings.showBottom = false;
        else { settings.showBottom = true; m_selectBottomTab = TabTerminal; m_shell.focus(); }
    }
    // Tabs: Cmd+Shift+[ / ], Ctrl+Tab.
    if (!m_docs.empty())
    {
        int n = int(m_docs.size());
        if (chord(ImGuiKey_RightBracket, true) || (physCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Tab)))
        { m_active = (m_active + 1) % n; m_selectTab = m_active; m_editor.focus(); }
        if (chord(ImGuiKey_LeftBracket, true) || (physCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Tab)))
        { m_active = (m_active + n - 1) % n; m_selectTab = m_active; m_editor.focus(); }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false) && !m_build.running())
    {
        if (io.KeyShift) debugProgram();
        else if (m_debug.running()) m_debug.sendText("continue\n", false);
        else build(AfterDebug);
    }
    if (doc && ImGui::IsKeyPressed(ImGuiKey_F9, false)) doc->toggleBreakpoint(doc->lineOf(doc->cursor) + 1);
    if (m_debug.running() && ImGui::IsKeyPressed(ImGuiKey_F10, false)) m_debug.sendText("next\n", false);
    if (m_debug.running() && ImGui::IsKeyPressed(ImGuiKey_F11, false)) m_debug.sendText(io.KeyShift ? "finish\n" : "step\n", false);
    if (m_findOpen && ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))
    {
        m_findOpen = m_replaceOpen = false;
        m_editor.findText.clear();
        m_editor.focus();
    }
}

// --- Panels ------------------------------------------------------------------------------------

void App::drawExplorer(float w, float h)
{
    const Theme& t = theme();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col4(t.panel));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
    ImGui::BeginChild("##explorer", ImVec2(w, h), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));
    std::string name = m_root.empty() ? "EXPLORER" : fs::path(m_root).filename().string();
    for (auto& c : name) if (m_root.empty()) c = char(std::toupper((unsigned char)c));
    panelHeader(name.c_str());
    ImGui::SameLine(std::max(0.0f, ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 56));
    if (flatButton("+##newfile", !m_root.empty(), "New file")) handleTreeAction({ FileTree::Action::NewFile, m_root, true });
    ImGui::SameLine(0, 2);
    if (flatButton("\xE2\x80\xA6##openfolder", true, "Open folder..."))
    {
        bool ok = true;
        std::string p = nativeDialog(Dialog::Folder, m_root, "", ok);
        if (!ok) { m_prompt = PromptOpenFolder; m_promptBuf[0] = 0; m_openPrompt = true; }
        else if (!p.empty()) openFolder(p);
    }
    ImGui::PopStyleVar();
    ImGui::Spacing();
    ImGui::BeginChild("##tree", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_None);
    if (m_root.empty())
    {
        ImGui::TextDisabled("No folder open.");
        ImGui::Spacing();
        if (ImGui::Button("Open Folder...", ImVec2(-1, 0)))
        {
            bool ok = true;
            std::string p = nativeDialog(Dialog::Folder, "", "", ok);
            if (!ok) { m_prompt = PromptOpenFolder; m_promptBuf[0] = 0; m_openPrompt = true; }
            else if (!p.empty()) openFolder(p);
        }
    }
    else
    {
        Document* d = activeDoc();
        FileTree::Action a = m_tree.render(d ? d->path() : std::string());
        if (a.kind != FileTree::Action::None) handleTreeAction(a);
    }
    ImGui::EndChild();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void App::handleTreeAction(const FileTree::Action& a)
{
    switch (a.kind)
    {
    case FileTree::Action::Open: openFile(a.path); break;
    case FileTree::Action::AskClil:
        ensureClil();
        if (m_clil.running() && m_clil.cursorLineText().rfind("\xE2\x80\xBA", 0) == 0)
            m_clil.sendText("@" + rel(a.path) + (a.isDir ? "/ " : " "), true);
        m_clil.focus();
        break;
    case FileTree::Action::Reveal: runCapture("open -R " + shellQuote(a.path)); break;
    case FileTree::Action::CopyPath: ImGui::SetClipboardText(a.path.c_str()); break;
    case FileTree::Action::OpenTerminal:
        m_shell.stop();
        startShell(a.path);
        m_selectBottomTab = TabTerminal;
        settings.showBottom = true;
        m_shell.focus();
        break;
    case FileTree::Action::NewFile:
    case FileTree::Action::NewFolder:
        m_prompt = a.kind == FileTree::Action::NewFile ? PromptNewFile : PromptNewFolder;
        m_promptBase = a.isDir ? a.path : fs::path(a.path).parent_path().string();
        m_promptBuf[0] = 0;
        m_openPrompt = true;
        break;
    case FileTree::Action::Rename:
        m_prompt = PromptRename;
        m_promptBase = a.path;
        std::snprintf(m_promptBuf, sizeof m_promptBuf, "%s", fs::path(a.path).filename().string().c_str());
        m_openPrompt = true;
        break;
    case FileTree::Action::Delete:
        m_deletePath = a.path;
        m_openDelete = true;
        break;
    default: break;
    }
}

void App::drawEditorArea(float w, float h)
{
    const Theme& t = theme();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col4(t.editorBg));
    ImGui::BeginChild("##center", ImVec2(w, h), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (m_docs.empty())
    {
        drawWelcome();
        ImGui::EndChild();
        ImGui::PopStyleColor();
        return;
    }

    // Tabs.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col4(t.panel));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
    ImGui::BeginChild("##tabs", ImVec2(0, ImGui::GetFrameHeight() + 8), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12, 5));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(6, 4));
    int closeIndex = -1;
    if (ImGui::BeginTabBar("##doctabs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyScroll |
                                            ImGuiTabBarFlags_NoTabListScrollingButtons))
    {
        for (int i = 0; i < int(m_docs.size()); ++i)
        {
            Document& d = *m_docs[size_t(i)];
            bool open = true;
            ImGuiTabItemFlags flags = d.dirty() ? ImGuiTabItemFlags_UnsavedDocument : 0;
            if (m_selectTab == i) flags |= ImGuiTabItemFlags_SetSelected;
            char label[512];
            std::snprintf(label, sizeof label, "%s###doc%p", d.displayName().c_str(), static_cast<void*>(&d));
            if (ImGui::BeginTabItem(label, &open, flags))
            {
                if (m_selectTab < 0 && m_active != i) { m_active = i; m_editor.focus(); }
                ImGui::EndTabItem();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !d.path().empty()) ImGui::SetTooltip("%s", d.path().c_str());
            if (!open) closeIndex = i;
        }
        ImGui::EndTabBar();
    }
    m_selectTab = -1;
    ImGui::PopStyleVar(2);
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    Document* doc = activeDoc();
    if (doc)
    {
        // Breadcrumb: relative path of the file.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 3));
        ImGui::BeginChild("##crumb", ImVec2(0, ImGui::GetTextLineHeight() + 8), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
        ImGui::PushStyleColor(ImGuiCol_Text, col4(t.textDim));
        std::string crumb = doc->path().empty() ? "untitled" : rel(doc->path());
        for (size_t p = 0; (p = crumb.find('/', p)) != std::string::npos; p += 5) crumb.replace(p, 1, "  \xE2\x80\xBA  ");
        ImGui::TextUnformatted(crumb.c_str());
        ImGui::PopStyleColor();
        ImGui::EndChild();
        ImGui::PopStyleVar();

        // Changed-on-disk banner for documents with unsaved edits.
        if (std::find(m_conflicts.begin(), m_conflicts.end(), doc) != m_conflicts.end())
        {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, col4(withAlpha(t.warning, 40)));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 4));
            ImGui::BeginChild("##conflict", ImVec2(0, ImGui::GetFrameHeight() + 8), ImGuiChildFlags_AlwaysUseWindowPadding);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("This file was changed on disk (by clil or another program).");
            ImGui::SameLine();
            if (ImGui::SmallButton("Reload"))
            {
                doc->reloadFromDisk();
                m_conflicts.erase(std::remove(m_conflicts.begin(), m_conflicts.end(), doc), m_conflicts.end());
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Keep my version"))
            {
                doc->markDiskStateSeen();
                m_conflicts.erase(std::remove(m_conflicts.begin(), m_conflicts.end(), doc), m_conflicts.end());
            }
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }

        if (m_findOpen) drawFindBar(*doc);
        m_editor.font = monoFont;
        m_editor.render(*doc);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    if (closeIndex >= 0) closeDoc(closeIndex);
}

void App::drawFindBar(Document& doc)
{
    const Theme& t = theme();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col4(t.panelAlt));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 5));
    float rows = m_replaceOpen ? 2.0f : 1.0f;
    ImGui::BeginChild("##findbar", ImVec2(0, rows * (ImGui::GetFrameHeight() + 5) + 6), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 5));
    float fieldW = std::min(360.0f, ImGui::GetContentRegionAvail().x * 0.5f);
    if (m_focusFind) { ImGui::SetKeyboardFocusHere(); m_focusFind = false; }
    ImGui::SetNextItemWidth(fieldW);
    bool enter = ImGui::InputTextWithHint("##find", "Find", m_findBuf, sizeof m_findBuf, ImGuiInputTextFlags_EnterReturnsTrue);
    m_editor.findText = m_findBuf;
    if (enter)
    {
        m_editor.findNext(doc, ImGui::GetIO().KeyShift);
        ImGui::SetKeyboardFocusHere(-1);
    }
    ImGui::SameLine();
    int count = m_editor.countMatches(doc);
    ImGui::AlignTextToFramePadding();
    if (m_findBuf[0]) ImGui::TextDisabled(count == 1 ? "1 match" : "%d matches", count);
    else ImGui::TextDisabled("          ");
    ImGui::SameLine();
    if (flatButton("\xE2\x86\x91##prev", count > 0, "Previous (Shift+Enter)")) m_editor.findNext(doc, true);
    ImGui::SameLine(0, 0);
    if (flatButton("\xE2\x86\x93##next", count > 0, "Next (Enter)")) m_editor.findNext(doc, false);
    ImGui::SameLine();
    ImGui::Checkbox("Aa", &m_editor.findCaseSensitive);
    ImGui::SameLine();
    if (flatButton(m_replaceOpen ? "Hide Replace" : "Replace...")) m_replaceOpen = !m_replaceOpen;
    ImGui::SameLine();
    if (flatButton("\xC3\x97##closefind", true, "Close (Esc)"))
    {
        m_findOpen = m_replaceOpen = false;
        m_editor.findText.clear();
        m_editor.focus();
    }
    if (m_replaceOpen)
    {
        ImGui::SetNextItemWidth(fieldW);
        bool rep = ImGui::InputTextWithHint("##replace", "Replace", m_replaceBuf, sizeof m_replaceBuf,
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button("Replace") || rep) m_editor.replaceCurrent(doc, m_replaceBuf);
        ImGui::SameLine();
        if (ImGui::Button("Replace All"))
        {
            int n = m_editor.replaceAll(doc, m_replaceBuf);
            m_status = "Replaced " + std::to_string(n) + " occurrence" + (n == 1 ? "" : "s");
            m_statusUntil = now() + 3;
        }
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void App::drawWelcome()
{
    const Theme& t = theme();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float colW = std::min(460.0f, avail.x - 40);
    ImGui::SetCursorPos(ImVec2((avail.x - colW) * 0.5f, std::max(30.0f, avail.y * 0.18f)));
    ImGui::BeginGroup();
    ImGui::PushFont(monoFont);
    ImGui::SetWindowFontScale(2.0f);
    ImGui::TextUnformatted("SF-IDE");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, col4(t.textDim));
    ImGui::TextUnformatted("A small, native C/C++ IDE with clil built in.");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 18));

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14, 8));
    if (ImGui::Button("Open Folder...", ImVec2(colW * 0.48f, 0)))
    {
        bool ok = true;
        std::string p = nativeDialog(Dialog::Folder, "", "", ok);
        if (!ok) { m_prompt = PromptOpenFolder; m_promptBuf[0] = 0; m_openPrompt = true; }
        else if (!p.empty()) openFolder(p);
    }
    ImGui::SameLine(0, colW * 0.04f);
    if (ImGui::Button("New File", ImVec2(colW * 0.48f, 0))) newFile();
    ImGui::PopStyleVar();

    if (!settings.recent.empty())
    {
        ImGui::Dummy(ImVec2(0, 16));
        panelHeader("RECENT");
        for (const auto& r : settings.recent)
        {
            std::string name = fs::path(r).filename().string();
            ImGui::PushID(r.c_str());
            if (ImGui::Selectable(name.c_str(), false, 0, ImVec2(colW, 0))) { openFolder(r); ImGui::PopID(); break; }
            ImGui::SameLine(std::min(colW * 0.35f, 160.0f));
            ImGui::TextDisabled("%s", r.c_str());
            ImGui::PopID();
        }
    }

    ImGui::Dummy(ImVec2(0, 16));
    panelHeader("SHORTCUTS");
    auto row = [&](const char* what, const std::string& keys) {
        ImGui::TextUnformatted(what);
        ImGui::SameLine(colW * 0.6f);
        ImGui::TextDisabled("%s", keys.c_str());
    };
    row("Quick open", sc("P"));
    row("Build & run", sc("R"));
    row("Build & debug (lldb)", "F5");
    row("Ask clil", sc("I"));
    row("Toggle terminal", isMac() ? "\xE2\x8C\x83`" : "Ctrl+`");
    row("Find / replace", sc("F") + "  " + sc("F", false, true));
    ImGui::EndGroup();
}

void App::drawBottomPanel(float w, float h)
{
    const Theme& t = theme();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col4(t.panel));
    ImGui::BeginChild("##bottom", ImVec2(w, h), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    if (m_selectBottomTab >= 0)
    {
        m_bottomTab = BottomTab(m_selectBottomTab);
        m_selectBottomTab = -1;
    }
    if (m_bottomTab == TabTerminal && !m_shell.started()) startShell();

    // Tab strip (VS Code style: text with an underline for the active tab).
    ImGui::SetCursorPos(ImVec2(8, 4));
    int errors = 0, warnings = 0;
    for (const auto& p : m_problems) (p.error ? errors : warnings)++;
    struct TabDef { BottomTab tab; std::string label; };
    std::string problemsLabel = "PROBLEMS";
    if (errors + warnings > 0) problemsLabel += "  " + std::to_string(errors + warnings);
    const TabDef tabs[] = {
        { TabProblems, problemsLabel },
        { TabOutput, m_build.running() ? "BUILD \xE2\x80\xA6" : "BUILD" },
        { TabTerminal, "TERMINAL" },
        { TabRun, m_run.running() ? "RUN \xE2\x97\x8F" : "RUN" },
        { TabDebug, m_debug.running() ? "DEBUG \xE2\x97\x8F" : "DEBUG" },
    };
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (const auto& td : tabs)
    {
        bool active = m_bottomTab == td.tab;
        ImVec2 ts = ImGui::CalcTextSize(td.label.c_str());
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::PushID(int(td.tab));
        if (ImGui::InvisibleButton("##btab", ImVec2(ts.x + 18, ImGui::GetFrameHeight())))
        {
            m_bottomTab = td.tab;
            if (td.tab == TabTerminal) m_shell.focus();
            if (td.tab == TabRun) m_run.focus();
            if (td.tab == TabDebug) m_debug.focus();
        }
        bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        dl->AddText(ImVec2(p.x + 9, p.y + (ImGui::GetFrameHeight() - ts.y) * 0.5f),
                    active ? t.text : (hov ? t.text : t.textDim), td.label.c_str());
        if (td.tab == TabProblems && errors > 0)
            dl->AddCircleFilled(ImVec2(p.x + 4, p.y + ImGui::GetFrameHeight() * 0.5f), 2.5f, t.error);
        if (active)
            dl->AddLine(ImVec2(p.x + 9, p.y + ImGui::GetFrameHeight() - 1),
                        ImVec2(p.x + 9 + ts.x, p.y + ImGui::GetFrameHeight() - 1), t.accent, 2.0f);
        ImGui::SameLine(0, 2);
    }

    // Actions for the current tab, right aligned.
    float right = ImGui::GetWindowWidth() - 8;
    auto rightButton = [&](const char* label, const char* tip) {
        float bw = ImGui::CalcTextSize(label, nullptr, true).x + 14;
        right -= bw + 2;
        ImGui::SetCursorPos(ImVec2(right, 4));
        return flatButton(label, true, tip);
    };
    if (rightButton("\xC3\x97##hidebottom", ("Hide panel  " + sc("J")).c_str())) settings.showBottom = false;
    switch (m_bottomTab)
    {
    case TabOutput:
        if (rightButton("Clear##bo", "Clear build output")) m_buildLines.clear();
        if (m_build.running() && rightButton("Cancel##bo", "Cancel the build")) m_build.cancel();
        break;
    case TabProblems:
        if (!m_problems.empty() && rightButton("Ask clil to fix##bp", "Send the errors to clil")) askClilFixProblems();
        break;
    case TabTerminal:
        if (rightButton("Clear##bt", ("Clear  " + sc("K")).c_str())) m_shell.clear();
        if (rightButton("Restart##bt", "Start a new shell")) { m_shell.stop(); startShell(); m_shell.focus(); }
        break;
    case TabRun:
        if (m_run.running() && rightButton("Stop##br", "Stop the program")) m_run.stop();
        if (!m_run.running() && rightButton("Run again##br", "Run without building")) runProgram();
        break;
    case TabDebug:
        if (m_debug.running())
        {
            if (rightButton("Stop##bd", "Stop debugging")) m_debug.stop();
            if (rightButton("Out##bd", "Step out (finish)")) m_debug.sendText("finish\n", false);
            if (rightButton("Into##bd", "Step into  F11")) m_debug.sendText("step\n", false);
            if (rightButton("Over##bd", "Step over  F10")) m_debug.sendText("next\n", false);
            if (rightButton("Continue##bd", "Continue  F5")) m_debug.sendText("continue\n", false);
        }
        break;
    }

    ImGui::SetCursorPos(ImVec2(0, ImGui::GetFrameHeight() + 8));
    switch (m_bottomTab)
    {
    case TabProblems: drawProblems(); break;
    case TabOutput: drawOutput(); break;
    case TabTerminal: m_shell.font = monoFont; m_shell.render("##term_shell"); break;
    case TabRun:
        if (!m_run.started()) { ImGui::SetCursorPosX(12); ImGui::TextDisabled("Run the program with %s (build & run) or %s.", sc("R").c_str(), sc("R", true).c_str()); }
        else { m_run.font = monoFont; m_run.render("##term_run"); }
        break;
    case TabDebug:
        if (!m_debug.started())
        {
            ImGui::SetCursorPosX(12);
            ImGui::TextDisabled("Press F5 to build and start lldb. Click a line number to set a breakpoint.");
        }
        else { m_debug.font = monoFont; m_debug.render("##term_debug"); }
        break;
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void App::drawProblems()
{
    const Theme& t = theme();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 4));
    ImGui::BeginChild("##problems", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    if (m_problems.empty())
    {
        ImGui::TextDisabled(m_build.state() == BuildRunner::State::Succeeded ? "No problems. The last build succeeded."
                                                                             : "No problems.");
    }
    ImGuiListClipper clip;
    clip.Begin(int(m_problems.size()));
    while (clip.Step())
    {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i)
        {
            const Problem& p = m_problems[size_t(i)];
            ImGui::PushID(i);
            ImVec2 pos = ImGui::GetCursorScreenPos();
            float h = ImGui::GetTextLineHeight() + 4;
            if (ImGui::Selectable("##prob", false, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, h)))
                openFile(p.file, p.line, p.col);
            if (ImGui::BeginPopupContextItem())
            {
                if (ImGui::MenuItem("Go to")) openFile(p.file, p.line, p.col);
                if (ImGui::MenuItem("Ask clil to fix this"))
                    sendToClil("Fix this " + std::string(p.error ? "error" : "warning") + " in " + rel(p.file) + ":" +
                                   std::to_string(p.line) + ":\n" + p.message, true);
                if (ImGui::MenuItem("Copy")) ImGui::SetClipboardText((rel(p.file) + ":" + std::to_string(p.line) + ": " + p.message).c_str());
                ImGui::EndPopup();
            }
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddCircleFilled(ImVec2(pos.x + 6, pos.y + h * 0.5f), 4.0f, p.error ? t.error : t.warning);
            std::string loc = rel(p.file) + ":" + std::to_string(p.line) + (p.col ? ":" + std::to_string(p.col) : "");
            float x = pos.x + 18;
            dl->AddText(ImVec2(x, pos.y + 2), t.text, p.message.c_str());
            float mw = ImGui::CalcTextSize(p.message.c_str()).x;
            dl->AddText(ImVec2(x + mw + 12, pos.y + 2), t.textDim, loc.c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void App::drawOutput()
{
    const Theme& t = theme();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 4));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col4(t.termBg));
    ImGui::BeginChild("##output", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushFont(monoFont);
    bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4;
    ImGuiListClipper clip;
    clip.Begin(int(m_buildLines.size()));
    while (clip.Step())
    {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i)
        {
            const auto& l = m_buildLines[size_t(i)];
            ImU32 c = l.kind == 1 ? t.error : l.kind == 2 ? t.warning : l.kind == 3 ? t.accent : t.termFg;
            ImGui::PushStyleColor(ImGuiCol_Text, col4(c));
            ImGui::TextUnformatted(l.text.c_str());
            ImGui::PopStyleColor();
            if ((l.kind == 1 || l.kind == 2) && ImGui::IsItemHovered())
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (ImGui::IsMouseClicked(0))
                {
                    Problem p;
                    if (BuildRunner::parseDiagnostic(l.text, m_build.dir(), p)) openFile(p.file, p.line, p.col);
                }
            }
        }
    }
    if (m_buildScroll && atBottom) ImGui::SetScrollHereY(1.0f);
    m_buildScroll = false;
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

void App::drawAssistant(float w, float h)
{
    const Theme& t = theme();
    if (!m_clilStarted) startClil();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col4(t.panel));
    ImGui::BeginChild("##assistant", ImVec2(w, h), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    // Header: logo + state + actions.
    ImGui::SetCursorPos(ImVec2(10, 5));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));
    {
        // The clil logo: two arrows drawn as blocks, like assets/icon.svg.
        ImVec2 p = ImGui::GetCursorScreenPos();
        float s = ImGui::GetFrameHeight();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImU32 orange = IM_COL32(255, 135, 0, 255);
        float u = s / 6.0f;
        dl->AddRectFilled(ImVec2(p.x, p.y + u), ImVec2(p.x + u, p.y + 2 * u), orange);
        dl->AddRectFilled(ImVec2(p.x + u, p.y + 2 * u), ImVec2(p.x + 2 * u, p.y + 3 * u), orange);
        dl->AddRectFilled(ImVec2(p.x + u, p.y + 3 * u), ImVec2(p.x + 2 * u, p.y + 4 * u), orange);
        dl->AddRectFilled(ImVec2(p.x, p.y + 4 * u), ImVec2(p.x + u, p.y + 5 * u), orange);
        ImGui::Dummy(ImVec2(2.4f * u, s));
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("clil");
        ImGui::SameLine();
        if (!m_clilMissingReason.empty()) pill("not found", withAlpha(t.error, 50), t.error);
        else if (m_clil.running())
        {
            bool idle = m_clil.cursorLineText().rfind("\xE2\x80\xBA", 0) == 0;
            pill(idle ? "ready" : "working", withAlpha(idle ? t.ok : t.accent, 45), idle ? t.ok : t.accent);
        }
        else pill("stopped", withAlpha(t.textDim, 40), t.textDim);
    }
    float right = ImGui::GetWindowWidth() - 8;
    auto headerButton = [&](const char* label, const char* tip) {
        float bw = ImGui::CalcTextSize(label, nullptr, true).x + 14;
        right -= bw + 2;
        ImGui::SetCursorPos(ImVec2(right, 5));
        return flatButton(label, true, tip);
    };
    if (headerButton("\xC3\x97##hideclil", ("Hide  " + sc("I")).c_str())) settings.showRight = false;
    if (headerButton("Restart##clil", "Restart clil")) { m_clil.stop(); startClil(); m_clil.focus(); }
    if (headerButton("New chat##clil", "Start a new conversation (/clear)")) sendToClil("/clear", true);
    ImGui::PopStyleVar();

    // Quick actions.
    ImGui::SetCursorPos(ImVec2(10, ImGui::GetFrameHeight() + 10));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(9, 3));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 6));
    bool hasDoc = activeDoc() != nullptr;
    bool sel = hasDoc && activeDoc()->hasSelection();
    float lineStart = ImGui::GetCursorPosX();
    auto chip = [&](const char* label, bool enabled) {
        float bw = ImGui::CalcTextSize(label, nullptr, true).x + 18;
        if (ImGui::GetCursorPosX() > lineStart && ImGui::GetCursorPosX() + bw > ImGui::GetWindowWidth() - 10) ImGui::NewLine(), ImGui::SetCursorPosX(lineStart);
        ImGui::BeginDisabled(!enabled);
        bool r = ImGui::SmallButton(label);
        ImGui::EndDisabled();
        ImGui::SameLine();
        return r;
    };
    if (chip(sel ? "Explain selection" : "Explain file", hasDoc)) askClilAboutSelection("Explain");
    if (chip(sel ? "Find bugs in selection" : "Find bugs", hasDoc)) askClilAboutSelection("Find bugs in");
    if (chip("Fix build errors", !m_root.empty())) askClilFixProblems();
    if (chip("Write tests", hasDoc)) askClilAboutSelection("Write unit tests for");
    if (chip("Commit message", !m_root.empty())) sendToClil("Look at `git diff` and write a short commit message for these changes.", true);
    ImGui::NewLine();
    ImGui::PopStyleVar(3);

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2);
    if (!m_clilMissingReason.empty())
    {
        ImGui::SetCursorPosX(12);
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - 16);
        ImGui::TextUnformatted(m_clilMissingReason.c_str());
        ImGui::Spacing();
        ImGui::TextDisabled("SF-IDE builds clil from the bundled external/clil sources. Run "
                            "`git submodule update --init` and rebuild, install clil yourself "
                            "(make && sudo make install in the clil repo), or set its path in Settings.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::Button("Settings...")) m_openSettings = true;
        ImGui::SameLine();
        if (ImGui::Button("Try again")) startClil();
        ImGui::EndGroup();
    }
    else
    {
        m_clil.font = monoFont;
        m_clil.render("##term_clil");
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void App::drawStatusBar(float w, float h)
{
    const Theme& t = theme();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col4(t.panelAlt));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 2));
    ImGui::BeginChild("##status", ImVec2(w, h), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
    ImGui::SetCursorPosY((h - ImGui::GetFrameHeight()) * 0.5f);
    ImGui::AlignTextToFramePadding();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    if (!m_gitBranch.empty())
    {
        ImGui::TextDisabled("\xE2\x8E\x87 %s", m_gitBranch.c_str());
        ImGui::SameLine(0, 16);
    }
    int errors = 0, warnings = 0;
    for (const auto& p : m_problems) (p.error ? errors : warnings)++;
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        char buf[64];
        std::snprintf(buf, sizeof buf, "   %d    %d", errors, warnings);
        ImVec2 ts = ImGui::CalcTextSize(buf);
        if (ImGui::InvisibleButton("##sb_problems", ImVec2(ts.x + 6, ImGui::GetFrameHeight())))
        {
            settings.showBottom = true;
            m_selectBottomTab = TabProblems;
        }
        float cy = p.y + ImGui::GetFrameHeight() * 0.5f;
        dl->AddCircleFilled(ImVec2(p.x + 5, cy), 4, errors ? t.error : t.textDim);
        std::string e = std::to_string(errors), wn = std::to_string(warnings);
        dl->AddText(ImVec2(p.x + 13, cy - ts.y * 0.5f), t.textDim, e.c_str());
        float x2 = p.x + 13 + ImGui::CalcTextSize(e.c_str()).x + 10;
        dl->AddTriangleFilled(ImVec2(x2, cy + 4), ImVec2(x2 + 8, cy + 4), ImVec2(x2 + 4, cy - 4), warnings ? t.warning : t.textDim);
        dl->AddText(ImVec2(x2 + 12, cy - ts.y * 0.5f), t.textDim, wn.c_str());
        ImGui::SameLine(0, 16);
    }
    switch (m_build.state())
    {
    case BuildRunner::State::Running: ImGui::Text("Building\xE2\x80\xA6 %.0fs", m_build.elapsed()); break;
    case BuildRunner::State::Succeeded: ImGui::TextColored(col4(t.ok), "Build succeeded"); break;
    case BuildRunner::State::Failed: ImGui::TextColored(col4(t.error), "Build failed"); break;
    case BuildRunner::State::Cancelled: ImGui::TextDisabled("Build cancelled"); break;
    default: break;
    }
    if (!m_status.empty() && now() < m_statusUntil)
    {
        ImGui::SameLine(0, 16);
        ImGui::TextUnformatted(m_status.c_str());
    }

    // Right side: cursor position and file info.
    std::string info;
    if (Document* d = activeDoc())
    {
        int line = d->lineOf(d->cursor);
        int col = d->columnOf(line, d->cursor) + 1;
        char buf[160];
        const char* lang = d->lang() == Document::Lang::CFamily ? "C/C++" : d->lang() == Document::Lang::Hash ? "Script"
                         : d->lang() == Document::Lang::Markdown ? "Markdown" : "Plain Text";
        if (d->hasSelection())
            std::snprintf(buf, sizeof buf, "Ln %d, Col %d (%zu selected)     UTF-8     %s", line + 1, col,
                          d->selMax() - d->selMin(), lang);
        else
            std::snprintf(buf, sizeof buf, "Ln %d, Col %d     UTF-8     %s", line + 1, col, lang);
        info = buf;
    }
    std::string clilState = !m_clilMissingReason.empty() ? "clil: not found" : m_clil.running() ? "clil: on" : "clil: off";
    info += (info.empty() ? "" : "     ") + clilState;
    float iw = ImGui::CalcTextSize(info.c_str()).x;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 20, w - iw - 14));
    ImGui::TextDisabled("%s", info.c_str());
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

// --- Popups -------------------------------------------------------------------------------------

void App::drawQuickOpen()
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + 60), ImGuiCond_Always, ImVec2(0.5f, 0));
    ImGui::SetNextWindowSize(ImVec2(std::min(640.0f, vp->Size.x - 40), 0));
    if (!ImGui::BeginPopup("Quick Open")) return;
    if (ImGui::IsWindowAppearing())
    {
        m_quickFiles = m_tree.allFiles();
        m_quickBuf[0] = 0;
        m_quickSel = 0;
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##quick", "Search files by name", m_quickBuf, sizeof m_quickBuf)) m_quickSel = 0;
    if (ImGui::IsItemDeactivated() && ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();

    struct Hit { int score; const std::string* path; };
    std::vector<Hit> hits;
    std::string needle = m_quickBuf;
    for (const auto& f : m_quickFiles)
    {
        std::string r = rel(f);
        int s = fuzzyScore(r, needle);
        if (s > 0) hits.push_back({ s, &f });
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.score > b.score; });
    if (hits.size() > 60) hits.resize(60);
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) m_quickSel = std::min(m_quickSel + 1, int(hits.size()) - 1);
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) m_quickSel = std::max(m_quickSel - 1, 0);
    bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);

    ImGui::BeginChild("##quicklist", ImVec2(0, std::min(14, int(hits.size())) * (ImGui::GetTextLineHeightWithSpacing() + 2) + 4));
    for (int i = 0; i < int(hits.size()); ++i)
    {
        std::string r = rel(*hits[size_t(i)].path);
        std::string name = fs::path(r).filename().string();
        bool selected = i == m_quickSel;
        ImGui::PushID(i);
        if (ImGui::Selectable("##q", selected) || (enter && selected))
        {
            openFile(*hits[size_t(i)].path);
            ImGui::CloseCurrentPopup();
        }
        if (selected) ImGui::SetScrollHereY();
        ImGui::SameLine(8);
        ImGui::TextUnformatted(name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", r.c_str());
        ImGui::PopID();
    }
    if (hits.empty()) ImGui::TextDisabled("No matching files");
    ImGui::EndChild();
    ImGui::EndPopup();
}

void App::drawSettings()
{
    ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Settings", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) return;
    if (ImGui::IsWindowAppearing())
    {
        std::snprintf(m_setClilPath, sizeof m_setClilPath, "%s", settings.clilPath.c_str());
        std::snprintf(m_setClilArgs, sizeof m_setClilArgs, "%s", settings.clilArgs.c_str());
        std::snprintf(m_setHost, sizeof m_setHost, "%s", settings.lmHost.c_str());
        std::snprintf(m_setBuild, sizeof m_setBuild, "%s", m_buildCmd.c_str());
        std::snprintf(m_setRun, sizeof m_setRun, "%s", m_runCmd.c_str());
        std::snprintf(m_setArgs, sizeof m_setArgs, "%s", m_runArgs.c_str());
    }
    const float labelW = 150;
    auto label = [&](const char* l) { ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(l); ImGui::SameLine(labelW); ImGui::SetNextItemWidth(-1); };

    ImGui::SeparatorText("Appearance");
    label("Theme");
    int themeIdx = settings.dark ? 0 : 1;
    if (ImGui::Combo("##theme", &themeIdx, "Dark\0Light\0")) { settings.dark = themeIdx == 0; setTheme(settings.dark); }
    label("Editor font size");
    if (ImGui::SliderFloat("##efs", &settings.editorFontSize, 9, 32, "%.0f")) fontsDirty = true;
    label("Interface font size");
    if (ImGui::SliderFloat("##ufs", &settings.uiFontSize, 10, 22, "%.1f")) fontsDirty = true;
    label("Save before build");
    ImGui::Checkbox("##sbb", &settings.saveBeforeBuild);

    ImGui::SeparatorText("clil");
    label("clil path");
    ImGui::InputTextWithHint("##clilpath", findClil().empty() ? "not found - enter a path" : findClil().c_str(), m_setClilPath, sizeof m_setClilPath);
    label("Extra arguments");
    ImGui::InputTextWithHint("##clilargs", "e.g. -m qwen3-coder -c 32768", m_setClilArgs, sizeof m_setClilArgs);
    label("LM Studio server");
    ImGui::InputTextWithHint("##host", "127.0.0.1:1234", m_setHost, sizeof m_setHost);

    if (!m_root.empty())
    {
        ImGui::SeparatorText(("Project: " + fs::path(m_root).filename().string() + "  (saved in .sfide)").c_str());
        label("Build command");
        ImGui::InputTextWithHint("##build", m_cmds.build.empty() ? "none detected" : m_cmds.build.c_str(), m_setBuild, sizeof m_setBuild);
        label("Run command");
        ImGui::InputTextWithHint("##run", m_lastExe.empty() ? "newest executable after the build" : m_lastExe.c_str(), m_setRun, sizeof m_setRun);
        label("Program arguments");
        ImGui::InputText("##args", m_setArgs, sizeof m_setArgs);
        if (!m_cmds.kind.empty()) ImGui::TextDisabled("Detected a %s project. Leave fields empty to use the defaults.", m_cmds.kind.c_str());
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    float bw = 100;
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - bw * 2 - 24);
    if (ImGui::Button("Cancel", ImVec2(bw, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::SameLine();
    if (ImGui::Button("Save", ImVec2(bw, 0)))
    {
        bool clilChanged = settings.clilPath != m_setClilPath || settings.clilArgs != m_setClilArgs || settings.lmHost != m_setHost;
        settings.clilPath = trim(m_setClilPath);
        settings.clilArgs = trim(m_setClilArgs);
        settings.lmHost = trim(m_setHost);
        if (!m_root.empty())
        {
            m_buildCmd = trim(m_setBuild);
            m_runCmd = trim(m_setRun);
            m_runArgs = trim(m_setArgs);
            saveProjectConfig();
        }
        saveSettings();
        if (clilChanged && m_clilStarted) { m_clil.stop(); startClil(); }
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void App::drawPopups()
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 center(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.4f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 8));

    if (m_openQuick) { ImGui::OpenPopup("Quick Open"); m_openQuick = false; }
    drawQuickOpen();

    if (m_openSettings) { ImGui::OpenPopup("Settings"); m_openSettings = false; }
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    drawSettings();

    if (m_openGoto) { ImGui::OpenPopup("Go to Line"); m_openGoto = false; m_gotoBuf[0] = 0; }
    ImGui::SetNextWindowPos(ImVec2(center.x, vp->Pos.y + 70), ImGuiCond_Appearing, ImVec2(0.5f, 0));
    if (ImGui::BeginPopup("Go to Line"))
    {
        Document* d = activeDoc();
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(260);
        char hint[64];
        std::snprintf(hint, sizeof hint, "Line (1-%d), optionally :column", d ? d->lineCount() : 1);
        if (ImGui::InputTextWithHint("##goto", hint, m_gotoBuf, sizeof m_gotoBuf, ImGuiInputTextFlags_EnterReturnsTrue))
        {
            int line = 0, col = 1;
            std::sscanf(m_gotoBuf, "%d:%d", &line, &col);
            if (d && line > 0) m_editor.gotoLine(*d, line, col);
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    auto modal = [&](const char* name) {
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        return ImGui::BeginPopupModal(name, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove);
    };

    if (m_openCloseConfirm) { ImGui::OpenPopup("Unsaved Changes"); m_openCloseConfirm = false; }
    if (modal("Unsaved Changes"))
    {
        Document* d = (m_pendingCloseIndex >= 0 && m_pendingCloseIndex < int(m_docs.size())) ? m_docs[size_t(m_pendingCloseIndex)].get() : nullptr;
        ImGui::Text("Save changes to %s before closing?", d ? d->displayName().c_str() : "this file");
        ImGui::Spacing();
        if (ImGui::Button("Save", ImVec2(110, 0)))
        {
            if (d && saveDoc(*d)) closeDoc(m_pendingCloseIndex, true);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Don't Save", ImVec2(110, 0))) { closeDoc(m_pendingCloseIndex, true); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (m_openQuitConfirm) { ImGui::OpenPopup("Quit SF-IDE"); m_openQuitConfirm = false; }
    if (modal("Quit SF-IDE"))
    {
        int n = int(std::count_if(m_docs.begin(), m_docs.end(), [](const auto& d) { return d->dirty(); }));
        ImGui::Text("%d file%s %s unsaved changes.", n, n == 1 ? "" : "s", n == 1 ? "has" : "have");
        ImGui::Spacing();
        if (ImGui::Button("Save All & Quit", ImVec2(140, 0)))
        {
            saveAll();
            if (std::none_of(m_docs.begin(), m_docs.end(), [](const auto& d) { return d->dirty(); })) quitConfirmed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Quit Without Saving", ImVec2(160, 0))) { quitConfirmed = true; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(90, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (m_openPrompt) { ImGui::OpenPopup("##prompt"); m_openPrompt = false; }
    if (modal("##prompt"))
    {
        const char* title = "";
        switch (m_prompt)
        {
        case PromptNewFile: title = "New file name"; break;
        case PromptNewFolder: title = "New folder name"; break;
        case PromptRename: title = "Rename to"; break;
        case PromptOpenFolder: title = "Folder to open (full path)"; break;
        case PromptOpenFile: title = "File to open (full path)"; break;
        case PromptSaveAs: title = "Save as (full path)"; break;
        default: break;
        }
        ImGui::TextUnformatted(title);
        if (m_prompt == PromptNewFile || m_prompt == PromptNewFolder) ImGui::TextDisabled("in %s", rel(m_promptBase).c_str());
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(420);
        bool enter = ImGui::InputText("##promptinput", m_promptBuf, sizeof m_promptBuf, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::Spacing();
        bool ok = ImGui::Button("OK", ImVec2(100, 0)) || enter;
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) { m_prompt = PromptNone; ImGui::CloseCurrentPopup(); }
        std::string v = trim(m_promptBuf);
        if (ok && !v.empty())
        {
            std::error_code ec;
            switch (m_prompt)
            {
            case PromptNewFile: {
                fs::path p = fs::path(m_promptBase) / v;
                fs::create_directories(p.parent_path(), ec);
                if (!fs::exists(p)) writeTextFile(p.string(), "");
                m_tree.refresh();
                openFile(p.string());
                break;
            }
            case PromptNewFolder: fs::create_directories(fs::path(m_promptBase) / v, ec); m_tree.refresh(); break;
            case PromptRename: {
                fs::path to = fs::path(m_promptBase).parent_path() / v;
                fs::rename(m_promptBase, to, ec);
                if (ec) showMessage("Rename", ec.message());
                else
                    for (auto& d : m_docs)
                        if (d->path() == m_promptBase) d->saveAs(to.string());
                m_tree.refresh();
                break;
            }
            case PromptOpenFolder: openFolder(v); break;
            case PromptOpenFile: openFile(v); break;
            case PromptSaveAs:
                if (m_saveAsDoc && std::any_of(m_docs.begin(), m_docs.end(), [&](const auto& d) { return d.get() == m_saveAsDoc; }))
                {
                    if (!m_saveAsDoc->saveAs(v)) showMessage("Save As", "Cannot write " + v);
                    m_tree.refresh();
                }
                break;
            default: break;
            }
            m_prompt = PromptNone;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (m_openDelete) { ImGui::OpenPopup("Delete"); m_openDelete = false; }
    if (modal("Delete"))
    {
        ImGui::Text("Delete %s?", rel(m_deletePath).c_str());
        ImGui::TextDisabled(isMac() ? "It is moved to the Trash." : "This cannot be undone.");
        ImGui::Spacing();
        if (ImGui::Button("Delete", ImVec2(100, 0)))
        {
            std::error_code ec;
#if defined(__APPLE__)
            int rc = 0;
            runCapture("osascript -e " + shellQuote("tell application \"Finder\" to delete POSIX file " + appleScriptString(m_deletePath)) + " >/dev/null 2>&1", &rc);
            if (rc != 0) fs::remove_all(m_deletePath, ec);
#else
            fs::remove_all(m_deletePath, ec);
#endif
            for (int i = int(m_docs.size()) - 1; i >= 0; --i)
                if (m_docs[size_t(i)]->path() == m_deletePath || m_docs[size_t(i)]->path().rfind(m_deletePath + "/", 0) == 0)
                    closeDoc(i, true);
            m_tree.refresh();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (m_openMessage) { ImGui::OpenPopup("##message"); m_openMessage = false; }
    if (modal("##message"))
    {
        ImGui::TextUnformatted(m_messageTitle.c_str());
        ImGui::Spacing();
        ImGui::PushTextWrapPos(420);
        ImGui::TextDisabled("%s", m_messageText.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::Button("OK", ImVec2(100, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (m_openAbout) { ImGui::OpenPopup("About SF-IDE"); m_openAbout = false; }
    if (modal("About SF-IDE"))
    {
        ImGui::TextUnformatted("SF-IDE 2.0");
        ImGui::TextDisabled("A small native C/C++ IDE for macOS, with the clil coding agent built in.");
        ImGui::TextDisabled("Dear ImGui %s  \xC2\xB7  GLFW  \xC2\xB7  OpenGL", IMGUI_VERSION);
        ImGui::Spacing();
        if (ImGui::Button("OK", ImVec2(100, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}
