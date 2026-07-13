// winmm.dll — ServerManager-Takaro universal in-DLL core (Unreal Engine / UE4SS).
//
// This single DLL does EVERYTHING — there is NO separate bridge process:
//   1. Proxies the real System32\winmm.dll (forwards every export at runtime).
//   2. Bootstraps UE4SS if present (LoadLibrary ue4ss\UE4SS.dll) so this one file
//      can also be UE4SS's loader.
//   3. Starts the Takaro core (takaro_core.cpp): a WinHTTP TLS WebSocket straight
//      to wss://connect.takaro.io/, and file-based IPC to the UE4SS Lua profile
//      that hooks the game's chat/death/join functions.
//
// Identical for every game. The only per-game piece is the Lua profile.
#define WIN32_LEAN_AND_MEAN
#define WINMMAPI   // strip dllimport from mmsystem.h declarations
#include <windows.h>
#include <mmsystem.h>
typedef void (CALLBACK* TASKCALLBACK)(DWORD_PTR);
typedef TASKCALLBACK* LPTASKCALLBACK;
#include <stdio.h>
#include <string.h>

static HMODULE g_real = NULL;

// ─── Load real winmm.dll ──────────────────────────────────────────────────────
static void LoadReal() {
    char sys[MAX_PATH];
    GetSystemDirectoryA(sys, MAX_PATH);
    char path[MAX_PATH];
    _snprintf(path, sizeof(path), "%s\\winmm.dll", sys);
    g_real = LoadLibraryA(path);
}

static void* RealFunc(const char* name) {
    return g_real ? (void*)GetProcAddress(g_real, name) : NULL;
}

// ─── Bootstrap UE4SS if it lives next to us ───────────────────────────────────
// RE-UE4SS's own proxy loaders do exactly this: LoadLibrary its UE4SS.dll, whose
// DllMain initialises the Lua runtime and loads the Mods. If UE4SS was already
// loaded by its own proxy (dwmapi/xinput), this just bumps the refcount (harmless).
static void LoadUE4SS() {
    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH);
    char* sl = strrchr(exe, '\\'); if (sl) *sl = '\0';
    // Common install locations relative to the game exe.
    const char* candidates[] = { "\\ue4ss\\UE4SS.dll", "\\UE4SS.dll", "\\ue4ss\\dwmapi.dll" };
    for (const char* c : candidates) {
        char path[MAX_PATH];
        _snprintf(path, sizeof(path), "%s%s", exe, c);
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) { LoadLibraryA(path); return; }
    }
}

// The Takaro core lives INSIDE this DLL (takaro_core.cpp) — no separate process.
extern "C" void StartTakaroCore();

// ─── Launcher (runs on a thread, safe outside DllMain) ────────────────────────
static DWORD WINAPI LaunchThread(LPVOID) {
    LoadUE4SS();         // make sure the Lua runtime + profile get loaded
    StartTakaroCore();   // reads config, opens WS + file-IPC worker threads, returns
    return 0;
}

// ─── DllMain ──────────────────────────────────────────────────────────────────
BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        LoadReal();
        HANDLE t = CreateThread(NULL, 0, LaunchThread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_real) { FreeLibrary(g_real); g_real = NULL; }
    }
    return TRUE;
}

// ─── Proxy stubs ──────────────────────────────────────────────────────────────
// Undefine macros that clash with our function names
#undef PlaySound
#undef mciSendString
#undef mciGetDeviceID

