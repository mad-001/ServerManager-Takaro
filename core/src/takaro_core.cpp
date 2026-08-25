// takaro_core.cpp — ServerManager-Takaro universal core, embedded in winmm.dll.
//
// One in-process core, identical for every Unreal Engine game. It owns:
//   • a WinHTTP secure WebSocket straight to wss://connect.takaro.io/ (TLS + framing
//     done by the OS — no OpenSSL, no Node, no separate bridge process), and
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

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>       // must precede windows.h; used by the in-DLL RCON client
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>

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

// Takaro websocket host. Hardcoded on purpose -- deliberately NOT readable from
// TakaroConfig.txt, so a shipped build can never be pointed somewhere else by an
// end user. Internal test builds override it at compile time:
//     -DTAKARO_WS_HOST=L"connect.k8s.takaro.dev"
#ifndef TAKARO_WS_HOST
#define TAKARO_WS_HOST L"connect.takaro.io"
#endif
// Port and TLS, same rationale as the host: overridable only at build time so a
// shipped DLL can never be pointed at a plaintext local endpoint.
#ifndef TAKARO_WS_PORT
#define TAKARO_WS_PORT 443
#endif
#ifndef TAKARO_WS_SECURE
#define TAKARO_WS_SECURE 1
#endif

std::string g_gameDir, g_modDir, g_ipcDir, g_configPath, g_logPath;
std::string g_evtDir, g_reqDir, g_resDir, g_playersPath;
std::string g_reqFile, g_resFile;   // single-file action mailbox (io.open-only, see main.lua)

// ─── State ─────────────────────────────────────────────────────────────────────
std::atomic<bool> g_running{true};
std::atomic<bool> g_connected{false};
std::atomic<bool> g_startupSent{false};

HINTERNET g_wsSession = NULL, g_wsConn = NULL, g_ws = NULL;
std::mutex g_wsMutex;      // guards g_ws + sends
std::mutex g_logMutex;
std::mutex g_cacheMutex;
std::mutex g_actionMutex;  // serializes gameAction (one mailbox request at a time)
std::atomic<unsigned long long> g_reqSeq{1};

struct PlayerInfo { std::string gameId, name, steamId, platformId; };
std::map<std::string, PlayerInfo> g_players;   // by gameId

const uint64_t STEAM64_BASE = 76561197960265728ULL;

// ─── Logging ───────────────────────────────────────────────────────────────────
std::string nowStamp() {
    SYSTEMTIME st; GetSystemTime(&st);
    char b[40];
    _snprintf(b, sizeof(b), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
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
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
void ensureDir(const std::string& p) { CreateDirectoryA(p.c_str(), NULL); }

// The game exe dir holds winmm.dll + ue4ss\. The TakaroConnector mod may live under a
// couple of standard UE4SS mods roots — probe them so one DLL fits every layout.
void computePaths() {
    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH);
    char* sl = strrchr(exe, '\\'); if (sl) *sl = '\0';
    g_gameDir = exe;
    const char* roots[] = { "\\ue4ss\\Mods", "\\Mods", "\\ue4ss\\mods" };
    g_modDir = g_gameDir + "\\ue4ss\\Mods\\TakaroConnector";   // default
    for (const char* r : roots) {
        std::string cand = g_gameDir + r + "\\TakaroConnector";
        if (dirExists(cand)) { g_modDir = cand; break; }
    }
    g_configPath  = g_modDir + "\\TakaroConfig.txt";
    g_logPath     = g_modDir + "\\core.log";
    g_ipcDir      = g_modDir + "\\ipc";
    g_evtDir      = g_ipcDir + "\\evt";
    g_reqDir      = g_ipcDir + "\\req";
    g_resDir      = g_ipcDir + "\\res";
    g_reqFile     = g_ipcDir + "\\req.json";   // single-file mailbox (dir listing is dead in UE4SS Lua)
    g_resFile     = g_ipcDir + "\\res.json";
    g_playersPath = g_ipcDir + "\\players.json";
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
    MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    return true;
}
// List *.json files in dir, sorted ascending by name (so numeric seq is chronological).
std::vector<std::string> listJson(const std::string& dir) {
    std::vector<std::string> names;
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

    if (!rcon::g_wsaInit) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { out = "WSAStartup failed"; return false; }
        rcon::g_wsaInit = true;
    }

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

    DWORD tmo = 4000;                                    // connect/auth timeout
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tmo, sizeof(tmo));
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

