#include "Document.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <sys/stat.h>

namespace {

double nowSeconds()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool statFile(const std::string& path, int64_t& mtime, int64_t& size)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
#if defined(__APPLE__)
    mtime = int64_t(st.st_mtimespec.tv_sec) * 1000000000 + st.st_mtimespec.tv_nsec;
#else
    mtime = int64_t(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec;
#endif
    size = int64_t(st.st_size);
    return true;
}

bool readFile(const std::string& path, std::string& out)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return true;
}

bool isWordChar(unsigned char c)
{
    return c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80;
}

} // namespace

// --- File I/O ---------------------------------------------------------------

bool Document::load(const std::string& path)
{
    std::string raw;
    if (!readFile(path, raw)) return false;

    // Normalise CRLF / CR to LF but remember the style for saving.
    m_crlf = raw.find("\r\n") != std::string::npos;
    m_text.clear();
    m_text.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i)
    {
        char c = raw[i];
        if (c == '\r')
        {
            m_text.push_back('\n');
            if (i + 1 < raw.size() && raw[i + 1] == '\n') ++i;
        }
        else
        {
            m_text.push_back(c);
        }
    }
    m_path = path;
    statFile(m_path, m_mtime, m_size);
    cursor = anchor = 0;
    preferredCol = -1;
    m_dirty = false;
    m_savedHash = std::hash<std::string>{}(m_text);
    m_undo.clear();
    m_redo.clear();
    m_lastKind = EditKind::Other;
    breakpoints.clear();
    diagnostics.clear();
    detectLang();
    rebuildLines();
    return true;
}

bool Document::saveAs(const std::string& path)
{
    std::string out;
    if (m_crlf)
    {
        out.reserve(m_text.size() + m_lineStarts.size());
        for (char c : m_text)
        {
            if (c == '\n') out += "\r\n";
            else out.push_back(c);
        }
    }
    // Write to a temporary file and rename so a crash never leaves half a file.
    std::string tmp = path + ".sfide-tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const std::string& data = m_crlf ? out : m_text;
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = (std::fclose(f) == 0) && ok;
    if (ok)
    {
        // Keep the permissions of the file we replace (e.g. executable scripts).
        struct stat st;
        if (stat(path.c_str(), &st) == 0) chmod(tmp.c_str(), st.st_mode & 07777);
        ok = std::rename(tmp.c_str(), path.c_str()) == 0;
    }
    if (!ok)
    {
        std::remove(tmp.c_str());
        return false;
    }
    bool renamed = m_path != path;
    m_path = path;
    if (renamed) detectLang();
    statFile(m_path, m_mtime, m_size);
    m_dirty = false;
    m_savedHash = std::hash<std::string>{}(m_text);
    breakUndoGroup();
    return true;
}

bool Document::save()
{
    if (m_path.empty()) return false;
    return saveAs(m_path);
}

bool Document::changedOnDisk() const
{
    if (m_path.empty()) return false;
    int64_t mtime = 0, size = 0;
    if (!statFile(m_path, mtime, size)) return false;
    return mtime != m_mtime || size != m_size;
}

void Document::markDiskStateSeen()
{
    statFile(m_path, m_mtime, m_size);
}

bool Document::reloadFromDisk()
{
    // Keep the cursor on the same line / column: the change is usually
    // somewhere else in the file.
    int line = lineOf(cursor);
    int col = columnOf(line, cursor);
    auto keepBps = breakpoints;
    std::string path = m_path;
    // Reloading is undoable: an external tool (clil, git) may have changed
    // the file and the user may want the old version back.
    Snapshot before{ m_text, cursor, anchor };
    if (!load(path)) return false;
    m_undo.push_back(std::move(before));
    breakpoints = keepBps;
    cursor = anchor = offsetAtColumn(std::min(line, lineCount() - 1), col);
    ++m_version;
    return true;
}

std::string Document::displayName() const
{
    if (m_path.empty()) return "untitled";
    size_t p = m_path.find_last_of('/');
    return p == std::string::npos ? m_path : m_path.substr(p + 1);
}

void Document::detectLang()
{
    std::string name = displayName();
    std::string ext;
    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) ext = name.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });

    static const char* cfam[] = { "c", "cc", "cpp", "cxx", "c++", "h", "hh", "hpp", "hxx", "inl", "ipp",
                                  "m", "mm", "metal", "java", "js", "ts", "cs", "swift", "go", "rs", "glsl",
                                  "vert", "frag", "json", "zig" };
    static const char* hash[] = { "cmake", "sh", "bash", "zsh", "py", "rb", "pl", "yml", "yaml", "toml",
                                  "mk", "conf", "ini", "txt" };
    m_lang = Lang::Plain;
    for (auto* e : cfam) if (ext == e) m_lang = Lang::CFamily;
    for (auto* e : hash) if (ext == e) m_lang = Lang::Hash;
    if (ext == "md" || ext == "markdown") m_lang = Lang::Markdown;
    if (name == "Makefile" || name == "makefile" || name == "GNUmakefile" || name == "CMakeLists.txt" ||
        name == "Dockerfile" || name == ".gitignore")
        m_lang = Lang::Hash;
}