extern "C" {

// Timer (most critical — vtserver calls these)
DWORD   WINAPI timeGetTime()               { auto f=(DWORD(WINAPI*)())RealFunc("timeGetTime"); return f?f():0; }
MMRESULT WINAPI timeBeginPeriod(UINT u)    { auto f=(MMRESULT(WINAPI*)(UINT))RealFunc("timeBeginPeriod"); return f?f(u):TIMERR_NOCANDO; }
MMRESULT WINAPI timeEndPeriod(UINT u)      { auto f=(MMRESULT(WINAPI*)(UINT))RealFunc("timeEndPeriod"); return f?f(u):TIMERR_NOCANDO; }
MMRESULT WINAPI timeGetDevCaps(LPTIMECAPS p,UINT s) { auto f=(MMRESULT(WINAPI*)(LPTIMECAPS,UINT))RealFunc("timeGetDevCaps"); return f?f(p,s):TIMERR_NOCANDO; }
MMRESULT WINAPI timeGetSystemTime(MMTIME* p,UINT s) { auto f=(MMRESULT(WINAPI*)(MMTIME*,UINT))RealFunc("timeGetSystemTime"); return f?f(p,s):TIMERR_NOCANDO; }
MMRESULT WINAPI timeSetEvent(UINT d,UINT r,LPTIMECALLBACK cb,DWORD_PTR usr,UINT fl) { auto f=(MMRESULT(WINAPI*)(UINT,UINT,LPTIMECALLBACK,DWORD_PTR,UINT))RealFunc("timeSetEvent"); return f?f(d,r,cb,usr,fl):0; }
MMRESULT WINAPI timeKillEvent(UINT id)     { auto f=(MMRESULT(WINAPI*)(UINT))RealFunc("timeKillEvent"); return f?f(id):TIMERR_NOCANDO; }

// Sound
BOOL WINAPI PlaySoundA(LPCSTR p,HMODULE m,DWORD f)  { auto fn=(BOOL(WINAPI*)(LPCSTR,HMODULE,DWORD))RealFunc("PlaySoundA"); return fn?fn(p,m,f):FALSE; }
BOOL WINAPI PlaySoundW(LPCWSTR p,HMODULE m,DWORD f) { auto fn=(BOOL(WINAPI*)(LPCWSTR,HMODULE,DWORD))RealFunc("PlaySoundW"); return fn?fn(p,m,f):FALSE; }
BOOL WINAPI PlaySound(LPCWSTR p,HMODULE m,DWORD f)  { auto fn=(BOOL(WINAPI*)(LPCWSTR,HMODULE,DWORD))RealFunc("PlaySound"); return fn?fn(p,m,f):FALSE; }
BOOL WINAPI sndPlaySoundA(LPCSTR p,UINT f) { auto fn=(BOOL(WINAPI*)(LPCSTR,UINT))RealFunc("sndPlaySoundA"); return fn?fn(p,f):FALSE; }
BOOL WINAPI sndPlaySoundW(LPCWSTR p,UINT f){ auto fn=(BOOL(WINAPI*)(LPCWSTR,UINT))RealFunc("sndPlaySoundW"); return fn?fn(p,f):FALSE; }

// Wave out
UINT     WINAPI waveOutGetNumDevs()        { auto f=(UINT(WINAPI*)())RealFunc("waveOutGetNumDevs"); return f?f():0; }
MMRESULT WINAPI waveOutGetDevCapsA(UINT_PTR d,LPWAVEOUTCAPSA c,UINT s)  { auto f=(MMRESULT(WINAPI*)(UINT_PTR,LPWAVEOUTCAPSA,UINT))RealFunc("waveOutGetDevCapsA"); return f?f(d,c,s):MMSYSERR_NOTSUPPORTED; }
MMRESULT WINAPI waveOutGetDevCapsW(UINT_PTR d,LPWAVEOUTCAPSW c,UINT s)  { auto f=(MMRESULT(WINAPI*)(UINT_PTR,LPWAVEOUTCAPSW,UINT))RealFunc("waveOutGetDevCapsW"); return f?f(d,c,s):MMSYSERR_NOTSUPPORTED; }
MMRESULT WINAPI waveOutGetErrorTextA(MMRESULT e,LPSTR t,UINT s)  { auto f=(MMRESULT(WINAPI*)(MMRESULT,LPSTR,UINT))RealFunc("waveOutGetErrorTextA"); return f?f(e,t,s):MMSYSERR_NOTSUPPORTED; }
MMRESULT WINAPI waveOutGetErrorTextW(MMRESULT e,LPWSTR t,UINT s) { auto f=(MMRESULT(WINAPI*)(MMRESULT,LPWSTR,UINT))RealFunc("waveOutGetErrorTextW"); return f?f(e,t,s):MMSYSERR_NOTSUPPORTED; }
MMRESULT WINAPI waveOutOpen(LPHWAVEOUT h,UINT id,LPCWAVEFORMATEX f,DWORD_PTR cb,DWORD_PTR inst,DWORD fl) { auto fn=(MMRESULT(WINAPI*)(LPHWAVEOUT,UINT,LPCWAVEFORMATEX,DWORD_PTR,DWORD_PTR,DWORD))RealFunc("waveOutOpen"); return fn?fn(h,id,f,cb,inst,fl):MMSYSERR_NOTSUPPORTED; }
MMRESULT WINAPI waveOutClose(HWAVEOUT h)   { auto f=(MMRESULT(WINAPI*)(HWAVEOUT))RealFunc("waveOutClose"); return f?f(h):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI waveOutPrepareHeader(HWAVEOUT h,LPWAVEHDR p,UINT s) { auto f=(MMRESULT(WINAPI*)(HWAVEOUT,LPWAVEHDR,UINT))RealFunc("waveOutPrepareHeader"); return f?f(h,p,s):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI waveOutUnprepareHeader(HWAVEOUT h,LPWAVEHDR p,UINT s){ auto f=(MMRESULT(WINAPI*)(HWAVEOUT,LPWAVEHDR,UINT))RealFunc("waveOutUnprepareHeader"); return f?f(h,p,s):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI waveOutWrite(HWAVEOUT h,LPWAVEHDR p,UINT s) { auto f=(MMRESULT(WINAPI*)(HWAVEOUT,LPWAVEHDR,UINT))RealFunc("waveOutWrite"); return f?f(h,p,s):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI waveOutPause(HWAVEOUT h)   { auto f=(MMRESULT(WINAPI*)(HWAVEOUT))RealFunc("waveOutPause"); return f?f(h):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI waveOutRestart(HWAVEOUT h) { auto f=(MMRESULT(WINAPI*)(HWAVEOUT))RealFunc("waveOutRestart"); return f?f(h):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI waveOutReset(HWAVEOUT h)   { auto f=(MMRESULT(WINAPI*)(HWAVEOUT))RealFunc("waveOutReset"); return f?f(h):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI waveOutBreakLoop(HWAVEOUT h){ auto f=(MMRESULT(WINAPI*)(HWAVEOUT))RealFunc("waveOutBreakLoop"); return f?f(h):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI waveOutGetPosition(HWAVEOUT h,MMTIME* t,UINT s) { auto f=(MMRESULT(WINAPI*)(HWAVEOUT,MMTIME*,UINT))RealFunc("waveOutGetPosition"); return f?f(h,t,s):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI waveOutGetPitch(HWAVEOUT h,LPDWORD p) { auto f=(MMRESULT(WINAPI*)(HWAVEOUT,LPDWORD))RealFunc("waveOutGetPitch"); return f?f(h,p):MMSYSERR_NOTSUPPORTED; }
MMRESULT WINAPI waveOutSetPitch(HWAVEOUT h,DWORD p)   { auto f=(MMRESULT(WINAPI*)(HWAVEOUT,DWORD))RealFunc("waveOutSetPitch"); return f?f(h,p):MMSYSERR_NOTSUPPORTED; }
MMRESULT WINAPI waveOutGetPlaybackRate(HWAVEOUT h,LPDWORD r) { auto f=(MMRESULT(WINAPI*)(HWAVEOUT,LPDWORD))RealFunc("waveOutGetPlaybackRate"); return f?f(h,r):MMSYSERR_NOTSUPPORTED; }
MMRESULT WINAPI waveOutSetPlaybackRate(HWAVEOUT h,DWORD r)   { auto f=(MMRESULT(WINAPI*)(HWAVEOUT,DWORD))RealFunc("waveOutSetPlaybackRate"); return f?f(h,r):MMSYSERR_NOTSUPPORTED; }
MMRESULT WINAPI waveOutGetVolume(HWAVEOUT h,LPDWORD v)  { auto f=(MMRESULT(WINAPI*)(HWAVEOUT,LPDWORD))RealFunc("waveOutGetVolume"); return f?f(h,v):MMSYSERR_NOTSUPPORTED; }
MMRESULT WINAPI waveOutSetVolume(HWAVEOUT h,DWORD v)    { auto f=(MMRESULT(WINAPI*)(HWAVEOUT,DWORD))RealFunc("waveOutSetVolume"); return f?f(h,v):MMSYSERR_NOTSUPPORTED; }
MMRESULT WINAPI waveOutMessage(HWAVEOUT h,UINT m,DWORD_PTR p1,DWORD_PTR p2) { auto f=(MMRESULT(WINAPI*)(HWAVEOUT,UINT,DWORD_PTR,DWORD_PTR))RealFunc("waveOutMessage"); return f?f(h,m,p1,p2):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI waveOutGetID(HWAVEOUT h,LPUINT id) { auto f=(MMRESULT(WINAPI*)(HWAVEOUT,LPUINT))RealFunc("waveOutGetID"); return f?f(h,id):MMSYSERR_INVALHANDLE; }

// Wave in
UINT     WINAPI waveInGetNumDevs()         { auto f=(UINT(WINAPI*)())RealFunc("waveInGetNumDevs"); return f?f():0; }
MMRESULT WINAPI waveInGetDevCapsA(UINT_PTR d,LPWAVEINCAPSA c,UINT s)  { auto f=(MMRESULT(WINAPI*)(UINT_PTR,LPWAVEINCAPSA,UINT))RealFunc("waveInGetDevCapsA"); return f?f(d,c,s):MMSYSERR_NOTSUPPORTED; }
MMRESULT WINAPI waveInGetDevCapsW(UINT_PTR d,LPWAVEINCAPSW c,UINT s)  { auto f=(MMRESULT(WINAPI*)(UINT_PTR,LPWAVEINCAPSW,UINT))RealFunc("waveInGetDevCapsW"); return f?f(d,c,s):MMSYSERR_NOTSUPPORTED; }
MMRESULT WINAPI waveInGetErrorTextA(MMRESULT e,LPSTR t,UINT s)  { auto f=(MMRESULT(WINAPI*)(MMRESULT,LPSTR,UINT))RealFunc("waveInGetErrorTextA"); return f?f(e,t,s):MMSYSERR_NOTSUPPORTED; }
MMRESULT WINAPI waveInGetErrorTextW(MMRESULT e,LPWSTR t,UINT s) { auto f=(MMRESULT(WINAPI*)(MMRESULT,LPWSTR,UINT))RealFunc("waveInGetErrorTextW"); return f?f(e,t,s):MMSYSERR_NOTSUPPORTED; }
MMRESULT WINAPI waveInOpen(LPHWAVEIN h,UINT id,LPCWAVEFORMATEX f,DWORD_PTR cb,DWORD_PTR inst,DWORD fl) { auto fn=(MMRESULT(WINAPI*)(LPHWAVEIN,UINT,LPCWAVEFORMATEX,DWORD_PTR,DWORD_PTR,DWORD))RealFunc("waveInOpen"); return fn?fn(h,id,f,cb,inst,fl):MMSYSERR_NOTSUPPORTED; }
MMRESULT WINAPI waveInClose(HWAVEIN h)     { auto f=(MMRESULT(WINAPI*)(HWAVEIN))RealFunc("waveInClose"); return f?f(h):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI waveInPrepareHeader(HWAVEIN h,LPWAVEHDR p,UINT s)  { auto f=(MMRESULT(WINAPI*)(HWAVEIN,LPWAVEHDR,UINT))RealFunc("waveInPrepareHeader"); return f?f(h,p,s):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI waveInUnprepareHeader(HWAVEIN h,LPWAVEHDR p,UINT s){ auto f=(MMRESULT(WINAPI*)(HWAVEIN,LPWAVEHDR,UINT))RealFunc("waveInUnprepareHeader"); return f?f(h,p,s):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI waveInAddBuffer(HWAVEIN h,LPWAVEHDR p,UINT s) { auto f=(MMRESULT(WINAPI*)(HWAVEIN,LPWAVEHDR,UINT))RealFunc("waveInAddBuffer"); return f?f(h,p,s):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI waveInStart(HWAVEIN h)     { auto f=(MMRESULT(WINAPI*)(HWAVEIN))RealFunc("waveInStart"); return f?f(h):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI waveInStop(HWAVEIN h)      { auto f=(MMRESULT(WINAPI*)(HWAVEIN))RealFunc("waveInStop"); return f?f(h):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI waveInReset(HWAVEIN h)     { auto f=(MMRESULT(WINAPI*)(HWAVEIN))RealFunc("waveInReset"); return f?f(h):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI waveInGetPosition(HWAVEIN h,MMTIME* t,UINT s) { auto f=(MMRESULT(WINAPI*)(HWAVEIN,MMTIME*,UINT))RealFunc("waveInGetPosition"); return f?f(h,t,s):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI waveInMessage(HWAVEIN h,UINT m,DWORD_PTR p1,DWORD_PTR p2) { auto f=(MMRESULT(WINAPI*)(HWAVEIN,UINT,DWORD_PTR,DWORD_PTR))RealFunc("waveInMessage"); return f?f(h,m,p1,p2):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI waveInGetID(HWAVEIN h,LPUINT id) { auto f=(MMRESULT(WINAPI*)(HWAVEIN,LPUINT))RealFunc("waveInGetID"); return f?f(h,id):MMSYSERR_INVALHANDLE; }

// MIDI out
UINT     WINAPI midiOutGetNumDevs()        { auto f=(UINT(WINAPI*)())RealFunc("midiOutGetNumDevs"); return f?f():0; }
MMRESULT WINAPI midiOutGetDevCapsA(UINT_PTR d,LPMIDIOUTCAPSA c,UINT s) { auto f=(MMRESULT(WINAPI*)(UINT_PTR,LPMIDIOUTCAPSA,UINT))RealFunc("midiOutGetDevCapsA"); return f?f(d,c,s):MMSYSERR_NOTSUPPORTED; }
MMRESULT WINAPI midiOutGetDevCapsW(UINT_PTR d,LPMIDIOUTCAPSW c,UINT s) { auto f=(MMRESULT(WINAPI*)(UINT_PTR,LPMIDIOUTCAPSW,UINT))RealFunc("midiOutGetDevCapsW"); return f?f(d,c,s):MMSYSERR_NOTSUPPORTED; }
MMRESULT WINAPI midiOutGetErrorTextA(MMRESULT e,LPSTR t,UINT s)  { auto f=(MMRESULT(WINAPI*)(MMRESULT,LPSTR,UINT))RealFunc("midiOutGetErrorTextA"); return f?f(e,t,s):MMSYSERR_NOTSUPPORTED; }
MMRESULT WINAPI midiOutGetErrorTextW(MMRESULT e,LPWSTR t,UINT s) { auto f=(MMRESULT(WINAPI*)(MMRESULT,LPWSTR,UINT))RealFunc("midiOutGetErrorTextW"); return f?f(e,t,s):MMSYSERR_NOTSUPPORTED; }
MMRESULT WINAPI midiOutOpen(LPHMIDIOUT h,UINT id,DWORD_PTR cb,DWORD_PTR inst,DWORD fl) { auto f=(MMRESULT(WINAPI*)(LPHMIDIOUT,UINT,DWORD_PTR,DWORD_PTR,DWORD))RealFunc("midiOutOpen"); return f?f(h,id,cb,inst,fl):MMSYSERR_NOTSUPPORTED; }
MMRESULT WINAPI midiOutClose(HMIDIOUT h)   { auto f=(MMRESULT(WINAPI*)(HMIDIOUT))RealFunc("midiOutClose"); return f?f(h):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI midiOutPrepareHeader(HMIDIOUT h,LPMIDIHDR p,UINT s) { auto f=(MMRESULT(WINAPI*)(HMIDIOUT,LPMIDIHDR,UINT))RealFunc("midiOutPrepareHeader"); return f?f(h,p,s):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI midiOutUnprepareHeader(HMIDIOUT h,LPMIDIHDR p,UINT s){ auto f=(MMRESULT(WINAPI*)(HMIDIOUT,LPMIDIHDR,UINT))RealFunc("midiOutUnprepareHeader"); return f?f(h,p,s):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI midiOutShortMsg(HMIDIOUT h,DWORD m) { auto f=(MMRESULT(WINAPI*)(HMIDIOUT,DWORD))RealFunc("midiOutShortMsg"); return f?f(h,m):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI midiOutLongMsg(HMIDIOUT h,LPMIDIHDR p,UINT s) { auto f=(MMRESULT(WINAPI*)(HMIDIOUT,LPMIDIHDR,UINT))RealFunc("midiOutLongMsg"); return f?f(h,p,s):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI midiOutReset(HMIDIOUT h)   { auto f=(MMRESULT(WINAPI*)(HMIDIOUT))RealFunc("midiOutReset"); return f?f(h):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI midiOutGetVolume(HMIDIOUT h,LPDWORD v) { auto f=(MMRESULT(WINAPI*)(HMIDIOUT,LPDWORD))RealFunc("midiOutGetVolume"); return f?f(h,v):MMSYSERR_NOTSUPPORTED; }
MMRESULT WINAPI midiOutSetVolume(HMIDIOUT h,DWORD v)   { auto f=(MMRESULT(WINAPI*)(HMIDIOUT,DWORD))RealFunc("midiOutSetVolume"); return f?f(h,v):MMSYSERR_NOTSUPPORTED; }
MMRESULT WINAPI midiOutCachePatches(HMIDIOUT h,UINT b,LPWORD pa,UINT f) { auto fn=(MMRESULT(WINAPI*)(HMIDIOUT,UINT,LPWORD,UINT))RealFunc("midiOutCachePatches"); return fn?fn(h,b,pa,f):MMSYSERR_NOTSUPPORTED; }
MMRESULT WINAPI midiOutCacheDrumPatches(HMIDIOUT h,UINT p,LPWORD pa,UINT f){ auto fn=(MMRESULT(WINAPI*)(HMIDIOUT,UINT,LPWORD,UINT))RealFunc("midiOutCacheDrumPatches"); return fn?fn(h,p,pa,f):MMSYSERR_NOTSUPPORTED; }
MMRESULT WINAPI midiOutGetID(HMIDIOUT h,LPUINT id) { auto f=(MMRESULT(WINAPI*)(HMIDIOUT,LPUINT))RealFunc("midiOutGetID"); return f?f(h,id):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI midiOutMessage(HMIDIOUT h,UINT m,DWORD_PTR p1,DWORD_PTR p2) { auto f=(MMRESULT(WINAPI*)(HMIDIOUT,UINT,DWORD_PTR,DWORD_PTR))RealFunc("midiOutMessage"); return f?f(h,m,p1,p2):MMSYSERR_INVALHANDLE; }

// MIDI in
UINT     WINAPI midiInGetNumDevs()         { auto f=(UINT(WINAPI*)())RealFunc("midiInGetNumDevs"); return f?f():0; }
MMRESULT WINAPI midiInGetDevCapsA(UINT_PTR d,LPMIDIINCAPSA c,UINT s) { auto f=(MMRESULT(WINAPI*)(UINT_PTR,LPMIDIINCAPSA,UINT))RealFunc("midiInGetDevCapsA"); return f?f(d,c,s):MMSYSERR_NOTSUPPORTED; }
MMRESULT WINAPI midiInGetDevCapsW(UINT_PTR d,LPMIDIINCAPSW c,UINT s) { auto f=(MMRESULT(WINAPI*)(UINT_PTR,LPMIDIINCAPSW,UINT))RealFunc("midiInGetDevCapsW"); return f?f(d,c,s):MMSYSERR_NOTSUPPORTED; }
MMRESULT WINAPI midiInGetErrorTextA(MMRESULT e,LPSTR t,UINT s)  { auto f=(MMRESULT(WINAPI*)(MMRESULT,LPSTR,UINT))RealFunc("midiInGetErrorTextA"); return f?f(e,t,s):MMSYSERR_NOTSUPPORTED; }
MMRESULT WINAPI midiInGetErrorTextW(MMRESULT e,LPWSTR t,UINT s) { auto f=(MMRESULT(WINAPI*)(MMRESULT,LPWSTR,UINT))RealFunc("midiInGetErrorTextW"); return f?f(e,t,s):MMSYSERR_NOTSUPPORTED; }
MMRESULT WINAPI midiInOpen(LPHMIDIIN h,UINT id,DWORD_PTR cb,DWORD_PTR inst,DWORD fl) { auto f=(MMRESULT(WINAPI*)(LPHMIDIIN,UINT,DWORD_PTR,DWORD_PTR,DWORD))RealFunc("midiInOpen"); return f?f(h,id,cb,inst,fl):MMSYSERR_NOTSUPPORTED; }
MMRESULT WINAPI midiInClose(HMIDIIN h)     { auto f=(MMRESULT(WINAPI*)(HMIDIIN))RealFunc("midiInClose"); return f?f(h):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI midiInPrepareHeader(HMIDIIN h,LPMIDIHDR p,UINT s) { auto f=(MMRESULT(WINAPI*)(HMIDIIN,LPMIDIHDR,UINT))RealFunc("midiInPrepareHeader"); return f?f(h,p,s):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI midiInUnprepareHeader(HMIDIIN h,LPMIDIHDR p,UINT s){ auto f=(MMRESULT(WINAPI*)(HMIDIIN,LPMIDIHDR,UINT))RealFunc("midiInUnprepareHeader"); return f?f(h,p,s):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI midiInAddBuffer(HMIDIIN h,LPMIDIHDR p,UINT s) { auto f=(MMRESULT(WINAPI*)(HMIDIIN,LPMIDIHDR,UINT))RealFunc("midiInAddBuffer"); return f?f(h,p,s):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI midiInStart(HMIDIIN h)     { auto f=(MMRESULT(WINAPI*)(HMIDIIN))RealFunc("midiInStart"); return f?f(h):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI midiInStop(HMIDIIN h)      { auto f=(MMRESULT(WINAPI*)(HMIDIIN))RealFunc("midiInStop"); return f?f(h):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI midiInReset(HMIDIIN h)     { auto f=(MMRESULT(WINAPI*)(HMIDIIN))RealFunc("midiInReset"); return f?f(h):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI midiInGetID(HMIDIIN h,LPUINT id) { auto f=(MMRESULT(WINAPI*)(HMIDIIN,LPUINT))RealFunc("midiInGetID"); return f?f(h,id):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI midiInMessage(HMIDIIN h,UINT m,DWORD_PTR p1,DWORD_PTR p2) { auto f=(MMRESULT(WINAPI*)(HMIDIIN,UINT,DWORD_PTR,DWORD_PTR))RealFunc("midiInMessage"); return f?f(h,m,p1,p2):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI midiConnect(HMIDI h,HMIDIOUT out,LPVOID r) { auto f=(MMRESULT(WINAPI*)(HMIDI,HMIDIOUT,LPVOID))RealFunc("midiConnect"); return f?f(h,out,r):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI midiDisconnect(HMIDI h,HMIDIOUT out,LPVOID r) { auto f=(MMRESULT(WINAPI*)(HMIDI,HMIDIOUT,LPVOID))RealFunc("midiDisconnect"); return f?f(h,out,r):MMSYSERR_INVALHANDLE; }

// MIDI stream
MMRESULT WINAPI midiStreamOpen(LPHMIDISTRM h,LPUINT id,DWORD c,DWORD_PTR cb,DWORD_PTR inst,DWORD fl) { auto f=(MMRESULT(WINAPI*)(LPHMIDISTRM,LPUINT,DWORD,DWORD_PTR,DWORD_PTR,DWORD))RealFunc("midiStreamOpen"); return f?f(h,id,c,cb,inst,fl):MMSYSERR_NOTSUPPORTED; }
MMRESULT WINAPI midiStreamClose(HMIDISTRM h) { auto f=(MMRESULT(WINAPI*)(HMIDISTRM))RealFunc("midiStreamClose"); return f?f(h):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI midiStreamProperty(HMIDISTRM h,LPBYTE p,DWORD fl) { auto f=(MMRESULT(WINAPI*)(HMIDISTRM,LPBYTE,DWORD))RealFunc("midiStreamProperty"); return f?f(h,p,fl):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI midiStreamPosition(HMIDISTRM h,LPMMTIME t,UINT s) { auto f=(MMRESULT(WINAPI*)(HMIDISTRM,LPMMTIME,UINT))RealFunc("midiStreamPosition"); return f?f(h,t,s):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI midiStreamOut(HMIDISTRM h,LPMIDIHDR p,UINT s) { auto f=(MMRESULT(WINAPI*)(HMIDISTRM,LPMIDIHDR,UINT))RealFunc("midiStreamOut"); return f?f(h,p,s):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI midiStreamPause(HMIDISTRM h)   { auto f=(MMRESULT(WINAPI*)(HMIDISTRM))RealFunc("midiStreamPause"); return f?f(h):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI midiStreamRestart(HMIDISTRM h) { auto f=(MMRESULT(WINAPI*)(HMIDISTRM))RealFunc("midiStreamRestart"); return f?f(h):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI midiStreamStop(HMIDISTRM h)    { auto f=(MMRESULT(WINAPI*)(HMIDISTRM))RealFunc("midiStreamStop"); return f?f(h):MMSYSERR_INVALHANDLE; }

// Mixer
UINT     WINAPI mixerGetNumDevs()          { auto f=(UINT(WINAPI*)())RealFunc("mixerGetNumDevs"); return f?f():0; }
MMRESULT WINAPI mixerGetDevCapsA(UINT_PTR d,LPMIXERCAPSA c,UINT s) { auto f=(MMRESULT(WINAPI*)(UINT_PTR,LPMIXERCAPSA,UINT))RealFunc("mixerGetDevCapsA"); return f?f(d,c,s):MMSYSERR_NOTSUPPORTED; }
MMRESULT WINAPI mixerGetDevCapsW(UINT_PTR d,LPMIXERCAPSW c,UINT s) { auto f=(MMRESULT(WINAPI*)(UINT_PTR,LPMIXERCAPSW,UINT))RealFunc("mixerGetDevCapsW"); return f?f(d,c,s):MMSYSERR_NOTSUPPORTED; }
MMRESULT WINAPI mixerOpen(LPHMIXER h,UINT id,DWORD_PTR cb,DWORD_PTR inst,DWORD fl) { auto f=(MMRESULT(WINAPI*)(LPHMIXER,UINT,DWORD_PTR,DWORD_PTR,DWORD))RealFunc("mixerOpen"); return f?f(h,id,cb,inst,fl):MMSYSERR_NOTSUPPORTED; }
MMRESULT WINAPI mixerClose(HMIXER h)       { auto f=(MMRESULT(WINAPI*)(HMIXER))RealFunc("mixerClose"); return f?f(h):MMSYSERR_INVALHANDLE; }
DWORD    WINAPI mixerMessage(HMIXER h,UINT m,DWORD_PTR p1,DWORD_PTR p2) { auto f=(DWORD(WINAPI*)(HMIXER,UINT,DWORD_PTR,DWORD_PTR))RealFunc("mixerMessage"); return f?f(h,m,p1,p2):(DWORD)MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI mixerGetLineInfoA(HMIXEROBJ h,LPMIXERLINEA l,DWORD fl) { auto f=(MMRESULT(WINAPI*)(HMIXEROBJ,LPMIXERLINEA,DWORD))RealFunc("mixerGetLineInfoA"); return f?f(h,l,fl):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI mixerGetLineInfoW(HMIXEROBJ h,LPMIXERLINEW l,DWORD fl) { auto f=(MMRESULT(WINAPI*)(HMIXEROBJ,LPMIXERLINEW,DWORD))RealFunc("mixerGetLineInfoW"); return f?f(h,l,fl):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI mixerGetID(HMIXEROBJ h,LPUINT id,DWORD fl) { auto f=(MMRESULT(WINAPI*)(HMIXEROBJ,LPUINT,DWORD))RealFunc("mixerGetID"); return f?f(h,id,fl):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI mixerGetLineControlsA(HMIXEROBJ h,LPMIXERLINECONTROLSA lc,DWORD fl) { auto f=(MMRESULT(WINAPI*)(HMIXEROBJ,LPMIXERLINECONTROLSA,DWORD))RealFunc("mixerGetLineControlsA"); return f?f(h,lc,fl):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI mixerGetLineControlsW(HMIXEROBJ h,LPMIXERLINECONTROLSW lc,DWORD fl) { auto f=(MMRESULT(WINAPI*)(HMIXEROBJ,LPMIXERLINECONTROLSW,DWORD))RealFunc("mixerGetLineControlsW"); return f?f(h,lc,fl):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI mixerGetControlDetailsA(HMIXEROBJ h,LPMIXERCONTROLDETAILS d,DWORD fl) { auto f=(MMRESULT(WINAPI*)(HMIXEROBJ,LPMIXERCONTROLDETAILS,DWORD))RealFunc("mixerGetControlDetailsA"); return f?f(h,d,fl):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI mixerGetControlDetailsW(HMIXEROBJ h,LPMIXERCONTROLDETAILS d,DWORD fl) { auto f=(MMRESULT(WINAPI*)(HMIXEROBJ,LPMIXERCONTROLDETAILS,DWORD))RealFunc("mixerGetControlDetailsW"); return f?f(h,d,fl):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI mixerSetControlDetails(HMIXEROBJ h,LPMIXERCONTROLDETAILS d,DWORD fl) { auto f=(MMRESULT(WINAPI*)(HMIXEROBJ,LPMIXERCONTROLDETAILS,DWORD))RealFunc("mixerSetControlDetails"); return f?f(h,d,fl):MMSYSERR_INVALHANDLE; }

// Auxiliary
UINT     WINAPI auxGetNumDevs()            { auto f=(UINT(WINAPI*)())RealFunc("auxGetNumDevs"); return f?f():0; }
MMRESULT WINAPI auxGetDevCapsA(UINT_PTR d,LPAUXCAPSA c,UINT s) { auto f=(MMRESULT(WINAPI*)(UINT_PTR,LPAUXCAPSA,UINT))RealFunc("auxGetDevCapsA"); return f?f(d,c,s):MMSYSERR_NOTSUPPORTED; }
MMRESULT WINAPI auxGetDevCapsW(UINT_PTR d,LPAUXCAPSW c,UINT s) { auto f=(MMRESULT(WINAPI*)(UINT_PTR,LPAUXCAPSW,UINT))RealFunc("auxGetDevCapsW"); return f?f(d,c,s):MMSYSERR_NOTSUPPORTED; }
MMRESULT WINAPI auxGetVolume(UINT id,LPDWORD v) { auto f=(MMRESULT(WINAPI*)(UINT,LPDWORD))RealFunc("auxGetVolume"); return f?f(id,v):MMSYSERR_NOTSUPPORTED; }
MMRESULT WINAPI auxSetVolume(UINT id,DWORD v)   { auto f=(MMRESULT(WINAPI*)(UINT,DWORD))RealFunc("auxSetVolume"); return f?f(id,v):MMSYSERR_NOTSUPPORTED; }
MMRESULT WINAPI auxOutMessage(UINT id,UINT m,DWORD_PTR p1,DWORD_PTR p2) { auto f=(MMRESULT(WINAPI*)(UINT,UINT,DWORD_PTR,DWORD_PTR))RealFunc("auxOutMessage"); return f?f(id,m,p1,p2):MMSYSERR_NOTSUPPORTED; }

// Joystick
UINT     WINAPI joyGetNumDevs()            { auto f=(UINT(WINAPI*)())RealFunc("joyGetNumDevs"); return f?f():0; }
MMRESULT WINAPI joyGetDevCapsA(UINT_PTR id,LPJOYCAPSA c,UINT s) { auto f=(MMRESULT(WINAPI*)(UINT_PTR,LPJOYCAPSA,UINT))RealFunc("joyGetDevCapsA"); return f?f(id,c,s):JOYERR_PARMS; }
MMRESULT WINAPI joyGetDevCapsW(UINT_PTR id,LPJOYCAPSW c,UINT s) { auto f=(MMRESULT(WINAPI*)(UINT_PTR,LPJOYCAPSW,UINT))RealFunc("joyGetDevCapsW"); return f?f(id,c,s):JOYERR_PARMS; }
MMRESULT WINAPI joyGetPos(UINT id,LPJOYINFO p)   { auto f=(MMRESULT(WINAPI*)(UINT,LPJOYINFO))RealFunc("joyGetPos"); return f?f(id,p):JOYERR_NOERROR; }
MMRESULT WINAPI joyGetPosEx(UINT id,LPJOYINFOEX p){ auto f=(MMRESULT(WINAPI*)(UINT,LPJOYINFOEX))RealFunc("joyGetPosEx"); return f?f(id,p):JOYERR_NOERROR; }
MMRESULT WINAPI joyGetThreshold(UINT id,LPUINT t) { auto f=(MMRESULT(WINAPI*)(UINT,LPUINT))RealFunc("joyGetThreshold"); return f?f(id,t):JOYERR_PARMS; }
MMRESULT WINAPI joySetThreshold(UINT id,UINT t)   { auto f=(MMRESULT(WINAPI*)(UINT,UINT))RealFunc("joySetThreshold"); return f?f(id,t):JOYERR_PARMS; }
MMRESULT WINAPI joySetCapture(HWND h,UINT id,UINT p,BOOL c) { auto f=(MMRESULT(WINAPI*)(HWND,UINT,UINT,BOOL))RealFunc("joySetCapture"); return f?f(h,id,p,c):JOYERR_NOCANDO; }
MMRESULT WINAPI joyReleaseCapture(UINT id) { auto f=(MMRESULT(WINAPI*)(UINT))RealFunc("joyReleaseCapture"); return f?f(id):JOYERR_NOERROR; }
MMRESULT WINAPI joyConfigChanged(DWORD fl) { auto f=(MMRESULT(WINAPI*)(DWORD))RealFunc("joyConfigChanged"); return f?f(fl):JOYERR_NOERROR; }

// MMIO
HMMIO    WINAPI mmioOpenA(LPSTR fn,LPMMIOINFO i,DWORD fl) { auto f=(HMMIO(WINAPI*)(LPSTR,LPMMIOINFO,DWORD))RealFunc("mmioOpenA"); return f?f(fn,i,fl):NULL; }
HMMIO    WINAPI mmioOpenW(LPWSTR fn,LPMMIOINFO i,DWORD fl){ auto f=(HMMIO(WINAPI*)(LPWSTR,LPMMIOINFO,DWORD))RealFunc("mmioOpenW"); return f?f(fn,i,fl):NULL; }
MMRESULT WINAPI mmioClose(HMMIO h,UINT fl) { auto f=(MMRESULT(WINAPI*)(HMMIO,UINT))RealFunc("mmioClose"); return f?f(h,fl):MMSYSERR_INVALHANDLE; }
LONG     WINAPI mmioRead(HMMIO h,HPSTR b,LONG s)  { auto f=(LONG(WINAPI*)(HMMIO,HPSTR,LONG))RealFunc("mmioRead"); return f?f(h,b,s):-1; }
LONG     WINAPI mmioWrite(HMMIO h,const char* b,LONG s) { auto f=(LONG(WINAPI*)(HMMIO,const char*,LONG))RealFunc("mmioWrite"); return f?f(h,b,s):-1; }
LONG     WINAPI mmioSeek(HMMIO h,LONG off,int org) { auto f=(LONG(WINAPI*)(HMMIO,LONG,int))RealFunc("mmioSeek"); return f?f(h,off,org):-1; }
MMRESULT WINAPI mmioGetInfo(HMMIO h,LPMMIOINFO i,UINT fl){ auto f=(MMRESULT(WINAPI*)(HMMIO,LPMMIOINFO,UINT))RealFunc("mmioGetInfo"); return f?f(h,i,fl):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI mmioSetInfo(HMMIO h,LPCMMIOINFO i,UINT fl){ auto f=(MMRESULT(WINAPI*)(HMMIO,LPCMMIOINFO,UINT))RealFunc("mmioSetInfo"); return f?f(h,i,fl):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI mmioSetBuffer(HMMIO h,LPSTR b,LONG s,UINT fl){ auto f=(MMRESULT(WINAPI*)(HMMIO,LPSTR,LONG,UINT))RealFunc("mmioSetBuffer"); return f?f(h,b,s,fl):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI mmioFlush(HMMIO h,UINT fl) { auto f=(MMRESULT(WINAPI*)(HMMIO,UINT))RealFunc("mmioFlush"); return f?f(h,fl):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI mmioAdvance(HMMIO h,LPMMIOINFO i,UINT fl){ auto f=(MMRESULT(WINAPI*)(HMMIO,LPMMIOINFO,UINT))RealFunc("mmioAdvance"); return f?f(h,i,fl):MMSYSERR_INVALHANDLE; }
LRESULT  WINAPI mmioSendMessage(HMMIO h,UINT m,LPARAM p1,LPARAM p2) { auto f=(LRESULT(WINAPI*)(HMMIO,UINT,LPARAM,LPARAM))RealFunc("mmioSendMessage"); return f?f(h,m,p1,p2):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI mmioDescend(HMMIO h,LPMMCKINFO c,const MMCKINFO* p,UINT fl){ auto f=(MMRESULT(WINAPI*)(HMMIO,LPMMCKINFO,const MMCKINFO*,UINT))RealFunc("mmioDescend"); return f?f(h,c,p,fl):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI mmioAscend(HMMIO h,LPMMCKINFO c,UINT fl){ auto f=(MMRESULT(WINAPI*)(HMMIO,LPMMCKINFO,UINT))RealFunc("mmioAscend"); return f?f(h,c,fl):MMSYSERR_INVALHANDLE; }
MMRESULT WINAPI mmioCreateChunk(HMMIO h,LPMMCKINFO c,UINT fl){ auto f=(MMRESULT(WINAPI*)(HMMIO,LPMMCKINFO,UINT))RealFunc("mmioCreateChunk"); return f?f(h,c,fl):MMSYSERR_INVALHANDLE; }
LPMMIOPROC WINAPI mmioInstallIOProcA(FOURCC fcc,LPMMIOPROC p,DWORD fl){ auto f=(LPMMIOPROC(WINAPI*)(FOURCC,LPMMIOPROC,DWORD))RealFunc("mmioInstallIOProcA"); return f?f(fcc,p,fl):NULL; }
LPMMIOPROC WINAPI mmioInstallIOProcW(FOURCC fcc,LPMMIOPROC p,DWORD fl){ auto f=(LPMMIOPROC(WINAPI*)(FOURCC,LPMMIOPROC,DWORD))RealFunc("mmioInstallIOProcW"); return f?f(fcc,p,fl):NULL; }
FOURCC   WINAPI mmioStringToFOURCCA(LPCSTR s,UINT fl){ auto f=(FOURCC(WINAPI*)(LPCSTR,UINT))RealFunc("mmioStringToFOURCCA"); return f?f(s,fl):0; }
FOURCC   WINAPI mmioStringToFOURCCW(LPCWSTR s,UINT fl){ auto f=(FOURCC(WINAPI*)(LPCWSTR,UINT))RealFunc("mmioStringToFOURCCW"); return f?f(s,fl):0; }
MMRESULT WINAPI mmioRenameA(LPCSTR fn,LPCSTR nfn,const MMIOINFO* i,DWORD fl){ auto f=(MMRESULT(WINAPI*)(LPCSTR,LPCSTR,const MMIOINFO*,DWORD))RealFunc("mmioRenameA"); return f?f(fn,nfn,i,fl):MMSYSERR_NOTSUPPORTED; }
MMRESULT WINAPI mmioRenameW(LPCWSTR fn,LPCWSTR nfn,const MMIOINFO* i,DWORD fl){ auto f=(MMRESULT(WINAPI*)(LPCWSTR,LPCWSTR,const MMIOINFO*,DWORD))RealFunc("mmioRenameW"); return f?f(fn,nfn,i,fl):MMSYSERR_NOTSUPPORTED; }

// MCI
BOOL     WINAPI mciGetErrorStringA(MCIERROR e,LPSTR t,UINT s){ auto f=(BOOL(WINAPI*)(MCIERROR,LPSTR,UINT))RealFunc("mciGetErrorStringA"); return f?f(e,t,s):FALSE; }
BOOL     WINAPI mciGetErrorStringW(MCIERROR e,LPWSTR t,UINT s){ auto f=(BOOL(WINAPI*)(MCIERROR,LPWSTR,UINT))RealFunc("mciGetErrorStringW"); return f?f(e,t,s):FALSE; }
MCIERROR WINAPI mciSendCommandA(MCIDEVICEID id,UINT m,DWORD_PTR fl,DWORD_PTR p){ auto f=(MCIERROR(WINAPI*)(MCIDEVICEID,UINT,DWORD_PTR,DWORD_PTR))RealFunc("mciSendCommandA"); return f?f(id,m,fl,p):MCIERR_DEVICE_NOT_INSTALLED; }
MCIERROR WINAPI mciSendCommandW(MCIDEVICEID id,UINT m,DWORD_PTR fl,DWORD_PTR p){ auto f=(MCIERROR(WINAPI*)(MCIDEVICEID,UINT,DWORD_PTR,DWORD_PTR))RealFunc("mciSendCommandW"); return f?f(id,m,fl,p):MCIERR_DEVICE_NOT_INSTALLED; }
MCIERROR WINAPI mciSendStringA(LPCSTR cmd,LPSTR ret,UINT retLen,HWND cb){ auto f=(MCIERROR(WINAPI*)(LPCSTR,LPSTR,UINT,HWND))RealFunc("mciSendStringA"); return f?f(cmd,ret,retLen,cb):MCIERR_DEVICE_NOT_INSTALLED; }
MCIERROR WINAPI mciSendStringW(LPCWSTR cmd,LPWSTR ret,UINT retLen,HWND cb){ auto f=(MCIERROR(WINAPI*)(LPCWSTR,LPWSTR,UINT,HWND))RealFunc("mciSendStringW"); return f?f(cmd,ret,retLen,cb):MCIERR_DEVICE_NOT_INSTALLED; }
MCIDEVICEID WINAPI mciGetDeviceIDA(LPCSTR name){ auto f=(MCIDEVICEID(WINAPI*)(LPCSTR))RealFunc("mciGetDeviceIDA"); return f?f(name):0; }
MCIDEVICEID WINAPI mciGetDeviceIDW(LPCWSTR name){ auto f=(MCIDEVICEID(WINAPI*)(LPCWSTR))RealFunc("mciGetDeviceIDW"); return f?f(name):0; }
MCIDEVICEID WINAPI mciGetDeviceIDFromElementIDA(DWORD el,LPCSTR t){ auto f=(MCIDEVICEID(WINAPI*)(DWORD,LPCSTR))RealFunc("mciGetDeviceIDFromElementIDA"); return f?f(el,t):0; }
MCIDEVICEID WINAPI mciGetDeviceIDFromElementIDW(DWORD el,LPCWSTR t){ auto f=(MCIDEVICEID(WINAPI*)(DWORD,LPCWSTR))RealFunc("mciGetDeviceIDFromElementIDW"); return f?f(el,t):0; }
BOOL     WINAPI mciExecute(LPCSTR cmd){ auto f=(BOOL(WINAPI*)(LPCSTR))RealFunc("mciExecute"); return f?f(cmd):FALSE; }
HTASK    WINAPI mciGetCreatorTask(MCIDEVICEID id){ auto f=(HTASK(WINAPI*)(MCIDEVICEID))RealFunc("mciGetCreatorTask"); return f?f(id):NULL; }
DWORD_PTR WINAPI mciGetDriverData(MCIDEVICEID id){ auto f=(DWORD_PTR(WINAPI*)(MCIDEVICEID))RealFunc("mciGetDriverData"); return f?f(id):0; }
BOOL     WINAPI mciSetDriverData(MCIDEVICEID id,DWORD_PTR d){ auto f=(BOOL(WINAPI*)(MCIDEVICEID,DWORD_PTR))RealFunc("mciSetDriverData"); return f?f(id,d):FALSE; }
UINT     WINAPI mciDriverYield(MCIDEVICEID id){ auto f=(UINT(WINAPI*)(MCIDEVICEID))RealFunc("mciDriverYield"); return f?f(id):0; }
BOOL     WINAPI mciDriverNotify(HANDLE h,MCIDEVICEID id,UINT s){ auto f=(BOOL(WINAPI*)(HANDLE,MCIDEVICEID,UINT))RealFunc("mciDriverNotify"); return f?f(h,id,s):FALSE; }
UINT     WINAPI mciLoadCommandResource(HANDLE h,LPCWSTR n,UINT t){ auto f=(UINT(WINAPI*)(HANDLE,LPCWSTR,UINT))RealFunc("mciLoadCommandResource"); return f?f(h,n,t):0; }
BOOL     WINAPI mciFreeCommandResource(UINT t){ auto f=(BOOL(WINAPI*)(UINT))RealFunc("mciFreeCommandResource"); return f?f(t):FALSE; }
YIELDPROC WINAPI mciGetYieldProc(MCIDEVICEID id,LPDWORD pyi){ auto f=(YIELDPROC(WINAPI*)(MCIDEVICEID,LPDWORD))RealFunc("mciGetYieldProc"); return f?f(id,pyi):NULL; }
BOOL     WINAPI mciSetYieldProc(MCIDEVICEID id,YIELDPROC p,DWORD d){ auto f=(BOOL(WINAPI*)(MCIDEVICEID,YIELDPROC,DWORD))RealFunc("mciSetYieldProc"); return f?f(id,p,d):FALSE; }

// Driver
HDRVR    WINAPI OpenDriver(LPCWSTR sz,LPCWSTR sec,LPARAM p){ auto f=(HDRVR(WINAPI*)(LPCWSTR,LPCWSTR,LPARAM))RealFunc("OpenDriver"); return f?f(sz,sec,p):NULL; }
LRESULT  WINAPI CloseDriver(HDRVR h,LPARAM p1,LPARAM p2){ auto f=(LRESULT(WINAPI*)(HDRVR,LPARAM,LPARAM))RealFunc("CloseDriver"); return f?f(h,p1,p2):0; }
LRESULT  WINAPI SendDriverMessage(HDRVR h,UINT m,LPARAM p1,LPARAM p2){ auto f=(LRESULT(WINAPI*)(HDRVR,UINT,LPARAM,LPARAM))RealFunc("SendDriverMessage"); return f?f(h,m,p1,p2):0; }
HMODULE  WINAPI DrvGetModuleHandle(HDRVR h){ auto f=(HMODULE(WINAPI*)(HDRVR))RealFunc("DrvGetModuleHandle"); return f?f(h):NULL; }
HMODULE  WINAPI GetDriverModuleHandle(HDRVR h){ auto f=(HMODULE(WINAPI*)(HDRVR))RealFunc("GetDriverModuleHandle"); return f?f(h):NULL; }
LRESULT  WINAPI DefDriverProc(DWORD_PTR id,HDRVR h,UINT m,LPARAM p1,LPARAM p2){ auto f=(LRESULT(WINAPI*)(DWORD_PTR,HDRVR,UINT,LPARAM,LPARAM))RealFunc("DefDriverProc"); return f?f(id,h,m,p1,p2):0; }
BOOL     WINAPI DriverCallback(DWORD_PTR cb,DWORD fl,HDRVR h,DWORD m,DWORD_PTR u,DWORD_PTR p1,DWORD_PTR p2){ auto f=(BOOL(WINAPI*)(DWORD_PTR,DWORD,HDRVR,DWORD,DWORD_PTR,DWORD_PTR,DWORD_PTR))RealFunc("DriverCallback"); return f?f(cb,fl,h,m,u,p1,p2):FALSE; }
UINT     WINAPI mmDrvInstall(HDRVR h,LPCWSTR fn,DRIVERMSGPROC p,UINT fl){ auto f=(UINT(WINAPI*)(HDRVR,LPCWSTR,DRIVERMSGPROC,UINT))RealFunc("mmDrvInstall"); return f?f(h,fn,p,fl):0; }

// Misc
UINT     WINAPI mmsystemGetVersion(){ auto f=(UINT(WINAPI*)())RealFunc("mmsystemGetVersion"); return f?f():0x030A; }
HANDLE   WINAPI mmTaskCreate(LPTASKCALLBACK cb,HANDLE* h,DWORD_PTR p){ auto f=(HANDLE(WINAPI*)(LPTASKCALLBACK,HANDLE*,DWORD_PTR))RealFunc("mmTaskCreate"); return f?f(cb,h,p):NULL; }
void     WINAPI mmTaskBlock(DWORD tid){ auto f=(void(WINAPI*)(DWORD))RealFunc("mmTaskBlock"); if(f)f(tid); }
BOOL     WINAPI mmTaskSignal(DWORD tid){ auto f=(BOOL(WINAPI*)(DWORD))RealFunc("mmTaskSignal"); return f?f(tid):FALSE; }
void     WINAPI mmTaskYield(){ auto f=(void(WINAPI*)())RealFunc("mmTaskYield"); if(f)f(); }
DWORD    WINAPI mmGetCurrentTask(){ auto f=(DWORD(WINAPI*)())RealFunc("mmGetCurrentTask"); return f?f():0; }
void     WINAPI WOWAppExit(HANDLE h){ auto f=(void(WINAPI*)(HANDLE))RealFunc("WOWAppExit"); if(f)f(h); }

} // extern "C"
