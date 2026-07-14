// version.dll — ServerManager-Takaro proxy variant for UE games that import version.dll
// (instead of winmm/dwmapi) as their UE4SS loader. Forwards every version.dll export to
// the real System32\version.dll, bootstraps UE4SS, and starts the Takaro core.
// The core has a single-instance guard, so shipping winmm.dll + version.dll together is
// safe — whichever the game imports loads, and the core starts exactly once.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

static HMODULE g_real = NULL;

static void LoadReal() {
    char sys[MAX_PATH]; GetSystemDirectoryA(sys, MAX_PATH);
    char path[MAX_PATH]; _snprintf(path, sizeof(path), "%s\\version.dll", sys);
    g_real = LoadLibraryA(path);
}
static void* RealFunc(const char* name) {
    return g_real ? (void*)GetProcAddress(g_real, name) : NULL;
}
static void LoadUE4SS() {
    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH);
    char* sl = strrchr(exe, '\\'); if (sl) *sl = '\0';
    const char* c[] = { "\\ue4ss\\UE4SS.dll", "\\UE4SS.dll", "\\ue4ss\\dwmapi.dll" };
    for (const char* p : c) {
        char path[MAX_PATH]; _snprintf(path, sizeof(path), "%s%s", exe, p);
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) { LoadLibraryA(path); return; }
    }
}
extern "C" void StartTakaroCore();
static DWORD WINAPI LaunchThread(LPVOID) { LoadUE4SS(); StartTakaroCore(); return 0; }

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

// ─── Forwarded version.dll exports ────────────────────────────────────────────
extern "C" {
DWORD WINAPI GetFileVersionInfoSizeA(LPCSTR f, LPDWORD h) { auto fn=(DWORD(WINAPI*)(LPCSTR,LPDWORD))RealFunc("GetFileVersionInfoSizeA"); return fn?fn(f,h):0; }
DWORD WINAPI GetFileVersionInfoSizeW(LPCWSTR f, LPDWORD h){ auto fn=(DWORD(WINAPI*)(LPCWSTR,LPDWORD))RealFunc("GetFileVersionInfoSizeW"); return fn?fn(f,h):0; }
BOOL  WINAPI GetFileVersionInfoA(LPCSTR f, DWORD h, DWORD l, LPVOID d) { auto fn=(BOOL(WINAPI*)(LPCSTR,DWORD,DWORD,LPVOID))RealFunc("GetFileVersionInfoA"); return fn?fn(f,h,l,d):FALSE; }
BOOL  WINAPI GetFileVersionInfoW(LPCWSTR f, DWORD h, DWORD l, LPVOID d){ auto fn=(BOOL(WINAPI*)(LPCWSTR,DWORD,DWORD,LPVOID))RealFunc("GetFileVersionInfoW"); return fn?fn(f,h,l,d):FALSE; }
DWORD WINAPI GetFileVersionInfoSizeExA(DWORD fl, LPCSTR f, LPDWORD h) { auto fn=(DWORD(WINAPI*)(DWORD,LPCSTR,LPDWORD))RealFunc("GetFileVersionInfoSizeExA"); return fn?fn(fl,f,h):0; }
DWORD WINAPI GetFileVersionInfoSizeExW(DWORD fl, LPCWSTR f, LPDWORD h){ auto fn=(DWORD(WINAPI*)(DWORD,LPCWSTR,LPDWORD))RealFunc("GetFileVersionInfoSizeExW"); return fn?fn(fl,f,h):0; }
BOOL  WINAPI GetFileVersionInfoExA(DWORD fl, LPCSTR f, DWORD h, DWORD l, LPVOID d) { auto fn=(BOOL(WINAPI*)(DWORD,LPCSTR,DWORD,DWORD,LPVOID))RealFunc("GetFileVersionInfoExA"); return fn?fn(fl,f,h,l,d):FALSE; }
BOOL  WINAPI GetFileVersionInfoExW(DWORD fl, LPCWSTR f, DWORD h, DWORD l, LPVOID d){ auto fn=(BOOL(WINAPI*)(DWORD,LPCWSTR,DWORD,DWORD,LPVOID))RealFunc("GetFileVersionInfoExW"); return fn?fn(fl,f,h,l,d):FALSE; }
BOOL  WINAPI VerQueryValueA(LPCVOID b, LPCSTR s, LPVOID* buf, PUINT len) { auto fn=(BOOL(WINAPI*)(LPCVOID,LPCSTR,LPVOID*,PUINT))RealFunc("VerQueryValueA"); return fn?fn(b,s,buf,len):FALSE; }
BOOL  WINAPI VerQueryValueW(LPCVOID b, LPCWSTR s, LPVOID* buf, PUINT len){ auto fn=(BOOL(WINAPI*)(LPCVOID,LPCWSTR,LPVOID*,PUINT))RealFunc("VerQueryValueW"); return fn?fn(b,s,buf,len):FALSE; }
DWORD WINAPI VerLanguageNameA(DWORD w, LPSTR s, DWORD c) { auto fn=(DWORD(WINAPI*)(DWORD,LPSTR,DWORD))RealFunc("VerLanguageNameA"); return fn?fn(w,s,c):0; }
DWORD WINAPI VerLanguageNameW(DWORD w, LPWSTR s, DWORD c){ auto fn=(DWORD(WINAPI*)(DWORD,LPWSTR,DWORD))RealFunc("VerLanguageNameW"); return fn?fn(w,s,c):0; }
// VerFindFile/VerInstallFile omitted (header-declared; games never import them from a proxy)
} // extern "C"
