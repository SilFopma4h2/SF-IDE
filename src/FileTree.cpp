#include "FileTree.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sys/stat.h>

#include <imgui.h>

#include "Theme.h"

namespace fs = std::filesystem;

namespace {

int64_t dirMtime(const std::string& path)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return -1;
#if defined(__APPLE__)
    return int64_t(st.st_mtimespec.tv_sec) * 1000000000 + st.st_mtimespec.tv_nsec;
#else
    return int64_t(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec;
#endif
}

// Colour dot per file type, so the tree is scannable without an icon font.
ImU32 fileColor(const std::string& name)
{
    const Theme& t = theme();
    std::string ext;
    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) ext = name.substr(dot + 1);
    if (ext == "c" || ext == "cc" || ext == "cpp" || ext == "cxx" || ext == "mm" || ext == "m") return t.synType;
    if (ext == "h" || ext == "hh" || ext == "hpp" || ext == "hxx" || ext == "inl") return t.synKeyword;
    if (ext == "md" || ext == "txt") return t.textDim;
    if (ext == "json" || ext == "yml" || ext == "yaml" || ext == "toml") return t.synNumber;
    if (name == "CMakeLists.txt" || name == "Makefile" || ext == "cmake" || ext == "sh") return t.synPreproc;
    return t.synComment;
}

} // namespace

bool isHiddenName(const std::string& name)
{
    return name == ".git" || name == ".DS_Store" || name == ".cache" || name == ".idea" || name == ".vs";
}

void FileTree::setRoot(const std::string& root)
{
    m_root = root;
    m_rootNode.reset();
    if (root.empty()) return;
    m_rootNode = std::make_unique<Node>();
    m_rootNode->path = root;
    m_rootNode->name = fs::path(root).filename().string();
    m_rootNode->isDir = true;
    m_rootNode->expanded = true;
    load(*m_rootNode);
}

void FileTree::load(Node& n)
{
    std::vector<std::unique_ptr<Node>> old;
    old.swap(n.children);
    std::error_code ec;
    for (auto it = fs::directory_iterator(n.path, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec))
    {
        std::string name = it->path().filename().string();
        if (isHiddenName(name)) continue;
        auto c = std::make_unique<Node>();
        c->name = name;
        c->path = it->path().string();
        c->isDir = it->is_directory(ec);
        // Keep expansion state of directories that still exist.
        for (auto& o : old)
        {
            if (o && o->path == c->path && o->isDir && c->isDir)
            {
                c = std::move(o);
                break;
            }
        }
        n.children.push_back(std::move(c));
    }
    std::sort(n.children.begin(), n.children.end(), [](const auto& a, const auto& b) {
        if (a->isDir != b->isDir) return a->isDir;
        // Case-insensitive, like Finder.
        return std::lexicographical_compare(a->name.begin(), a->name.end(), b->name.begin(), b->name.end(),
                                            [](char x, char y) { return std::tolower((unsigned char)x) < std::tolower((unsigned char)y); });
    });
    n.loaded = true;
    n.mtime = dirMtime(n.path);
}

void FileTree::refreshNode(Node& n)
{
    if (!n.isDir || !n.expanded || !n.loaded) return;
    if (dirMtime(n.path) != n.mtime) load(n);
    for (auto& c : n.children) refreshNode(*c);
}

void FileTree::refresh()
{
    if (m_rootNode) refreshNode(*m_rootNode);
}

void FileTree::reveal(const std::string& path)
{
    m_revealPath = path;
}

