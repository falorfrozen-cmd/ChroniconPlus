// ChroniconPlus loader: a version.dll that Chronicon.exe imports from its own folder (adapted from EpochPact).
//
// It forwards every export to the real System32 version.dll (version_thunks.asm jumps through g_real) and, only
// inside Chronicon.exe, starts the core from <game>\ChroniconPlus\ChroniconPlus.Core.dll on a thread of its own.
// The loader never touches the game; a file named <game>\ChroniconPlus\disabled keeps the core from loading.
// Each step is written to <game>\ChroniconPlus\loader.log.

#include <windows.h>
#include <winver.h>

#include <cstdarg>
#include <cstdio>

extern "C" void* g_real[15] = {};
static INIT_ONCE g_forwarding = INIT_ONCE_STATIC_INIT;
static HINSTANCE g_self = nullptr;

static const char* const kRealNames[15] = {
    "GetFileVersionInfoA",     "GetFileVersionInfoByHandle", "GetFileVersionInfoExA",
    "GetFileVersionInfoExW",   "GetFileVersionInfoSizeA",    "GetFileVersionInfoSizeExA",
    "GetFileVersionInfoSizeExW", "GetFileVersionInfoSizeW",  "GetFileVersionInfoW",
    "VerFindFileA",            "VerFindFileW",               "VerInstallFileA",
    "VerInstallFileW",         "VerQueryValueA",             "VerQueryValueW",
};

// The folder of the host executable, with a trailing backslash; false if it does not fit.
static bool HostDir(wchar_t* out, DWORD cap, const wchar_t** fileName) {
    DWORD n = GetModuleFileNameW(nullptr, out, cap);
    if (n == 0 || n >= cap) return false;
    wchar_t* slash = wcsrchr(out, L'\\');
    if (!slash) return false;
    *fileName = slash + 1;
    return true;
}

static bool IsGameHost() {
    wchar_t host[MAX_PATH];
    const wchar_t* exe = nullptr;
    return HostDir(host, MAX_PATH, &exe) && _wcsicmp(exe, L"Chronicon.exe") == 0;
}

static void Trace(const wchar_t* fmt, ...) {
    wchar_t text[1024];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(text, _TRUNCATE, fmt, args);
    va_end(args);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t line[1200];
    _snwprintf_s(line, _TRUNCATE, L"%04u-%02u-%02u %02u:%02u:%02u.%03u pid %lu: %s\r\n", t.wYear, t.wMonth, t.wDay,
                 t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentProcessId(), text);
    char utf8[4096];
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof utf8, nullptr, nullptr);
    if (bytes <= 1) return;

    wchar_t path[MAX_PATH];
    const wchar_t* exe = nullptr;
    if (!HostDir(path, MAX_PATH, &exe)) return;
    *const_cast<wchar_t*>(exe) = L'\0';
    if (wcscat_s(path, L"ChroniconPlus")) return;
    CreateDirectoryW(path, nullptr);
    if (wcscat_s(path, L"\\loader.log")) return;
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, utf8, static_cast<DWORD>(bytes - 1), &written, nullptr);
    CloseHandle(file);
}

static DWORD WINAPI LoadCore() {
    wchar_t path[MAX_PATH];
    const wchar_t* exe = nullptr;
    if (!HostDir(path, MAX_PATH, &exe)) {
        Trace(L"the game path is too long; core not loaded");
        return 0;
    }
    *const_cast<wchar_t*>(exe) = L'\0';

    wchar_t probe[MAX_PATH];
    if (wcscpy_s(probe, path) || wcscat_s(probe, L"ChroniconPlus\\disabled")) return 0;
    if (GetFileAttributesW(probe) != INVALID_FILE_ATTRIBUTES) {
        Trace(L"ChroniconPlus\\disabled exists; core not loaded");
        return 0;
    }

    if (wcscat_s(path, L"ChroniconPlus\\ChroniconPlus.Core.dll")) return 0;
    // Dependencies only from beside the core or System32, never the current directory or PATH.
    HMODULE core = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!core) {
        Trace(L"core load failed: Windows error %lu", GetLastError());
        return 0;
    }
    using StartFn = void (*)();
    auto start = reinterpret_cast<StartFn>(GetProcAddress(core, "ChroniconPlus_Start"));
    if (!start) {
        Trace(L"core loaded, but ChroniconPlus_Start is missing");
        return 0;
    }
    start();
    Trace(L"core started; see ChroniconPlus.log");
    return 0;
}

static DWORD WINAPI StartCore(void* lease) {
    const DWORD result = LoadCore();
    FreeLibraryAndExitThread(static_cast<HMODULE>(lease), result);
}

// These exports have different failure conventions; never jump through nullptr.
extern "C" DWORD ep_VersionUnavailable() { SetLastError(ERROR_DLL_INIT_FAILED); return 0; }
extern "C" DWORD ep_FindUnavailable() { SetLastError(ERROR_DLL_INIT_FAILED); return VFF_BUFFTOOSMALL; }
extern "C" DWORD ep_InstallUnavailable() { SetLastError(ERROR_DLL_INIT_FAILED); return VIF_CANNOTREADSRC; }

static BOOL CALLBACK ResolveForwarding(PINIT_ONCE, PVOID, PVOID*) {
    void* functions[15]{};
    wchar_t sys[MAX_PATH];
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    HMODULE real = nullptr;
    bool ok = n != 0 && n < MAX_PATH && wcscat_s(sys, L"\\version.dll") == 0;
    if (ok) {
        real = LoadLibraryExW(sys, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        ok = real != nullptr;
    }
    for (int i = 0; ok && i < 15; ++i) {
        functions[i] = reinterpret_cast<void*>(GetProcAddress(real, kRealNames[i]));
        ok = functions[i] != nullptr;
    }
    if (!ok && real) FreeLibrary(real);
    for (int i = 0; i < 15; ++i)
        g_real[i] = ok ? functions[i] : reinterpret_cast<void*>(
            i == 9 || i == 10 ? &ep_FindUnavailable :
            i == 11 || i == 12 ? &ep_InstallUnavailable : &ep_VersionUnavailable);
    return TRUE;
}

extern "C" void* ep_ResolveVersion(unsigned index) {
    const DWORD saved = GetLastError();
    InitOnceExecuteOnce(&g_forwarding, ResolveForwarding, nullptr, nullptr);
    void* result = index < 15 ? g_real[index] : reinterpret_cast<void*>(&ep_VersionUnavailable);
    SetLastError(saved);
    return result;
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = self;
        DisableThreadLibraryCalls(self);
        // Only CreateThread here (no library loads); the thread runs after the loader lock is released.
        if (IsGameHost()) {
            HMODULE lease = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(g_self), &lease)) {
                HANDLE thread = CreateThread(nullptr, 0, StartCore, lease, 0, nullptr);
                if (thread) CloseHandle(thread);
                else FreeLibrary(lease);
            }
        }
    }
    return TRUE;
}