// --- Line index ---------------------------------------------------------------

void Document::rebuildLines()
{
    m_lineStarts.clear();
    m_lineStarts.push_back(0);
    const char* d = m_text.data();
    const size_t n = m_text.size();
    for (size_t i = 0; i < n; ++i)
        if (d[i] == '\n') m_lineStarts.push_back(i + 1);
    ++m_version;
}

size_t Document::lineStart(int line) const
{
    if (line <= 0) return 0;
    if (line >= lineCount()) return m_text.size();
    return m_lineStarts[size_t(line)];
}

size_t Document::lineEnd(int line) const
{
    if (line + 1 >= lineCount()) return m_text.size();
    return m_lineStarts[size_t(line) + 1] - 1;
}

std::string_view Document::lineText(int line) const
{
    size_t b = lineStart(line), e = lineEnd(line);
    return std::string_view(m_text.data() + b, e - b);
}

int Document::lineOf(size_t offset) const
{
    auto it = std::upper_bound(m_lineStarts.begin(), m_lineStarts.end(), offset);
    return int(it - m_lineStarts.begin()) - 1;
}

std::string Document::selectedText() const
{
    return m_text.substr(selMin(), selMax() - selMin());
}

// --- Cursor -----------------------------------------------------------------------

void Document::setCursor(size_t pos, bool extend)
{
    cursor = std::min(pos, m_text.size());
    if (!extend) anchor = cursor;
    breakUndoGroup();
}

void Document::selectAll()
{
    anchor = 0;
    cursor = m_text.size();
}

size_t Document::nextChar(size_t pos) const
{
    if (pos >= m_text.size()) return m_text.size();
    ++pos;
    while (pos < m_text.size() && (static_cast<unsigned char>(m_text[pos]) & 0xC0) == 0x80) ++pos;
    return pos;
}

size_t Document::prevChar(size_t pos) const
{
    if (pos == 0) return 0;
    --pos;
    while (pos > 0 && (static_cast<unsigned char>(m_text[pos]) & 0xC0) == 0x80) --pos;
    return pos;
}

size_t Document::wordLeft(size_t pos) const
{
    if (pos > 0 && m_text[pos - 1] == '\n') return pos - 1;
    while (pos > 0 && !isWordChar(m_text[pos - 1]) && m_text[pos - 1] != '\n') --pos;
    while (pos > 0 && isWordChar(m_text[pos - 1])) --pos;
    return pos;
}

size_t Document::wordRight(size_t pos) const
{
    const size_t n = m_text.size();
    if (pos < n && m_text[pos] == '\n') return pos + 1;
    while (pos < n && !isWordChar(m_text[pos]) && m_text[pos] != '\n') ++pos;
    while (pos < n && isWordChar(m_text[pos])) ++pos;
    return pos;
}

void Document::wordAt(size_t pos, size_t& b, size_t& e) const
{
    b = e = std::min(pos, m_text.size());
    if (b < m_text.size() && !isWordChar(m_text[b]) && (b == 0 || !isWordChar(m_text[b - 1])))
    {
        e = nextChar(b);
        return;
    }
    while (b > 0 && isWordChar(m_text[b - 1])) --b;
    while (e < m_text.size() && isWordChar(m_text[e])) ++e;
}

int Document::columnOf(int line, size_t offset) const
{
    size_t b = lineStart(line);
    size_t e = std::min(offset, lineEnd(line));
    int col = 0;
    for (size_t i = b; i < e; ++i)
    {
        unsigned char c = static_cast<unsigned char>(m_text[i]);
        if (c == '\t') col = (col / kTabWidth + 1) * kTabWidth;
        else if ((c & 0xC0) != 0x80) ++col;
    }
    return col;
}

size_t Document::offsetAtColumn(int line, int col) const
{
    size_t i = lineStart(line);
    const size_t e = lineEnd(line);
    int c = 0;
    while (i < e && c < col)
    {
        unsigned char ch = static_cast<unsigned char>(m_text[i]);
        int next = (ch == '\t') ? (c / kTabWidth + 1) * kTabWidth : c + 1;
        if (col < next)  // inside a tab: snap to the nearer edge
            return (col - c) <= (next - col) ? i : nextChar(i);
        c = next;
        i = nextChar(i);
    }
    return i;
}

// --- Editing ------------------------------------------------------------------------

void Document::pushUndo(EditKind kind)
{
    double t = nowSeconds();
    bool group = kind != EditKind::Other && kind == m_lastKind && (t - m_lastEditTime) < 1.0;
    if (!group)
    {
        m_undo.push_back({ m_text, cursor, anchor });
        if (m_undo.size() > kMaxUndo) m_undo.erase(m_undo.begin());
    }
    m_redo.clear();
    m_lastKind = kind;
    m_lastEditTime = t;
}