FileTree::Action FileTree::render(const std::string& activePath)
{
    Action action;
    if (!m_rootNode)
    {
        ImGui::Spacing();
        ImGui::TextDisabled("No folder open");
        return action;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 2));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 3));
    ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, 12);
    for (auto& c : m_rootNode->children) renderNode(*c, activePath, action);

    // Context menu on empty space: new file / folder in the root.
    if (ImGui::BeginPopupContextWindow("##tree_bg", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
    {
        if (ImGui::MenuItem("New File...")) action = { Action::NewFile, m_root, true };
        if (ImGui::MenuItem("New Folder...")) action = { Action::NewFolder, m_root, true };
        ImGui::Separator();
        if (ImGui::MenuItem("Open in Terminal")) action = { Action::OpenTerminal, m_root, true };
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(3);
    m_revealPath.clear();
    return action;
}

void FileTree::renderNode(Node& n, const std::string& activePath, Action& action)
{
    const Theme& t = theme();
    ImGui::PushID(n.path.c_str());
    if (n.isDir)
    {
        if (!m_revealPath.empty() && m_revealPath.rfind(n.path + "/", 0) == 0) n.expanded = true;
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                   ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_FramePadding |
                                   ImGuiTreeNodeFlags_NoTreePushOnOpen;
        ImGui::SetNextItemOpen(n.expanded);
        bool dim = n.name == "build" || n.name.rfind("cmake-build", 0) == 0 || n.name == "node_modules" ||
                   (!n.name.empty() && n.name[0] == '.');
        if (dim) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(t.textDim));
        bool open = ImGui::TreeNodeEx("##dir", flags, "%s", n.name.c_str());
        if (dim) ImGui::PopStyleColor();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) open = !open;
        if (open != n.expanded)
        {
            n.expanded = open;
            if (open && !n.loaded) load(n);
        }
        if (ImGui::BeginPopupContextItem())
        {
            if (ImGui::MenuItem("New File...")) action = { Action::NewFile, n.path, true };
            if (ImGui::MenuItem("New Folder...")) action = { Action::NewFolder, n.path, true };
            ImGui::Separator();
            if (ImGui::MenuItem("Ask clil about this folder")) action = { Action::AskClil, n.path, true };
            if (ImGui::MenuItem("Open in Terminal")) action = { Action::OpenTerminal, n.path, true };
#if defined(__APPLE__)
            if (ImGui::MenuItem("Reveal in Finder")) action = { Action::Reveal, n.path, true };
#endif
            if (ImGui::MenuItem("Copy Path")) action = { Action::CopyPath, n.path, true };
            ImGui::Separator();
            if (ImGui::MenuItem("Rename...")) action = { Action::Rename, n.path, true };
            if (ImGui::MenuItem("Delete...")) action = { Action::Delete, n.path, true };
            ImGui::EndPopup();
        }
        if (n.expanded)
        {
            if (!n.loaded) load(n);
            ImGui::Indent();
            for (auto& c : n.children) renderNode(*c, activePath, action);
            ImGui::Unindent();
        }
    }
    else
    {
        bool active = n.path == activePath;
        if (active && !m_revealPath.empty() && m_revealPath == n.path) ImGui::SetScrollHereY(0.4f);
        ImVec2 p = ImGui::GetCursorScreenPos();
        float h = ImGui::GetFrameHeight();
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0, 0.5f));
        // Leave room for the arrow column so files line up with folder names.
        float arrowW = ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.x * 2;
        if (ImGui::Selectable("##file", active, ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_SpanAllColumns,
                              ImVec2(0, h)))
            action = { Action::Open, n.path, false };
        ImGui::PopStyleVar();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float cx = p.x + arrowW * 0.5f;
        dl->AddCircleFilled(ImVec2(cx, p.y + h * 0.5f), 3.2f, fileColor(n.name));
        dl->AddText(ImVec2(p.x + arrowW, p.y + (h - ImGui::GetFontSize()) * 0.5f), t.text,
                    n.name.c_str());
        if (ImGui::BeginPopupContextItem())
        {
            if (ImGui::MenuItem("Open")) action = { Action::Open, n.path, false };
            if (ImGui::MenuItem("Ask clil about this file")) action = { Action::AskClil, n.path, false };
            ImGui::Separator();
#if defined(__APPLE__)
            if (ImGui::MenuItem("Reveal in Finder")) action = { Action::Reveal, n.path, false };
#endif
            if (ImGui::MenuItem("Copy Path")) action = { Action::CopyPath, n.path, false };
            ImGui::Separator();
            if (ImGui::MenuItem("Rename...")) action = { Action::Rename, n.path, false };
            if (ImGui::MenuItem("Delete...")) action = { Action::Delete, n.path, false };
            ImGui::EndPopup();
        }
    }
    ImGui::PopID();
}

std::vector<std::string> FileTree::allFiles(size_t limit) const
{
    std::vector<std::string> out;
    if (m_root.empty()) return out;
    std::error_code ec;
    auto it = fs::recursive_directory_iterator(m_root, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator() && out.size() < limit; it.increment(ec))
    {
        std::string name = it->path().filename().string();
        if (it->is_directory(ec))
        {
            if (isHiddenName(name) || name == "build" || name == "node_modules" || name.rfind("cmake-build", 0) == 0 ||
                name == "CMakeFiles" || name == "_deps" || (name.size() > 1 && name[0] == '.'))
                it.disable_recursion_pending();
            continue;
        }
        if (isHiddenName(name)) continue;
        out.push_back(it->path().string());
    }
    std::sort(out.begin(), out.end());
    return out;
}
