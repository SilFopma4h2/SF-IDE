// FileTree - project explorer with cached directory listings.
//
// Directories are listed once when expanded and re-listed only when their
// modification time changes, so drawing the tree never touches the disk.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class FileTree {
public:
    struct Action {
        enum Kind { None, Open, AskClil, Reveal, NewFile, NewFolder, Rename, Delete, CopyPath, OpenTerminal } kind = None;
        std::string path;
        bool isDir = false;
    };

    void setRoot(const std::string& root);
    const std::string& root() const { return m_root; }

    // Re-list expanded directories whose mtime changed. Cheap; call every second or so.
    void refresh();

    // Draws the tree; returns what the user asked for this frame.
    Action render(const std::string& activePath);

    // All files below the root (for quick open), up to `limit`.
    std::vector<std::string> allFiles(size_t limit = 20000) const;

    void reveal(const std::string& path);   // expand the parents of `path`

private:
    struct Node {
        std::string name, path;
        bool isDir = false;
        bool expanded = false;
        bool loaded = false;
        int64_t mtime = 0;
        std::vector<std::unique_ptr<Node>> children;
    };

    void load(Node& n);
    void refreshNode(Node& n);
    void renderNode(Node& n, const std::string& activePath, Action& action);

    std::unique_ptr<Node> m_rootNode;
    std::string m_root;
    std::string m_revealPath;
};

bool isHiddenName(const std::string& name);
