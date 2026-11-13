// takaro_core.cpp — ServerManager-Takaro universal core, embedded in winmm.dll.
//
// One in-process core, identical for every Unreal Engine game. It owns:
//   • a WinHTTP WebSocket straight to the dtMatrix generic hub (Takaro generic protocol;
//     default ws://127.0.0.1:8442/, WS_URL overrides; no separate bridge process),
//   • for Astroneer, the game's own console (RCON) on localhost for roster + commands, and
//   • file-based IPC to the UE4SS Lua profile that hooks the game.
//
// Why files and not UDP? VoxelTurf exposes a native N_EXTERNAL UDP API to its Lua;
// UE4SS Lua has no sockets at all — only io/os (see Longvinter's TakaroAgent). So the
// universal game-side transport is a tiny file protocol both sides can drive:
//
//   ipc/evt/<seq>.json     Lua → core : one game event   {type, data}
//   ipc/req/<id>.json      core → Lua : one action req   {action, args}
//   ipc/res/<id>.json      Lua → core : one action result{success, result, error}
//   ipc/players.json       Lua → core : current roster   [{gameId,name,steamId}]
//
// Every file is published atomically (write .tmp then rename), so a reader never sees
// a half-written file. Entry point: StartTakaroCore() — call once on DLL load.

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>       // must precede windows.h; used by the in-DLL RCON client
  #include <ws2tcpip.h>
  #include <windows.h>
  #include <winhttp.h>
#else
  // ── Linux/POSIX build (native UE4SS-Linux path; the core is an LD_PRELOAD .so) ──
  // The same core, minus the Windows OS calls: sockets are POSIX, TLS+WebSocket is
  // OpenSSL (WinHTTP has no Linux twin), and a tiny Win32 compat shim below lets the
  // shared body compile unchanged.
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <dirent.h>
  #include <errno.h>
  #include <sys/time.h>
  #include <openssl/ssl.h>
  #include <openssl/err.h>
  typedef int SOCKET;
  typedef unsigned long DWORD;
  #define INVALID_SOCKET (-1)
  #define SOCKET_ERROR   (-1)
  #define closesocket(s) ::close(s)
  #define DeleteFileA(p) (::remove(p))
  #define _snprintf snprintf
  static inline void SleepMs(int ms) { if (ms > 0) usleep(ms * 1000); }
  #define Sleep(ms) SleepMs((int)(ms))
#endif

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>
#include <thread>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/stat.h>

#include "json.hpp"
using nlohmann::json;

namespace {

// ─── Config ────────────────────────────────────────────────────────────────────
std::string IDENTITY_TOKEN, REGISTRATION_TOKEN;
int   POLL_INTERVAL_MS = 1000;   // how often the core drains ipc/evt + reads roster
int   REQ_TIMEOUT_MS   = 6000;   // how long a Takaro action waits for the Lua result
bool  ENABLED          = true;
std::string SERVER_STARTED_MSG = "Server started";

// RCON (Source protocol). Optional: when RCON_PORT > 0, Takaro's console
// (executeConsoleCommand) and any RCON-backed action are run against the game's OWN
// RCON on localhost — because that IS how those games expose their admin/command
// surface. Still one in-process DLL dialing localhost; not a separate bridge process.
// These are read from TakaroConfig.txt (unlike the WS host, which is hardcoded): the
// RCON endpoint is the operator's own local server, not a security boundary.
std::string RCON_HOST     = "127.0.0.1";
int         RCON_PORT     = 0;            // 0 = RCON disabled (fall back to UE console)
std::string RCON_PASSWORD;
std::string RCON_SHUTDOWN_CMD;            // optional: game's RCON shutdown cmd (e.g. "Shutdown 1")
// RCON wire protocol: "source" (Valve Source RCON, default) or "astroneer" (Astroneer's
// line protocol: password line, then "DS<Command>" lines, JSON replies). Astroneer is also
// auto-detected from the server exe name, and its ConsolePort/ConsolePassword are read
// from AstroServerSettings.ini when RCON_PORT/RCON_PASSWORD are not set here.
std::string RCON_PROTOCOL = "source";
int ROSTER_POLL_MS = 3000;                // astroneer: how often the roster is read over RCON

// Hub websocket endpoint: dtMatrix's generic hub (Takaro generic protocol). Default is the
// plain local listener on the same machine; WS_URL in TakaroConfig.txt overrides it
// (ws://host:port/ or wss://host:port/). Build-time overrides still work:
//     -DTAKARO_WS_HOST=L"..." -DTAKARO_WS_HOST_A="..." -DTAKARO_WS_PORT=... -DTAKARO_WS_SECURE=...
#ifndef TAKARO_WS_HOST
#define TAKARO_WS_HOST L"127.0.0.1"
#endif
// Narrow copy of the host for the Linux OpenSSL path (WinHTTP takes the wide one).
#ifndef TAKARO_WS_HOST_A
#define TAKARO_WS_HOST_A "127.0.0.1"
#endif
#ifndef TAKARO_WS_PORT
#define TAKARO_WS_PORT 8442
#endif
#ifndef TAKARO_WS_SECURE
#define TAKARO_WS_SECURE 0
#endif
std::string g_wsHost = TAKARO_WS_HOST_A;
int         g_wsPort = TAKARO_WS_PORT;
bool        g_wsSecure = TAKARO_WS_SECURE != 0;
std::string g_wsPath = "/";

std::string g_gameDir, g_modDir, g_ipcDir, g_configPath, g_logPath;
std::string g_evtDir, g_reqDir, g_resDir, g_playersPath;
std::string g_reqFile, g_resFile;   // single-file action mailbox (io.open-only, see main.lua)

// ─── State ─────────────────────────────────────────────────────────────────────
std::atomic<bool> g_running{true};
std::atomic<bool> g_connected{false};
std::atomic<bool> g_startupSent{false};

#ifdef _WIN32
HINTERNET g_wsSession = NULL, g_wsConn = NULL, g_ws = NULL;
#else
SSL_CTX* g_sslCtx = nullptr;
SSL*     g_ssl    = nullptr;   // the live TLS websocket
int      g_wsSock = -1;
#endif
std::mutex g_wsMutex;      // guards the websocket + sends
std::mutex g_logMutex;
std::mutex g_cacheMutex;
std::mutex g_actionMutex;  // serializes gameAction (one mailbox request at a time)
std::atomic<unsigned long long> g_reqSeq{1};

struct PlayerInfo { std::string gameId, name, steamId, platformId, epicOnlineServicesId; };
std::map<std::string, PlayerInfo> g_players;   // by gameId

const uint64_t STEAM64_BASE = 76561197960265728ULL;

// ─── Logging ───────────────────────────────────────────────────────────────────
std::string nowStamp() {
    char b[40];
#ifdef _WIN32
    SYSTEMTIME st; GetSystemTime(&st);
    _snprintf(b, sizeof(b), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
#else
    struct timeval tv; gettimeofday(&tv, nullptr);
    struct tm g; gmtime_r(&tv.tv_sec, &g);
    snprintf(b, sizeof(b), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec,
             (int)(tv.tv_usec / 1000));
#endif
    return b;
}
void logmsg(const std::string& m) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    std::string line = nowStamp() + " " + m + "\n";
    FILE* f = fopen(g_logPath.c_str(), "ab");
    if (f) { fwrite(line.data(), 1, line.size(), f); fclose(f); }
}

// ─── Path / config helpers ──────────────────────────────────────────────────────
std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool dirExists(const std::string& p) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st; return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}
#ifdef _WIN32
void ensureDir(const std::string& p) { CreateDirectoryA(p.c_str(), NULL); }
#else
void ensureDir(const std::string& p) { mkdir(p.c_str(), 0775); }
#endif