void Document::shiftMarkers(int fromLine0, int delta)
{
    if (delta == 0) return;
    // Markers below the edited line move with the text; markers inside a
    // deleted range collapse onto the edited line.
    auto shift = [&](int line1) {
        int l0 = line1 - 1;
        if (l0 > fromLine0) l0 = std::max(fromLine0, l0 + delta);
        return l0 + 1;
    };
    std::set<int> bps;
    for (int l : breakpoints) bps.insert(shift(l));
    breakpoints.swap(bps);
    for (auto& d : diagnostics) d.line = shift(d.line);
}

void Document::replace(size_t from, size_t to, const std::string& s, EditKind kind)
{
    from = std::min(from, m_text.size());
    to = std::clamp(to, from, m_text.size());
    if (from == to && s.empty()) return;
    pushUndo(kind);

    int line0 = lineOf(from);
    int removed = int(std::count(m_text.begin() + long(from), m_text.begin() + long(to), '\n'));
    int added = int(std::count(s.begin(), s.end(), '\n'));

    m_text.replace(from, to - from, s);
    cursor = anchor = from + s.size();
    preferredCol = -1;
    m_dirty = true;
    rebuildLines();
    shiftMarkers(line0, added - removed);
}

void Document::insert(const std::string& s, EditKind kind)
{
    replace(selMin(), selMax(), s, hasSelection() ? EditKind::Other : kind);
}

void Document::backspace()
{
    if (hasSelection()) { replace(selMin(), selMax(), "", EditKind::Other); return; }
    if (cursor == 0) return;
    replace(prevChar(cursor), cursor, "", EditKind::Delete);
}

void Document::deleteForward()
{
    if (hasSelection()) { replace(selMin(), selMax(), "", EditKind::Other); return; }
    if (cursor >= m_text.size()) return;
    replace(cursor, nextChar(cursor), "", EditKind::Delete);
}

void Document::deleteRange(size_t from, size_t to)
{
    replace(from, to, "", EditKind::Other);
}

void Document::undo()
{
    if (m_undo.empty()) return;
    m_redo.push_back({ m_text, cursor, anchor });
    Snapshot s = std::move(m_undo.back());
    m_undo.pop_back();
    m_text = std::move(s.text);
    cursor = std::min(s.cursor, m_text.size());
    anchor = std::min(s.anchor, m_text.size());
    m_lastKind = EditKind::Other;
    updateDirtyFromHash();
    rebuildLines();
}

void Document::redo()
{
    if (m_redo.empty()) return;
    m_undo.push_back({ m_text, cursor, anchor });
    Snapshot s = std::move(m_redo.back());
    m_redo.pop_back();
    m_text = std::move(s.text);
    cursor = std::min(s.cursor, m_text.size());
    anchor = std::min(s.anchor, m_text.size());
    m_lastKind = EditKind::Other;
    updateDirtyFromHash();
    rebuildLines();
}

// Undoing back to the saved text makes the document clean again.
void Document::updateDirtyFromHash()
{
    m_dirty = m_path.empty() ? !m_text.empty() : std::hash<std::string>{}(m_text) != m_savedHash;
}

void Document::toggleBreakpoint(int line1)
{
    if (!breakpoints.erase(line1)) breakpoints.insert(line1);
}

// --- Syntax / layout caches --------------------------------------------------

const std::vector<uint8_t>& Document::blockCommentState()
{
    if (m_commentVersion == m_version) return m_commentState;
    m_commentVersion = m_version;
    m_commentState.assign(m_lineStarts.size(), 0);
    if (m_lang != Lang::CFamily) return m_commentState;

    // One pass over the file that knows about strings, char literals and
    // line comments, so "/*" inside a string does not start a comment.
    enum { Code, Block, Str, Chr, LineC } st = Code;
    int line = 0;
    const size_t n = m_text.size();
    for (size_t i = 0; i < n; ++i)
    {
        char c = m_text[i];
        if (c == '\n')
        {
            if (st == LineC || st == Str || st == Chr) st = Code;
            ++line;
            m_commentState[size_t(line)] = (st == Block);
            continue;
        }
        switch (st)
        {
        case Code:
            if (c == '/' && i + 1 < n && m_text[i + 1] == '*') { st = Block; ++i; }
            else if (c == '/' && i + 1 < n && m_text[i + 1] == '/') st = LineC;
            else if (c == '"') st = Str;
            else if (c == '\'') st = Chr;
            break;
        case Block:
            if (c == '*' && i + 1 < n && m_text[i + 1] == '/') { st = Code; ++i; }
            break;
        case Str:
            if (c == '\\') ++i;
            else if (c == '"') st = Code;
            break;
        case Chr:
            if (c == '\\') ++i;
            else if (c == '\'') st = Code;
            break;
        case LineC:
            break;
        }
    }
    return m_commentState;
}

int Document::maxColumns()
{
    if (m_maxColsVersion == m_version) return m_maxCols;
    m_maxColsVersion = m_version;
    int best = 0;
    for (int l = 0; l < lineCount(); ++l)
    {
        // Byte length is an upper bound; only measure lines that could win.
        size_t len = lineEnd(l) - lineStart(l);
        if (int(len) * kTabWidth <= best) continue;
        best = std::max(best, columnOf(l, lineEnd(l)));
    }
    m_maxCols = best;
    return best;
}