// ─── WebSocket send ──────────────────────────────────────────────────────────────
bool wsSend(const json& obj) {
    std::string s = obj.dump();
    std::lock_guard<std::mutex> lk(g_wsMutex);
    if (!g_ws) return false;
    DWORD rc = WinHttpWebSocketSend(g_ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                    (PVOID)s.data(), (DWORD)s.size());
    return rc == 0;
}
void sendResponse(const std::string& requestId, const json& payload) {
    wsSend(json{{"type","response"},{"requestId",requestId},{"payload",payload}});
}
void sendEvent(const std::string& type, json data) {
    if (!g_connected) return;
    if (!data.is_object()) data = json::object();
    data["type"] = type;
    wsSend(json{{"type","gameEvent"},{"payload",{{"type",type},{"data",data}}}});
    logmsg("Event -> Takaro: " + type);
}
void sendLog(const std::string& msg) {
    if (!g_connected) { logmsg("(log queued, not yet connected) " + msg); return; }
    wsSend(json{{"type","gameEvent"},{"payload",{{"type","log"},{"data",{{"type","log"},{"msg",msg}}}}}});
    logmsg("Event -> Takaro: log :: " + msg);
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

void handleRequest(const std::string& requestId, const json& payload) {
    std::string action = payload.value("action", "");
    json args = json::object();
    if (payload.contains("args")) {
        const json& ra = payload["args"];
        if (ra.is_string()) { try { args = json::parse(ra.get<std::string>()); } catch (...) {} }
        else if (ra.is_object()) args = ra;
    }
    logmsg("Request: " + action + " (" + requestId + ")");

    if (action == "testReachability") { sendResponse(requestId, json{{"connectable",true}}); return; }

    if (action == "getPlayers") {
        json list = json::array();
        std::lock_guard<std::mutex> lk(g_cacheMutex);
        for (auto& kv : g_players) {
            const PlayerInfo& p = kv.second;
            json e{{"gameId",p.gameId},{"name",p.name}};
            if (!p.steamId.empty())    e["steamId"]    = p.steamId;
            if (!p.platformId.empty()) e["platformId"] = p.platformId;
            list.push_back(e);
        }
        sendResponse(requestId, list);
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
        std::string cmd = args.value("command", "");
        if (cmd.empty()) cmd = args.value("rawCommand", "");
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
    if (res.is_object()) {
        // Lua returns {success, result?, error?}. Prefer an explicit result payload.
        if (res.contains("result") && !res["result"].is_null()) { sendResponse(requestId, res["result"]); return; }
        sendResponse(requestId, res);
    } else {
        sendResponse(requestId, json{{"success",false},{"error","No result from game (timeout or action unsupported)"}});
    }
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
        pi.gameId     = p.value("gameId", "");
        pi.name       = p.value("name", "");
        pi.steamId    = toSteamId64(p.value("steamId", ""));
        pi.platformId = p.value("platformId", "");
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
        sendEvent(type, data);
    }
}

void pollLoop() {
    while (g_running) {
        if (g_connected) {
            if (!g_startupSent) { g_startupSent = true; sendLog(SERVER_STARTED_MSG); }
            refreshPlayers();
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
        else { logmsg("Identified with Takaro"); g_connected = true; }
    } else if (type == "connected") {
        logmsg("Takaro confirmed connection");
    } else if (type == "ping") {
        wsSend(json{{"type","pong"}});
    } else if (type == "request") {
        handleRequest(msg.value("requestId", ""), msg.contains("payload") ? msg["payload"] : json::object());
    } else if (type == "error") {
        logmsg("Takaro error: " + (msg.contains("payload") ? msg["payload"].dump() : msg.dump()));
    } else if (type != "response") {
        logmsg("Unknown msg: " + type);
    }
}

// Returns false when the connection is closed/failed (caller reconnects).
bool wsRunOnce() {
    g_wsSession = WinHttpOpen(L"ServerManagerTakaro", WINHTTP_ACCESS_TYPE_NO_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!g_wsSession) return false;
    g_wsConn = WinHttpConnect(g_wsSession, TAKARO_WS_HOST, TAKARO_WS_PORT, 0);
    if (!g_wsConn) { WinHttpCloseHandle(g_wsSession); g_wsSession = NULL; return false; }

    HINTERNET req = WinHttpOpenRequest(g_wsConn, L"GET", L"/", NULL, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       TAKARO_WS_SECURE ? WINHTTP_FLAG_SECURE : 0);
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
    // (winmm / version / xinput), only the first to boot starts the core.
    static bool s_booted = false;
    CreateMutexA(NULL, FALSE, "Global\\ServerManagerTakaroCore");
    if (GetLastError() == ERROR_ALREADY_EXISTS || s_booted) return;
    s_booted = true;

    computePaths();
    loadConfig();
    if (!ENABLED) { logmsg("TakaroConnector disabled (ENABLED=false)"); return; }
    logmsg("ServerManager-Takaro core (in-DLL) started — mod=" + g_modDir);
    std::thread(wsLoop).detach();
    std::thread(pollLoop).detach();
}

} // namespace

// Called once from winmm.dll's launcher thread (NOT from DllMain directly).
extern "C" void StartTakaroCore() {
    coreBoot();
}