// The game exe dir holds winmm.dll + ue4ss\. The TakaroConnector mod may live under a
// couple of standard UE4SS mods roots — probe them so one DLL fits every layout.
void computePaths() {
#ifdef _WIN32
    const char SEP = '\\';
    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH);
    char* sl = strrchr(exe, SEP); if (sl) *sl = '\0';
    g_gameDir = exe;
    const char* roots[] = { "\\ue4ss\\Mods", "\\Mods", "\\ue4ss\\mods" };
    g_modDir = g_gameDir + "\\ue4ss\\Mods\\TakaroConnector";   // default
#else
    const char SEP = '/';
    char exe[4096]; ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) n = 0; exe[n] = '\0';
    char* sl = strrchr(exe, SEP); if (sl) *sl = '\0';
    g_gameDir = exe;
    // UE4SS-Linux keeps Mods next to the server binary (Pal/Binaries/Linux/Mods/...).
    const char* roots[] = { "/Mods", "/ue4ss/Mods", "/ue4ss/mods" };
    g_modDir = g_gameDir + "/Mods/TakaroConnector";           // default
#endif
    for (const char* r : roots) {
        std::string cand = g_gameDir + r + SEP + "TakaroConnector";
        if (dirExists(cand)) { g_modDir = cand; break; }
    }
    auto J = [&](const std::string& a, const char* b) { return a + SEP + b; };
    g_configPath  = J(g_modDir, "TakaroConfig.txt");
    g_logPath     = J(g_modDir, "core.log");
    g_ipcDir      = J(g_modDir, "ipc");
    g_evtDir      = J(g_ipcDir, "evt");
    g_reqDir      = J(g_ipcDir, "req");
    g_resDir      = J(g_ipcDir, "res");
    g_reqFile     = J(g_ipcDir, "req.json");   // single-file mailbox (dir listing is dead in UE4SS Lua)
    g_resFile     = J(g_ipcDir, "res.json");
    g_playersPath = J(g_ipcDir, "players.json");
    ensureDir(g_modDir); ensureDir(g_ipcDir);
    ensureDir(g_evtDir); ensureDir(g_reqDir); ensureDir(g_resDir);
}

void loadConfig() {
    FILE* f = fopen(g_configPath.c_str(), "rb");
    if (!f) return;
    std::string data; char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    fclose(f);
    std::map<std::string, std::string> cfg;
    size_t pos = 0;
    while (pos < data.size()) {
        size_t nl = data.find('\n', pos);
        std::string line = trim(data.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
        pos = (nl == std::string::npos) ? data.size() : nl + 1;
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        cfg[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    auto get = [&](const char* k) -> std::string {
        auto it = cfg.find(k); return it == cfg.end() ? std::string() : it->second;
    };
    IDENTITY_TOKEN     = !get("SERVER_NAME").empty() ? get("SERVER_NAME") : get("IDENTITY_TOKEN");
    REGISTRATION_TOKEN = get("REGISTRATION_TOKEN");
    if (!get("POLL_INTERVAL_MS").empty()) POLL_INTERVAL_MS = atoi(get("POLL_INTERVAL_MS").c_str());
    if (!get("REQ_TIMEOUT_MS").empty())   REQ_TIMEOUT_MS   = atoi(get("REQ_TIMEOUT_MS").c_str());
    if (!get("SERVER_STARTED_MSG").empty()) SERVER_STARTED_MSG = get("SERVER_STARTED_MSG");
    std::string en = get("ENABLED");
    if (!en.empty()) ENABLED = (en != "false" && en != "0");
    if (!get("RCON_HOST").empty())     RCON_HOST     = get("RCON_HOST");
    if (!get("RCON_PORT").empty())     RCON_PORT     = atoi(get("RCON_PORT").c_str());
    RCON_PASSWORD     = get("RCON_PASSWORD");
    RCON_SHUTDOWN_CMD = get("RCON_SHUTDOWN_CMD");
    if (!get("RCON_PROTOCOL").empty()) {
        RCON_PROTOCOL = get("RCON_PROTOCOL");
        std::transform(RCON_PROTOCOL.begin(), RCON_PROTOCOL.end(), RCON_PROTOCOL.begin(), ::tolower);
    }
    if (!get("ROSTER_POLL_MS").empty()) ROSTER_POLL_MS = atoi(get("ROSTER_POLL_MS").c_str());
    if (ROSTER_POLL_MS < 500) ROSTER_POLL_MS = 500;

    // WS_URL = ws[s]://host[:port][/path]
    std::string url = get("WS_URL");
    if (!url.empty()) {
        bool secure = false; std::string rest;
        if (url.compare(0, 6, "wss://") == 0)      { secure = true;  rest = url.substr(6); }
        else if (url.compare(0, 5, "ws://") == 0)  { secure = false; rest = url.substr(5); }
        if (!rest.empty()) {
            size_t sl = rest.find('/');
            std::string hostport = rest.substr(0, sl);
            g_wsPath = sl == std::string::npos ? "/" : rest.substr(sl);
            size_t colon = hostport.rfind(':');
            if (colon != std::string::npos) {
                g_wsHost = hostport.substr(0, colon);
                g_wsPort = atoi(hostport.substr(colon + 1).c_str());
            } else {
                g_wsHost = hostport;
                g_wsPort = secure ? 443 : 80;
            }
            g_wsSecure = secure;
        }
    }
}

std::string toSteamId64(const std::string& acct) {
    std::string s = trim(acct);
    if (s.empty()) return "";
    for (char c : s) if (c < '0' || c > '9') return s;   // non-numeric -> leave as-is
    uint64_t v = strtoull(s.c_str(), NULL, 10);
    if (v > 0 && v < STEAM64_BASE) v += STEAM64_BASE;    // steam3 acct id -> steam64
    char b[32]; _snprintf(b, sizeof(b), "%llu", (unsigned long long)v);
    return b;
}

// ─── Small file utilities ─────────────────────────────────────────────────────
bool readFile(const std::string& path, std::string& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear(); char buf[8192]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}
// Publish atomically: write .tmp then rename over the target.
bool writeFileAtomic(const std::string& path, const std::string& data) {
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    fwrite(data.data(), 1, data.size(), f); fclose(f);
#ifdef _WIN32
    MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
#else
    rename(tmp.c_str(), path.c_str());   // atomic replace on POSIX
#endif
    return true;
}
// List *.json files in dir, sorted ascending by name (so numeric seq is chronological).
std::vector<std::string> listJson(const std::string& dir) {
    std::vector<std::string> names;
    auto keep = [](const std::string& n) {
        return n.size() > 5 && n.substr(n.size() - 5) == ".json";
    };
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*.json").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::string n = fd.cFileName;
            if (n.size() > 4 && n.substr(n.size() - 4) == ".tmp") continue;
            names.push_back(n);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    DIR* d = opendir(dir.c_str());
    if (d) {
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            std::string n = e->d_name;
            if (keep(n)) names.push_back(n);
        }
        closedir(d);
    }
#endif
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
        // numeric-aware: shorter number sorts first, else lexicographic
        if (a.size() != b.size()) return a.size() < b.size();
        return a < b;
    });
    return names;
}

// ─── Source RCON client (in-process) ──────────────────────────────────────────────
// Many dedicated servers expose their real command/admin surface only over RCON — so
// Takaro's console (executeConsoleCommand) has to reach the game that way. We open the
// game's OWN RCON socket on localhost, run the command, and return its text. This is
// still one in-process DLL talking to localhost — no separate bridge process.
//
// Wire format (Valve Source RCON): int32 size, int32 id, int32 type, body(ASCII)+\0, \0.
//   type: 3 = AUTH, 2 = AUTH_RESPONSE / EXECCOMMAND, 0 = RESPONSE_VALUE.
namespace rcon {
    std::mutex g_mtx;          // one short-lived RCON socket at a time
    bool       g_wsaInit = false;

