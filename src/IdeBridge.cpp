#include "IdeBridge.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "Wake.h"

IdeBridge::~IdeBridge()
{
    stop();
}

bool IdeBridge::start()
{
    if (m_listen >= 0) return true;
    // A private directory: only this user can connect to the socket.
    std::string dir = "/tmp/sf-ide-" + std::to_string(getuid());
    mkdir(dir.c_str(), 0700);
    struct stat st;
    if (lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077) != 0)
        return false;
    m_path = dir + "/bridge-" + std::to_string(getpid()) + ".sock";
    unlink(m_path.c_str());

    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    if (m_path.size() >= sizeof addr.sun_path) return false;
    std::strcpy(addr.sun_path, m_path.c_str());
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(fd, 8) != 0)
    {
        close(fd);
        return false;
    }
    chmod(m_path.c_str(), 0600);
    m_listen = fd;
    m_stop = false;
    m_thread = std::thread(&IdeBridge::acceptLoop, this);
    return true;
}

void IdeBridge::stop()
{
    m_stop = true;
    if (m_thread.joinable()) m_thread.join();
    if (m_listen >= 0)
    {
        close(m_listen);
        m_listen = -1;
        unlink(m_path.c_str());
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& r : m_queue) close(r.fd);
    m_queue.clear();
}

void IdeBridge::acceptLoop()
{
    while (!m_stop)
    {
        pollfd p = { m_listen, POLLIN, 0 };
        if (poll(&p, 1, 200) <= 0) continue;
        int fd = accept(m_listen, nullptr, nullptr);
        if (fd < 0) continue;
        // Read one line; a slow or silent client cannot block the IDE for long.
        timeval tv = { 5, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        std::string line;
        char buf[65536];
        bool ok = false;
        while (line.size() < (64u << 20))
        {
            ssize_t n = read(fd, buf, sizeof buf);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            line.append(buf, size_t(n));
            if (line.back() == '\n') { ok = true; break; }
        }
        Request r;
        if (!ok || !parseRequest(line, r))
        {
            r.fd = fd;
            reply(r, "{\"ok\":false,\"error\":\"bad request\"}");
            continue;
        }
        r.fd = fd;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_queue.push_back(std::move(r));
        }
        wakeUi();
    }
}

bool IdeBridge::next(Request& out)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_queue.empty()) return false;
    out = std::move(m_queue.front());
    m_queue.pop_front();
    return true;
}

void IdeBridge::reply(Request& r, const std::string& json)
{
    if (r.fd < 0) return;
    std::string out = json + "\n";
    size_t off = 0;
    while (off < out.size())
    {
        ssize_t n = write(r.fd, out.data() + off, out.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        off += size_t(n);
    }
    close(r.fd);
    r.fd = -1;
}

// --- JSON ---------------------------------------------------------------------------

std::string IdeBridge::quote(const std::string& s)
{
    std::string out = "\"";
    for (unsigned char c : s)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            }
            else
            {
                out.push_back(char(c));
            }
        }
    }
    return out + "\"";
}

namespace {

void skipWs(const std::string& s, size_t& i)
{
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
}

void appendUtf8(std::string& out, uint32_t cp)
{
    if (cp < 0x80) out.push_back(char(cp));
    else if (cp < 0x800) { out.push_back(char(0xC0 | (cp >> 6))); out.push_back(char(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000)
    {
        out.push_back(char(0xE0 | (cp >> 12)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    }
    else
    {
        out.push_back(char(0xF0 | (cp >> 18)));
        out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    }
}

bool parseHex4(const std::string& s, size_t i, uint32_t& v)
{
    if (i + 4 > s.size()) return false;
    v = 0;
    for (size_t k = i; k < i + 4; ++k)
    {
        char c = s[k];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
        else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
        else return false;
    }
    return true;
}

bool parseString(const std::string& s, size_t& i, std::string& out)
{
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    out.clear();
    while (i < s.size())
    {
        char c = s[i++];
        if (c == '"') return true;
        if (c != '\\') { out.push_back(c); continue; }
        if (i >= s.size()) return false;
        char e = s[i++];
        switch (e)
        {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
            uint32_t cp;
            if (!parseHex4(s, i, cp)) return false;
            i += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u')
            {
                uint32_t lo;
                if (parseHex4(s, i + 2, lo) && lo >= 0xDC00 && lo < 0xE000)
                {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 6;
                }
            }
            appendUtf8(out, cp);
            break;
        }
        default: return false;
        }
    }
    return false;
}

// Skips any JSON value (used for keys we do not care about).
bool skipValue(const std::string& s, size_t& i)
{
    skipWs(s, i);
    if (i >= s.size()) return false;
    if (s[i] == '"') { std::string tmp; return parseString(s, i, tmp); }
    if (s[i] == '{' || s[i] == '[')
    {
        int depth = 0;
        while (i < s.size())
        {
            char c = s[i];
            if (c == '"') { std::string tmp; if (!parseString(s, i, tmp)) return false; continue; }
            if (c == '{' || c == '[') ++depth;
            if (c == '}' || c == ']') { if (--depth == 0) { ++i; return true; } }
            ++i;
        }
        return false;
    }
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']') ++i;
    return true;
}

} // namespace

bool IdeBridge::parseRequest(const std::string& line, Request& out)
{
    size_t i = 0;
    skipWs(line, i);
    if (i >= line.size() || line[i] != '{') return false;
    ++i;
    for (;;)
    {
        skipWs(line, i);
        if (i < line.size() && line[i] == '}') return !out.op.empty();
        std::string key;
        if (!parseString(line, i, key)) return false;
        skipWs(line, i);
        if (i >= line.size() || line[i] != ':') return false;
        ++i;
        skipWs(line, i);
        if (i < line.size() && line[i] == '"')
        {
            std::string v;
            if (!parseString(line, i, v)) return false;
            if (key == "op") out.op = std::move(v);
            else if (key == "path") out.path = std::move(v);
            else if (key == "text") out.text = std::move(v);
        }
        else if (!skipValue(line, i))
        {
            return false;
        }
        skipWs(line, i);
        if (i < line.size() && line[i] == ',') { ++i; continue; }
        if (i < line.size() && line[i] == '}') return !out.op.empty();
        return false;
    }
}