    bool sendPacket(SOCKET s, int32_t id, int32_t type, const std::string& body) {
        int32_t size = (int32_t)(body.size() + 10);
        std::vector<char> buf(4 + size, 0);
        memcpy(&buf[0], &size, 4);
        memcpy(&buf[4], &id, 4);
        memcpy(&buf[8], &type, 4);
        if (!body.empty()) memcpy(&buf[12], body.data(), body.size());
        // last two bytes stay 0 (body null-terminator + packet null-terminator)
        int total = (int)buf.size(), sent = 0;
        while (sent < total) {
            int n = send(s, buf.data() + sent, total - sent, 0);
            if (n <= 0) return false;
            sent += n;
        }
        return true;
    }
    bool recvAll(SOCKET s, char* out, int n) {
        int got = 0;
        while (got < n) {
            int r = recv(s, out + got, n - got, 0);
            if (r <= 0) return false;
            got += r;
        }
        return true;
    }
    bool recvPacket(SOCKET s, int32_t& id, int32_t& type, std::string& body) {
        char hdr[4];
        if (!recvAll(s, hdr, 4)) return false;
        int32_t size; memcpy(&size, hdr, 4);
        if (size < 10 || size > 4 * 1024 * 1024) return false;
        std::vector<char> rest(size);
        if (!recvAll(s, rest.data(), size)) return false;
        memcpy(&id, &rest[0], 4);
        memcpy(&type, &rest[4], 4);
        int bodyLen = size - 10;                    // minus id(4)+type(4)+two null bytes(2)
        body.assign(rest.data() + 8, bodyLen > 0 ? bodyLen : 0);
        return true;
    }
}

// Run one command over the game's RCON. Returns true on success; fills `out` with the
// server's text response (or an error string on failure).
bool rconExec(const std::string& cmd, std::string& out) {
    std::lock_guard<std::mutex> lk(rcon::g_mtx);
    out.clear();
    if (RCON_PORT <= 0) { out = "RCON not configured"; return false; }

#ifdef _WIN32
    if (!rcon::g_wsaInit) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { out = "WSAStartup failed"; return false; }
        rcon::g_wsaInit = true;
    }
#endif

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { out = "socket() failed"; return false; }

    sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((u_short)RCON_PORT);
    addr.sin_addr.s_addr = inet_addr(RCON_HOST.c_str());
    if (addr.sin_addr.s_addr == INADDR_NONE) {          // not a dotted quad -> resolve
        struct addrinfo hints, *ai = NULL; memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
        char portstr[16]; _snprintf(portstr, sizeof(portstr), "%d", RCON_PORT);
        if (getaddrinfo(RCON_HOST.c_str(), portstr, &hints, &ai) != 0 || !ai) {
            closesocket(s); out = "RCON host resolve failed"; return false;
        }
        memcpy(&addr, ai->ai_addr, sizeof(sockaddr_in));
        freeaddrinfo(ai);
    }

#ifdef _WIN32
    DWORD tmo = 4000;                                    // connect/auth timeout (ms on Winsock)
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tmo, sizeof(tmo));
#else
    struct timeval tmo; tmo.tv_sec = 4; tmo.tv_usec = 0;  // POSIX SO_RCVTIMEO takes timeval
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof(tmo));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tmo, sizeof(tmo));
#endif
    if (connect(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
        closesocket(s); out = "RCON connect failed (" + RCON_HOST + ":" + std::to_string(RCON_PORT) + ")"; return false;
    }

    // authenticate
    if (!rcon::sendPacket(s, 1, 3, RCON_PASSWORD)) { closesocket(s); out = "RCON auth send failed"; return false; }
    for (int i = 0; i < 4; i++) {
        int32_t id, type; std::string body;
        if (!rcon::recvPacket(s, id, type, body)) { closesocket(s); out = "RCON auth recv failed"; return false; }
        if (type == 2) {                                 // AUTH_RESPONSE
            if (id == -1) { closesocket(s); out = "RCON auth failed (bad RCON_PASSWORD)"; return false; }
            break;
        }
    }

    // execute + drain the response (short idle timeout terminates multi-packet reads)
    if (!rcon::sendPacket(s, 2, 2, cmd)) { closesocket(s); out = "RCON exec send failed"; return false; }
    DWORD drain = 800;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&drain, sizeof(drain));
    for (int i = 0; i < 128; i++) {
        int32_t id, type; std::string body;
        if (!rcon::recvPacket(s, id, type, body)) break; // idle timeout / close -> done
        if (type == 0) out += body;                      // RESPONSE_VALUE
    }
    closesocket(s);
    return true;
}

// ─── Astroneer RCON (in-process) ─────────────────────────────────────────────────
// Astroneer's dedicated server has its own console protocol on ConsolePort (TCP): the
// first line is the password, then each line is a "DS<Command> args" command; replies are
// one JSON object. Some commands (DSSaveGame, DSNewGame, DSServerShutdown) send no reply.
bool g_astro = false;   // Astroneer mode (RCON_PROTOCOL=astroneer or AstroServer exe)

static void setRecvTimeout(SOCKET s, int ms) {
#ifdef _WIN32
    DWORD t = (DWORD)ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof(t));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&t, sizeof(t));
#else
    struct timeval t; t.tv_sec = ms / 1000; t.tv_usec = (ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof(t));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &t, sizeof(t));
#endif
}

// Port/password from TakaroConfig.txt win; otherwise read them from the server's own
// AstroServerSettings.ini (Astro\Saved\Config\WindowsServer, two levels above Win64).
// Re-read on every call, so a changed ini (the server rewrites it) is picked up.
static void astroEndpoint(int& port, std::string& pass) {
    port = RCON_PORT; pass = RCON_PASSWORD;
    if (port > 0 && !pass.empty()) return;
    std::string ini;
#ifdef _WIN32
    std::string p = g_gameDir + "\\..\\..\\Saved\\Config\\WindowsServer\\AstroServerSettings.ini";
#else
    std::string p = g_gameDir + "/../../Saved/Config/LinuxServer/AstroServerSettings.ini";
#endif
    bool portSet = port > 0;
    if (readFile(p, ini)) {
        size_t pos = 0;
        while (pos < ini.size()) {
            size_t nl = ini.find('\n', pos);
            std::string line = trim(ini.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
            pos = (nl == std::string::npos) ? ini.size() : nl + 1;
            if (!portSet && line.compare(0, 12, "ConsolePort=") == 0) port = atoi(line.c_str() + 12);
            else if (pass.empty() && line.compare(0, 16, "ConsolePassword=") == 0) pass = trim(line.substr(16));
        }
    }
    if (port <= 0) port = 1234;   // the game's default (DefaultAstroServerSettings.ini in the pak)
}

static bool sendAll(SOCKET s, const std::string& d) {
    size_t off = 0;
    while (off < d.size()) {
        int n = send(s, d.data() + off, (int)(d.size() - off), 0);
        if (n <= 0) return false;
        off += (size_t)n;
    }
    return true;
}

// Run one Astroneer console command (with or without the DS prefix). `out` gets the raw
// reply ("" when the command sends none) or an error string when it returns false.
bool astroExec(const std::string& command, std::string& out) {
    std::lock_guard<std::mutex> lk(rcon::g_mtx);
    out.clear();
    int port; std::string pass;
    astroEndpoint(port, pass);
    if (port <= 0) { out = "Astroneer RCON not configured (no ConsolePort in AstroServerSettings.ini)"; return false; }

#ifdef _WIN32
    if (!rcon::g_wsaInit) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { out = "WSAStartup failed"; return false; }
        rcon::g_wsaInit = true;
    }
#endif
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { out = "socket() failed"; return false; }
    sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((u_short)port);
    addr.sin_addr.s_addr = inet_addr(RCON_HOST.c_str());
    setRecvTimeout(s, 4000);
    if (connect(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
        closesocket(s); out = "RCON connect failed (" + RCON_HOST + ":" + std::to_string(port) + ")"; return false;
    }
    std::string cmd = trim(command);
    if (!(cmd.size() > 2 && cmd[0] == 'D' && cmd[1] == 'S' && isupper((unsigned char)cmd[2]))) cmd = "DS" + cmd;
    if (!sendAll(s, pass + "\n")) { closesocket(s); out = "RCON password send failed"; return false; }
    Sleep(150);
    if (!sendAll(s, cmd + "\n")) { closesocket(s); out = "RCON command send failed"; return false; }

    // Wait up to 1.5 s for the first bytes, then read until the reply parses as JSON or
    // goes quiet for 300 ms. No bytes at all = a command that sends no reply.
    setRecvTimeout(s, 1500);
    char buf[16384];
    for (int i = 0; i < 512; i++) {
        int r = recv(s, buf, sizeof(buf), 0);
        if (r <= 0) break;
        out.append(buf, r);
        std::string t = trim(out);
        if (!t.empty() && json::accept(t)) break;
        setRecvTimeout(s, 300);
    }
    closesocket(s);
    out = trim(out);
    return true;
}

// DSListPlayers -> playerInfo array (every known player, online or not). False on error.
static bool astroListPlayers(json& list) {
    list = json::array();
    std::string out;
    if (!astroExec("DSListPlayers", out)) return false;
    try {
        json j = json::parse(out);
        if (j.is_object() && j.contains("playerInfo") && j["playerInfo"].is_array()) { list = j["playerInfo"]; return true; }
    } catch (...) {}
    return false;
}
static std::string astroGuid(const json& p) {
    if (!p.is_object() || !p.contains("playerGuid")) return "";
    const json& g = p["playerGuid"];
    if (g.is_string()) return g.get<std::string>();
    if (g.is_number()) return g.dump();
    return "";
}

// ─── WebSocket send ──────────────────────────────────────────────────────────────
#ifndef _WIN32
// ── Minimal RFC6455 client over the OpenSSL TLS stream ─────────────────────────────
// Windows uses WinHTTP's built-in websocket; Linux has no equivalent, so we frame it
// ourselves over the TLS BIO. Client frames are masked (required by the RFC); server
// frames are not. Only used from wsRunOnce / wsSend, both under g_wsMutex for sends.
static bool sslWriteAll(const char* p, size_t n) {
    size_t off = 0;
    while (off < n) { int w = SSL_write(g_ssl, p + off, (int)(n - off)); if (w <= 0) return false; off += (size_t)w; }
    return true;
}
static bool sslReadN(char* p, size_t n) {
    size_t off = 0;
    while (off < n) { int r = SSL_read(g_ssl, p + off, (int)(n - off)); if (r <= 0) return false; off += (size_t)r; }
    return true;
}
static bool wsSendFrame(uint8_t opcode, const std::string& payload) {
    std::string f;
    f.push_back((char)(0x80 | opcode));               // FIN + opcode
    size_t len = payload.size();
    const uint8_t MB = 0x80;                           // mask bit (client frames must be masked)
    if (len < 126) f.push_back((char)(MB | (uint8_t)len));
    else if (len <= 0xFFFF) { f.push_back((char)(MB | 126)); f.push_back((char)((len >> 8) & 0xFF)); f.push_back((char)(len & 0xFF)); }
    else { f.push_back((char)(MB | 127)); for (int i = 7; i >= 0; --i) f.push_back((char)((len >> (8 * i)) & 0xFF)); }
    uint8_t mask[4]; for (int i = 0; i < 4; ++i) mask[i] = (uint8_t)(rand() & 0xFF);
    f.append((const char*)mask, 4);
    size_t base = f.size(); f.resize(base + len);
    for (size_t i = 0; i < len; ++i) f[base + i] = (char)((uint8_t)payload[i] ^ mask[i & 3]);
    return sslWriteAll(f.data(), f.size());
}
// Read one full application message, reassembling fragments and answering pings.
// Returns true with `out` set (text/binary); false on close or socket error.
static bool wsRecvMessage(std::string& out) {
    out.clear();
    for (;;) {
        unsigned char h[2];
        if (!sslReadN((char*)h, 2)) return false;
        bool fin = h[0] & 0x80; uint8_t opcode = h[0] & 0x0F; bool masked = h[1] & 0x80;
        uint64_t len = h[1] & 0x7F;
        if (len == 126) { unsigned char e[2]; if (!sslReadN((char*)e, 2)) return false; len = ((uint64_t)e[0] << 8) | e[1]; }
        else if (len == 127) { unsigned char e[8]; if (!sslReadN((char*)e, 8)) return false; len = 0; for (int i = 0; i < 8; ++i) len = (len << 8) | e[i]; }
        unsigned char mk[4] = {0,0,0,0};
        if (masked && !sslReadN((char*)mk, 4)) return false;
        std::string payload;
        if (len > 0) {
            if (len > 64ull * 1024 * 1024) return false;   // sanity cap
            payload.resize((size_t)len);
            if (!sslReadN(&payload[0], (size_t)len)) return false;
            if (masked) for (size_t i = 0; i < payload.size(); ++i) payload[i] ^= mk[i & 3];
        }
        if (opcode == 0x8) return false;                    // close
        if (opcode == 0x9) { std::lock_guard<std::mutex> lk(g_wsMutex); wsSendFrame(0xA, payload); continue; } // ping->pong
        if (opcode == 0xA) continue;                        // pong
        out += payload;                                     // text/binary/continuation
        if (fin) return true;
    }
}
#endif // !_WIN32

bool wsSend(const json& obj) {
    std::string s = obj.dump();
    std::lock_guard<std::mutex> lk(g_wsMutex);
#ifdef _WIN32
    if (!g_ws) return false;
    DWORD rc = WinHttpWebSocketSend(g_ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                    (PVOID)s.data(), (DWORD)s.size());
    return rc == 0;
#else
    if (!g_ssl) return false;
    return wsSendFrame(0x1, s);   // text frame
#endif
}
void sendResponse(const std::string& requestId, const json& payload) {
    wsSend(json{{"type","response"},{"requestId",requestId},{"payload",payload}});
}
void sendEvent(const std::string& type, json data) {
    if (!g_connected) return;
    if (!data.is_object()) data = json::object();
    data["type"] = type;
    wsSend(json{{"type","gameEvent"},{"payload",{{"type",type},{"data",data}}}});
    logmsg("Event -> hub: " + type);
}
void sendLog(const std::string& msg) {
    if (!g_connected) { logmsg("(log queued, not yet connected) " + msg); return; }
    wsSend(json{{"type","gameEvent"},{"payload",{{"type","log"},{"data",{{"type","log"},{"msg",msg}}}}}});
    logmsg("Event -> hub: log :: " + msg);
}

// ─── File IPC: ask the Lua profile to perform a game action ───────────────────────
// Single-file mailbox: write ipc/req.json = {id, action, args}, wait for ipc/res.json
// carrying the SAME id (up to REQ_TIMEOUT_MS). We can use one file because gameAction is
// serialized (g_actionMutex) and only ever called from the single WS-receive loop — never
// more than one request in flight. This uses only the io.open fs path that works in UE4SS
// Lua; the old ipc/req/<id>.json + `dir /B` listing never ran (io.popen is a no-op there).
json gameAction(const std::string& action, const json& args) {
    std::lock_guard<std::mutex> lk(g_actionMutex);
    unsigned long long id = g_reqSeq.fetch_add(1);
    char idbuf[32]; _snprintf(idbuf, sizeof(idbuf), "%llu", id);
    std::string idstr = idbuf;

    DeleteFileA(g_resFile.c_str());   // clear any stale result before posting the new request
    writeFileAtomic(g_reqFile, json{{"id",idstr},{"action",action},{"args",args}}.dump());

    int waited = 0;
    while (waited < REQ_TIMEOUT_MS && g_running) {
        std::string body;
        if (readFile(g_resFile, body) && !body.empty()) {
            json r;
            try { r = json::parse(body); } catch (...) { r = json(); }
            if (r.is_object() && r.value("id", std::string()) == idstr) {  // ignore a stale result
                DeleteFileA(g_resFile.c_str());
                return r;
            }
        }
        Sleep(50); waited += 50;
    }
    DeleteFileA(g_reqFile.c_str());   // give up; drop the request
    return json();                    // null = timeout
}

// ─── Request handling (Takaro → us) ───────────────────────────────────────────────
// Actions we answer directly in the core; everything else is forwarded to the Lua.
bool isCoreAction(const std::string& a) {
    return a == "testReachability" || a == "getPlayers" || a == "getServerInfo";
}

// Null-safe string read. Takaro modules routinely send explicit JSON null for optional args
// (e.g. {"gameId": null}); nlohmann's value(key, default) THROWS on a present-but-null key,
// and that throw is swallowed upstream so no response is ever sent and Takaro waits out the
// full timeout. jstr() treats null/missing/non-string all as the default.
static std::string jstr(const json& j, const char* key, const std::string& def = "") {
    if (!j.is_object()) return def;
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return def;
    return it->get<std::string>();
}

// ─── Astroneer: roster + actions over its RCON ───────────────────────────────────
std::atomic<bool> g_rosterInit{false};   // first roster read sets the baseline without join events

static json playerJson(const PlayerInfo& p) {
    json e{{"gameId",p.gameId},{"name",p.name}};
    if (!p.steamId.empty())              e["steamId"]              = p.steamId;
    if (!p.epicOnlineServicesId.empty()) e["epicOnlineServicesId"] = p.epicOnlineServicesId;
    if (!p.platformId.empty())           e["platformId"]           = p.platformId;
    return e;
}

// Online players from DSListPlayers; diff against the last read -> join/leave events.
static void astroPollRoster() {
    json list;
    if (!astroListPlayers(list)) return;
    std::map<std::string, PlayerInfo> now;
    for (auto& p : list) {
        if (!p.is_object() || !p.value("inGame", false)) continue;
        PlayerInfo pi;
        pi.gameId = astroGuid(p);
        pi.name   = jstr(p, "playerName");
        if (pi.gameId.empty() || pi.name.empty()) continue;   // still joining (no name yet)
        pi.platformId = "astroneer:" + pi.gameId;
        now[pi.gameId] = pi;
    }
    std::vector<PlayerInfo> joined, left;
    {
        std::lock_guard<std::mutex> lk(g_cacheMutex);
        if (g_rosterInit) {
            for (auto& kv : now)       if (!g_players.count(kv.first)) joined.push_back(kv.second);
            for (auto& kv : g_players) if (!now.count(kv.first))       left.push_back(kv.second);
        }
        g_players = now;
        g_rosterInit = true;
    }
    for (auto& p : joined) sendEvent("player-connected",    json{{"player", playerJson(p)}});
    for (auto& p : left)   sendEvent("player-disconnected", json{{"player", playerJson(p)}});
}

static std::string lowerStr(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}
static std::string targetGameId(const json& args) {
    std::string g = jstr(args, "gameId");
    if (g.empty() && args.is_object() && args.contains("player")) g = jstr(args["player"], "gameId");
    return g;
}
// A reply with "status": false is a refusal; anything else (incl. no reply) counts as done.
static bool astroOk(const std::string& out) {
    try {
        json j = json::parse(out);
        if (j.is_object() && j.contains("status") && j["status"].is_boolean()) return j["status"].get<bool>();
    } catch (...) {}
    return true;
}
static std::string pretty(const std::string& out) {
    if (out.empty()) return "Command executed (the server sends no reply for this command)";
    try { return json::parse(out).dump(2); } catch (...) { return out; }
}

// Category by name is the reliable path on Astroneer; the GUID variant is the fallback.
static bool astroSetCategory(const std::string& gid, const std::string& cat, std::string& out) {
    json list; std::string name;
    if (astroListPlayers(list))
        for (auto& p : list) if (astroGuid(p) == gid) { name = jstr(p, "playerName"); break; }
    bool ok = !name.empty()
        ? astroExec("DSSetPlayerCategoryForPlayerName \"" + name + "\" " + cat, out)
        : astroExec("DSSetPlayerCategoryGuid " + gid + " " + cat, out);
    return ok && astroOk(out);
}

// DSServerShutdown saves and exits; if the process is still up 20 s later, end it so the
// restart loop can bring it back (the old bridge force-killed it the same way).
static void astroShutdownAsync() {
    std::thread([] {
        std::string o;
        astroExec("DSServerShutdown", o);
        Sleep(20000);
#ifdef _WIN32
        TerminateProcess(GetCurrentProcess(), 0);
#else
        _exit(0);
#endif
    }).detach();
}

static const char* ASTRO_HELP = R"(ASTRONEER SERVER COMMANDS (type them without the "DS" prefix; it is added for you)

PLAYERS
  ListPlayers                                   every known player: GUID, name, category, online
  kick <playerName>                             kick by name (looked up to the GUID)
  KickPlayerGuid <playerGuid>                   kick by GUID
  SetPlayerCategoryForPlayerName "<name>" <cat> set a role by name (the reliable way)
  SetPlayerCategoryGuid <playerGuid> <cat>      set a role by GUID
     categories: Owner, Admin, Whitelisted, Unlisted, Blacklisted (= banned)

SERVER
  ServerStatistics                              build, FPS, players, active save, password/whitelist
  ServerShutdown                                save and shut down (restarts if a restart loop runs it)
  SetPassword <password>                        set the join password (empty = none)
  SetDenyUnlisted <true|false>                  whitelist on/off

SAVES
  ListGames                                     active save + every save on the server
  SaveGame [name]                               save now (optionally under a new name)
  LoadGame <saveName>                           load a save and make it active
  NewGame <saveName> [true|false]               start a new save (players reload); true = Glitchwalkers
  RenameGame <oldName> <newName>                rename a save
  DeleteGame <saveName>                         delete a save (permanent)

CREATIVE (current save)
  CreativeMode <true|false>, SetFuelFreeCreative, SetOxygenFreeCreative, SetInvincibleCreative,
  SetInvisibleToHazardsCreative, SetBackpackPowerUnlimitedCreative <true|false>
)";

// Returns true when the action was answered here.
static bool handleAstroRequest(const std::string& requestId, const std::string& action, const json& args) {
    if (action == "testReachability") {
        std::string o;
        bool ok = astroExec("DSServerStatistics", o) && json::accept(o);
        json r{{"connectable", ok}};
        r["reason"] = ok ? json(nullptr) : json(o.empty() ? std::string("no reply from the Astroneer console") : o);
        sendResponse(requestId, r);
        return true;
    }
    if (action == "getPlayers" || action == "getPlayer") {
        if (!g_rosterInit) astroPollRoster();
        return false;   // answered from the roster cache below
    }
    if (action == "getServerInfo") {
        std::string o; std::string version = "unknown";
        if (astroExec("DSServerStatistics", o)) {
            try { json s = json::parse(o); if (s.contains("build") && s["build"].is_string()) version = s["build"]; } catch (...) {}
        }
        sendResponse(requestId, json{{"name", IDENTITY_TOKEN.empty() ? "Astroneer Server" : IDENTITY_TOKEN},
                                     {"version", version}});
        return true;
    }
    if (action == "executeConsoleCommand") {
        std::string cmd = trim(jstr(args, "command"));
        if (cmd.empty()) cmd = trim(jstr(args, "rawCommand"));
        if (cmd.empty()) { sendResponse(requestId, json{{"success",false},{"rawResult","Error: No command provided"}}); return true; }
        std::string lc = lowerStr(cmd);
        if (lc == "help") { sendResponse(requestId, json{{"success",true},{"rawResult",ASTRO_HELP}}); return true; }
        if (lc.find("servershutdown") != std::string::npos) {
            sendResponse(requestId, json{{"success",true},{"rawResult","Server shutdown initiated"}});
            astroShutdownAsync();
            return true;
        }
        // kick <name>: Astroneer only kicks by GUID, so look the name up first.
        if (lc.compare(0, 5, "kick ") == 0) {
            std::string target = trim(cmd.substr(5));
            if (target.size() > 1 && target.front() == '"' && target.back() == '"') target = target.substr(1, target.size() - 2);
            std::string gid;
            bool numeric = !target.empty() && std::all_of(target.begin(), target.end(), ::isdigit);
            if (numeric) gid = target;
            else {
                json list; astroListPlayers(list);
                for (auto& p : list) if (lowerStr(jstr(p, "playerName")) == lowerStr(target)) { gid = astroGuid(p); target = jstr(p, "playerName"); break; }
            }
            if (gid.empty()) { sendResponse(requestId, json{{"success",false},{"rawResult","Error: Player \"" + target + "\" not found"}}); return true; }
            std::string o; bool ok = astroExec("DSKickPlayerGuid " + gid, o) && astroOk(o);
            sendResponse(requestId, json{{"success",ok},{"rawResult", ok ? "Kicked " + target + " (" + gid + ")" : pretty(o)}});
            return true;
        }
        // NewGame <name>: since 1.45 the game also needs isGlitchwalkersGame; default it to false
        // (a call without it is rejected and leaves the server in a state that later crashed it).
        {
            std::string n = lc.compare(0, 2, "ds") == 0 ? lc.substr(2) : lc;
            if (n.compare(0, 8, "newgame ") == 0) {
                std::string a = trim(cmd.substr(cmd.size() - (n.size() - 8)));
                if (!a.empty() && a.find(' ') == std::string::npos) cmd += " false";
            }
        }
        std::string o; bool ok = astroExec(cmd, o);
        sendResponse(requestId, json{{"success", ok && astroOk(o)}, {"rawResult", ok ? pretty(o) : "Error: " + o}});
        return true;
    }
    if (action == "kickPlayer") {
        std::string gid = targetGameId(args), o;
        bool ok = !gid.empty() && astroExec("DSKickPlayerGuid " + gid, o) && astroOk(o);
        sendResponse(requestId, ok ? json{{"success",true}} : json{{"success",false},{"error", gid.empty() ? "No player specified" : o}});
        return true;
    }
    if (action == "banPlayer" || action == "unbanPlayer") {
        std::string gid = targetGameId(args), o;
        bool ban = action == "banPlayer";
        bool ok = !gid.empty() && astroSetCategory(gid, ban ? "Blacklisted" : "Unlisted", o);
        if (ok && ban) {
            bool online;
            { std::lock_guard<std::mutex> lk(g_cacheMutex); online = g_players.count(gid) > 0; }
            if (online) { std::string k; astroExec("DSKickPlayerGuid " + gid, k); }
        }
        sendResponse(requestId, ok ? json{{"success",true}} : json{{"success",false},{"error", gid.empty() ? "No player specified" : o}});
        return true;
    }
    if (action == "listBans") {
        json list, bans = json::array();
        astroListPlayers(list);
        for (auto& p : list)
            if (jstr(p, "playerCategory") == "Blacklisted")
                bans.push_back(json{{"player", {{"gameId", astroGuid(p)}, {"name", jstr(p, "playerName")}}},
                                    {"reason", ""}, {"expiresAt", nullptr}});
        sendResponse(requestId, bans);
        return true;
    }
    if (action == "shutdown") {
        sendResponse(requestId, json{{"success",true}});
        astroShutdownAsync();
        return true;
    }
    if (action == "getPlayerInventory") { sendResponse(requestId, json::array()); return true; }
    if (action == "sendMessage" || action == "getPlayerLocation" || action == "giveItem" || action == "teleportPlayer") {
        sendResponse(requestId, json{{"success",false},{"error","Astroneer's server console has no " + action}});
        return true;
    }
    return false;
}

void handleRequest(const std::string& requestId, const json& payload) {
    std::string action = jstr(payload, "action");
    json args = json::object();
    if (payload.contains("args")) {
        const json& ra = payload["args"];
        if (ra.is_string()) { try { args = json::parse(ra.get<std::string>()); } catch (...) {} }
        else if (ra.is_object()) args = ra;
    }
    logmsg("Request: " + action + " (" + requestId + ")");

    if (g_astro && handleAstroRequest(requestId, action, args)) return;

    if (action == "testReachability") { sendResponse(requestId, json{{"connectable",true}}); return; }

    if (action == "getPlayers") {
        json list = json::array();
        std::lock_guard<std::mutex> lk(g_cacheMutex);
        for (auto& kv : g_players) {
            const PlayerInfo& p = kv.second;
            json e{{"gameId",p.gameId},{"name",p.name}};
            if (!p.steamId.empty())              e["steamId"]              = p.steamId;
            if (!p.epicOnlineServicesId.empty()) e["epicOnlineServicesId"] = p.epicOnlineServicesId;
            if (!p.platformId.empty())           e["platformId"]           = p.platformId;
            list.push_back(e);
        }
        sendResponse(requestId, list);
        return;
    }
    if (action == "getPlayer") {
        // Single player by game id, answered from the same roster cache as getPlayers
        // (Takaro's Generic connector otherwise gets "Action not implemented in profile").
        std::string gid = jstr(args, "gameId");
        if (gid.empty() && args.contains("player")) gid = jstr(args["player"], "gameId");
        std::lock_guard<std::mutex> lk(g_cacheMutex);
        auto it = g_players.find(gid);
        if (it == g_players.end()) { sendResponse(requestId, json{{"success",false},{"error","player not found"}}); return; }
        const PlayerInfo& p = it->second;
        json e{{"gameId",p.gameId},{"name",p.name}};
        if (!p.steamId.empty())              e["steamId"]              = p.steamId;
        if (!p.epicOnlineServicesId.empty()) e["epicOnlineServicesId"] = p.epicOnlineServicesId;
        if (!p.platformId.empty())           e["platformId"]           = p.platformId;
        sendResponse(requestId, e);
        return;
    }
    if (action == "getServerInfo") {
        sendResponse(requestId, json{{"name", IDENTITY_TOKEN.empty() ? "Unreal Server" : IDENTITY_TOKEN},
                                     {"version","unknown"}});
        return;
    }

    // Catalog/list actions Takaro validates as ARRAYS. A generic UE server exposes no
    // entity/item/ban/location catalog, so the correct (and Takaro-valid) answer is an
    // empty array — NOT the {success,error} object the Lua "not implemented" path returns
    // (that produced "Expected array … but got object"). Answered here so it never falls
    // through to Lua. (A future game needing a real catalog would add its own core path.)
    if (action == "listEntities" || action == "listItems" ||
        action == "listBans"     || action == "listLocations") {
        sendResponse(requestId, json::array());
        return;
    }
    // Takaro console -> executeConsoleCommand. If RCON is configured, run it against the
    // game's own RCON (that's where the real command surface lives); otherwise fall
    // through to the Lua UE-console path.
    if (action == "executeConsoleCommand" && RCON_PORT > 0) {
        std::string cmd = jstr(args, "command");
        if (cmd.empty()) cmd = jstr(args, "rawCommand");
        std::string out; bool ok = rconExec(cmd, out);
        sendResponse(requestId, json{{"success", ok}, {"rawResult", out}});
        return;
    }
    // shutdown via RCON only when the operator maps a shutdown command (RCON_SHUTDOWN_CMD);
    // otherwise it falls through to the Lua builtin (UE console "quit").
    if (action == "shutdown" && RCON_PORT > 0 && !RCON_SHUTDOWN_CMD.empty()) {
        std::string out; bool ok = rconExec(RCON_SHUTDOWN_CMD, out);
        sendResponse(requestId, json{{"success", ok}, {"rawResult", out}});
        return;
    }

    // Forward every other action to the Lua profile over file IPC.
    json res = gameAction(action, args);
    bool handled = res.is_object() && res.contains("result") && !res["result"].is_null();
    // Lua returns {success, result?, error?}. Prefer an explicit result payload — this is
    // where a profile's real getPlayerLocation/getPlayerInventory/giveItem/etc. is answered.
    if (handled) { sendResponse(requestId, res["result"]); return; }

    // Profile has NO handler for this action. getPlayerInventory is polled every sync cycle
    // and Takaro validates it as an ARRAY, so answer the schema-valid empty inventory (keeps
    // the sync clean). For anything else — including getPlayerLocation — return an honest
    // error rather than fabricated data (do NOT report every player at 0,0,0, which corrupts
    // player tracking / teleport). This only runs when the active profile lacks the handler,
    // so profile handlers are never shadowed.
    if (action == "getPlayerInventory") { sendResponse(requestId, json::array()); return; }
    if (res.is_object()) { sendResponse(requestId, res); return; }
    sendResponse(requestId, json{{"success",false},{"error","No result from game (timeout or action unsupported)"}});
}

// ─── Poll loop (drain events + refresh roster) ────────────────────────────────────
void refreshPlayers() {
    std::string body;
    if (!readFile(g_playersPath, body) || body.empty()) return;
    json arr;
    try { arr = json::parse(body); } catch (...) { return; }
    if (!arr.is_array()) return;
    std::lock_guard<std::mutex> lk(g_cacheMutex);
    g_players.clear();
    for (auto& p : arr) {
        if (!p.is_object()) continue;
        PlayerInfo pi;
        pi.gameId               = p.value("gameId", "");
        pi.name                 = p.value("name", "");
        pi.steamId              = toSteamId64(p.value("steamId", ""));
        pi.epicOnlineServicesId = p.value("epicOnlineServicesId", "");
        pi.platformId           = p.value("platformId", "");
        if (pi.platformId.empty() && !pi.steamId.empty()) pi.platformId = "steam:" + pi.steamId;
        if (!pi.gameId.empty()) g_players[pi.gameId] = pi;
    }
}

void drainEvents() {
    for (const std::string& name : listJson(g_evtDir)) {
        std::string path = g_evtDir + "\\" + name, body;
        if (!readFile(path, body)) { DeleteFileA(path.c_str()); continue; }
        DeleteFileA(path.c_str());
        json ev;
        try { ev = json::parse(body); } catch (...) { continue; }
        if (!ev.is_object() || !ev.contains("type")) continue;
        std::string type = ev["type"].get<std::string>();
        json data = ev.contains("data") && ev["data"].is_object() ? ev["data"] : json::object();
        // normalise a player's steamId to steam64 if the profile handed us a raw acct id
        if (data.contains("player") && data["player"].is_object() && data["player"].contains("steamId"))
            data["player"]["steamId"] = toSteamId64(data["player"]["steamId"].get<std::string>());
        if (type == "log") { sendLog(data.value("msg", "")); continue; }
        // Astroneer: join/leave come from the RCON roster, not the Lua PlayerState diff.
        if (g_astro && (type == "player-connected" || type == "player-disconnected")) continue;
        sendEvent(type, data);
    }
}

void pollLoop() {
    int sinceRoster = ROSTER_POLL_MS;
    while (g_running) {
        if (g_astro) {
            sinceRoster += POLL_INTERVAL_MS;
            if (sinceRoster >= ROSTER_POLL_MS) { sinceRoster = 0; astroPollRoster(); }
        }
        if (g_connected) {
            if (!g_startupSent) { g_startupSent = true; sendLog(SERVER_STARTED_MSG); }
            if (!g_astro) refreshPlayers();
            drainEvents();
        }
        Sleep(POLL_INTERVAL_MS);
    }
}

// ─── WebSocket connect + receive loop ─────────────────────────────────────────────
void handleMessage(const json& msg) {
    std::string type = msg.value("type", "");
    if (type == "identifyResponse") {
        if (msg.contains("payload") && msg["payload"].is_object() && msg["payload"].contains("error"))
            logmsg("Identify failed: " + msg["payload"]["error"].dump());
        else { logmsg("Identified with hub " + g_wsHost + ":" + std::to_string(g_wsPort)); g_connected = true; }
    } else if (type == "connected") {
        logmsg("Hub confirmed connection");
    } else if (type == "ping") {
        wsSend(json{{"type","pong"}});
    } else if (type == "request") {
        handleRequest(msg.value("requestId", ""), msg.contains("payload") ? msg["payload"] : json::object());
    } else if (type == "error") {
        logmsg("Hub error: " + (msg.contains("payload") ? msg["payload"].dump() : msg.dump()));
    } else if (type != "response") {
        logmsg("Unknown msg: " + type);
    }
}

// Returns false when the connection is closed/failed (caller reconnects).
#ifdef _WIN32
bool wsRunOnce() {
    g_wsSession = WinHttpOpen(L"ServerManagerTakaro", WINHTTP_ACCESS_TYPE_NO_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!g_wsSession) return false;
    std::wstring whost(g_wsHost.begin(), g_wsHost.end()), wpath(g_wsPath.begin(), g_wsPath.end());
    g_wsConn = WinHttpConnect(g_wsSession, whost.c_str(), (INTERNET_PORT)g_wsPort, 0);
    if (!g_wsConn) { WinHttpCloseHandle(g_wsSession); g_wsSession = NULL; return false; }

    HINTERNET req = WinHttpOpenRequest(g_wsConn, L"GET", wpath.c_str(), NULL, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       g_wsSecure ? WINHTTP_FLAG_SECURE : 0);
    bool ok = req &&
        WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0) &&
        WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0) &&
        WinHttpReceiveResponse(req, NULL);
    HINTERNET ws = ok ? WinHttpWebSocketCompleteUpgrade(req, 0) : NULL;
    if (req) WinHttpCloseHandle(req);
    if (!ws) {
        if (g_wsConn) WinHttpCloseHandle(g_wsConn); g_wsConn = NULL;
        if (g_wsSession) WinHttpCloseHandle(g_wsSession); g_wsSession = NULL;
        return false;
    }
    { std::lock_guard<std::mutex> lk(g_wsMutex); g_ws = ws; }

    logmsg("Connected. Identifying as \"" + IDENTITY_TOKEN + "\"");
    json idp{{"identityToken", IDENTITY_TOKEN}};
    if (!REGISTRATION_TOKEN.empty()) idp["registrationToken"] = REGISTRATION_TOKEN;
    wsSend(json{{"type","identify"},{"payload",idp}});

    std::string acc;
    BYTE buf[8192];
    while (g_running) {
        DWORD got = 0; WINHTTP_WEB_SOCKET_BUFFER_TYPE bt;
        DWORD rc = WinHttpWebSocketReceive(ws, buf, sizeof(buf), &got, &bt);
        if (rc != 0) break;
        if (bt == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
        acc.append((char*)buf, got);
        if (bt == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || bt == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
            try { handleMessage(json::parse(acc)); } catch (...) { logmsg("Parse error on WS message"); }
            acc.clear();
        }
        // FRAGMENT buffer types keep accumulating in acc
    }

    g_connected = false;
    { std::lock_guard<std::mutex> lk(g_wsMutex);
      WinHttpWebSocketClose(ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0);
      WinHttpCloseHandle(ws); g_ws = NULL; }
    if (g_wsConn) { WinHttpCloseHandle(g_wsConn); g_wsConn = NULL; }
    if (g_wsSession) { WinHttpCloseHandle(g_wsSession); g_wsSession = NULL; }
    return false;
}
#else // ── Linux: TLS + RFC6455 over OpenSSL ──────────────────────────────────────

static int tcpConnect(const char* host, int port) {
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_socktype = SOCK_STREAM; hints.ai_family = AF_UNSPEC;
    char portstr[16]; snprintf(portstr, sizeof(portstr), "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0) return -1;
    int fd = -1;
    for (auto p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        ::close(fd); fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

bool wsRunOnce() {
    // TLS only on Linux: a plain ws:// WS_URL still gets a TLS handshake here.
    const char* host = g_wsHost.c_str();
    int fd = tcpConnect(host, g_wsPort);
    if (fd < 0) { logmsg("WS: TCP connect failed"); return false; }

    g_sslCtx = SSL_CTX_new(TLS_client_method());
    if (!g_sslCtx) { ::close(fd); return false; }
    SSL_CTX_set_default_verify_paths(g_sslCtx);
    SSL* ssl = SSL_new(g_sslCtx);
    SSL_set_fd(ssl, fd);
    SSL_set_tlsext_host_name(ssl, host);                      // SNI
#ifndef TAKARO_TLS_INSECURE
    // Verify the server cert + hostname (the WS host is fixed; this blocks MITM). The
    // target container needs ca-certificates; if it lacks them, build with -DTAKARO_TLS_INSECURE.
    SSL_set_verify(ssl, SSL_VERIFY_PEER, nullptr);
    X509_VERIFY_PARAM_set1_host(SSL_get0_param(ssl), host, 0);
#endif
    if (SSL_connect(ssl) != 1) {
        logmsg("WS: TLS handshake failed");
        SSL_free(ssl); SSL_CTX_free(g_sslCtx); g_sslCtx = nullptr; ::close(fd); return false;
    }
    { std::lock_guard<std::mutex> lk(g_wsMutex); g_ssl = ssl; g_wsSock = fd; }

    // WebSocket upgrade handshake (fixed RFC sample key; the server echoes an Accept we
    // don't need to validate for a client). Read the response headers byte-by-byte so we
    // never consume bytes belonging to the first data frame.
    std::string req =
        std::string("GET ") + g_wsPath + " HTTP/1.1\r\nHost: " + host + "\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
    if (!sslWriteAll(req.data(), req.size())) { logmsg("WS: upgrade send failed"); }
    std::string hdr; char c;
    while (hdr.size() < 8192) {
        if (!sslReadN(&c, 1)) { hdr.clear(); break; }
        hdr.push_back(c);
        if (hdr.size() >= 4 && hdr.compare(hdr.size() - 4, 4, "\r\n\r\n") == 0) break;
    }
    bool upgraded = hdr.find(" 101 ") != std::string::npos;
    if (!upgraded) {
        logmsg("WS: upgrade rejected: " + (hdr.empty() ? std::string("no response") : hdr.substr(0, hdr.find("\r\n"))));
        g_connected = false;
        std::lock_guard<std::mutex> lk(g_wsMutex);
        SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(g_sslCtx); g_sslCtx = nullptr;
        ::close(fd); g_ssl = nullptr; g_wsSock = -1;
        return false;
    }

    logmsg("Connected. Identifying as \"" + IDENTITY_TOKEN + "\"");
    json idp{{"identityToken", IDENTITY_TOKEN}};
    if (!REGISTRATION_TOKEN.empty()) idp["registrationToken"] = REGISTRATION_TOKEN;
    wsSend(json{{"type","identify"},{"payload",idp}});

    while (g_running) {
        std::string m;
        if (!wsRecvMessage(m)) break;
        if (m.empty()) continue;
        try { handleMessage(json::parse(m)); } catch (...) { logmsg("Parse error on WS message"); }
    }

    g_connected = false;
    { std::lock_guard<std::mutex> lk(g_wsMutex);
      wsSendFrame(0x8, "");                                  // polite close
      SSL_shutdown(ssl); SSL_free(ssl);
      SSL_CTX_free(g_sslCtx); g_sslCtx = nullptr;
      ::close(fd); g_ssl = nullptr; g_wsSock = -1; }
    return false;
}
#endif // _WIN32

void wsLoop() {
    int delay = 3000;
    while (g_running) {
        wsRunOnce();
        if (!g_running) break;
        logmsg("Disconnected. Reconnecting in " + std::to_string(delay / 1000) + "s");
        Sleep(delay);
        delay = (delay * 2 > 60000) ? 60000 : delay * 2;
        if (g_connected) delay = 3000;   // reset backoff after a good connection
    }
}

void coreBoot() {
    // Single-instance guard: if the game imports more than one of our proxy DLLs
    // (winmm / version / xinput) they load into the SAME process and would each boot the
    // core. Each DLL has its own copy of this static (separate images), so the static bool
    // alone can't coordinate across them — we need a shared kernel object. Scope it to THIS
    // process id (not a machine-global "Global\\..." name), so two proxies in one process
    // dedupe while a host running two Unreal servers still gets BOTH cores. (The old
    // machine-global mutex silently killed the second server's core.)
    static bool s_booted = false;
    if (s_booted) return;
#ifdef _WIN32
    std::string mtxName = "Local\\ServerManagerTakaroCore-" + std::to_string(GetCurrentProcessId());
    CreateMutexA(NULL, FALSE, mtxName.c_str());
    if (GetLastError() == ERROR_ALREADY_EXISTS) return;   // another proxy DLL in this process booted first
#endif
    // On Linux only one core .so is LD_PRELOAD'd per process, so the static bool is enough.
    s_booted = true;

    computePaths();
    loadConfig();
    if (!ENABLED) { logmsg("TakaroConnector disabled (ENABLED=false)"); return; }
#ifdef _WIN32
    {
        char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH);
        if (RCON_PROTOCOL == "astroneer" || strstr(exe, "AstroServer")) g_astro = true;
    }
#else
    if (RCON_PROTOCOL == "astroneer") g_astro = true;
#endif
    logmsg("ServerManager-Takaro core (in-DLL) started — mod=" + g_modDir);
    std::thread(wsLoop).detach();
    std::thread(pollLoop).detach();
}

} // namespace

// Called once from winmm.dll's launcher thread (NOT from DllMain directly).
extern "C" void StartTakaroCore() {
    coreBoot();
}

#ifndef _WIN32
// Linux delivery: this core is a small .so LD_PRELOAD'd into the UE dedicated-server
// process alongside native UE4SS-Linux (which loads the shared Lua profile unchanged).
// The constructor runs at load; it hands off to a detached thread so no real work runs
// inside the dynamic linker, and gives UE4SS a moment to lay down the mod/ipc tree.
__attribute__((constructor))
static void takaro_linux_boot() {
    std::thread([] { SleepMs(2000); StartTakaroCore(); }).detach();
}
#endif
