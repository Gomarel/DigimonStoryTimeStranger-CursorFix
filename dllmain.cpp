// SPDX-License-Identifier: MIT
//
// DigimonCursorFix — ASI plugin (x64 DLL) for Digimon Story: Time Stranger
//
// The game keeps snapping the mouse cursor back to the center of the screen (SetCursorPos)
// on every load, menu and dialog. Under Wine/Proton on Wayland this causes lag and visual
// artifacts. The plugin hooks the Win32 cursor functions and:
//   - SetCursorPos                  -> does nothing, just returns TRUE (this is the actual fix),
//   - GetCursorPos / GetCursorInfo  -> report a fixed position (far off-screen by default),
//   - ShowCursor                    -> keeps the system cursor hidden (optional),
//   - ClipCursor                    -> does nothing (optional, off by default).
//
// Settings: <plugin name>.ini next to the .asi (defaults apply when it is missing).
// Log:      <plugin name>.log next to the .asi (overwritten on every game start).

#include <windows.h>
#include <cstdarg>
#include <cstdio>

#include "MinHook.h"

#define CURSORFIX_VERSION "1.0.0"

namespace
{
    // ---------------------------------------------------------------------
    // Settings
    // ---------------------------------------------------------------------

    enum class CursorMode : int
    {
        Offscreen = 1,   // fixed position from the INI (OffscreenX/OffscreenY)
        WindowCenter = 2 // center of the game window's client area (fallback: Offscreen)
    };

    struct Config
    {
        bool       hookGetCursorPos = true;
        bool       hookSetCursorPos = true;
        bool       hookShowCursor   = true;
        bool       hookClipCursor   = false;
        bool       hookGetCursorInfo = true;
        CursorMode mode       = CursorMode::Offscreen;
        int        offscreenX = -9999;
        int        offscreenY = -9999;
        bool       log        = true;
        int        logIntervalSeconds = 10;
    };

    Config g_config;
    HMODULE g_module = nullptr;
    wchar_t g_iniPath[MAX_PATH] = {};
    wchar_t g_logPath[MAX_PATH] = {};

    // ---------------------------------------------------------------------
    // Original functions (MinHook trampolines)
    // ---------------------------------------------------------------------

    using GetCursorPos_t  = BOOL(WINAPI*)(LPPOINT);
    using SetCursorPos_t  = BOOL(WINAPI*)(int, int);
    using ShowCursor_t    = int(WINAPI*)(BOOL);
    using ClipCursor_t    = BOOL(WINAPI*)(const RECT*);
    using GetCursorInfo_t = BOOL(WINAPI*)(PCURSORINFO);

    GetCursorPos_t  o_GetCursorPos  = nullptr;
    SetCursorPos_t  o_SetCursorPos  = nullptr;
    ShowCursor_t    o_ShowCursor    = nullptr;
    ClipCursor_t    o_ClipCursor    = nullptr;
    GetCursorInfo_t o_GetCursorInfo = nullptr;

    // Call counters — diagnostics only (which functions the game calls and how often).
    volatile LONG g_cntGetCursorPos  = 0;
    volatile LONG g_cntSetCursorPos  = 0;
    volatile LONG g_cntShowCursor    = 0;
    volatile LONG g_cntClipCursor    = 0;
    volatile LONG g_cntGetCursorInfo = 0;

    // ---------------------------------------------------------------------
    // Log
    // ---------------------------------------------------------------------

    HANDLE  g_logFile = INVALID_HANDLE_VALUE;
    SRWLOCK g_logLock = SRWLOCK_INIT;

    void LogOpen()
    {
        if (!g_config.log || g_logPath[0] == L'\0')
            return;

        g_logFile = CreateFileW(g_logPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    void Log(const char* format, ...)
    {
        if (g_logFile == INVALID_HANDLE_VALUE)
            return;

        char line[1024];
        SYSTEMTIME now;
        GetLocalTime(&now);
        int prefix = snprintf(line, sizeof(line), "[%02u:%02u:%02u.%03u] ",
                              now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
        if (prefix < 0)
            return;

        va_list args;
        va_start(args, format);
        int body = vsnprintf(line + prefix, sizeof(line) - prefix - 2, format, args);
        va_end(args);
        if (body < 0)
            return;

        size_t length = strnlen(line, sizeof(line) - 2);
        line[length++] = '\r';
        line[length++] = '\n';

        AcquireSRWLockExclusive(&g_logLock);
        DWORD written = 0;
        WriteFile(g_logFile, line, static_cast<DWORD>(length), &written, nullptr);
        ReleaseSRWLockExclusive(&g_logLock);
    }

    // Converts a wide string (a path) to UTF-8 for the log.
    void ToUtf8(const wchar_t* text, char* buffer, int bufferSize)
    {
        if (WideCharToMultiByte(CP_UTF8, 0, text, -1, buffer, bufferSize, nullptr, nullptr) == 0)
            buffer[0] = '\0';
    }

    // ---------------------------------------------------------------------
    // Helpers
    // ---------------------------------------------------------------------

    // The INI and log paths are derived from the location of the .asi itself —
    // we do not rely on the process working directory.
    bool BuildPaths()
    {
        wchar_t modulePath[MAX_PATH] = {};
        DWORD length = GetModuleFileNameW(g_module, modulePath, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
            return false;

        wchar_t* dot = wcsrchr(modulePath, L'.');
        wchar_t* slash = wcsrchr(modulePath, L'\\');
        if (dot == nullptr || (slash != nullptr && dot < slash))
            return false;
        *dot = L'\0';

        if (swprintf(g_iniPath, MAX_PATH, L"%s.ini", modulePath) < 0)
            return false;
        if (swprintf(g_logPath, MAX_PATH, L"%s.log", modulePath) < 0)
            return false;
        return true;
    }

    // When the INI does not exist, GetPrivateProfileIntW returns the default value.
    int IniInt(const wchar_t* key, int defaultValue)
    {
        if (g_iniPath[0] == L'\0')
            return defaultValue;
        return static_cast<int>(GetPrivateProfileIntW(L"CursorFix", key, defaultValue, g_iniPath));
    }

    void LoadConfig()
    {
        Config defaults;
        g_config.hookGetCursorPos  = IniInt(L"HookGetCursorPos",  defaults.hookGetCursorPos)  != 0;
        g_config.hookSetCursorPos  = IniInt(L"HookSetCursorPos",  defaults.hookSetCursorPos)  != 0;
        g_config.hookShowCursor    = IniInt(L"HookShowCursor",    defaults.hookShowCursor)    != 0;
        g_config.hookClipCursor    = IniInt(L"HookClipCursor",    defaults.hookClipCursor)    != 0;
        g_config.hookGetCursorInfo = IniInt(L"HookGetCursorInfo", defaults.hookGetCursorInfo) != 0;
        g_config.offscreenX = IniInt(L"OffscreenX", defaults.offscreenX);
        g_config.offscreenY = IniInt(L"OffscreenY", defaults.offscreenY);
        g_config.log        = IniInt(L"Log", defaults.log) != 0;

        int mode = IniInt(L"Mode", static_cast<int>(defaults.mode));
        g_config.mode = (mode == static_cast<int>(CursorMode::WindowCenter))
                            ? CursorMode::WindowCenter
                            : CursorMode::Offscreen;

        int interval = IniInt(L"LogIntervalSeconds", defaults.logIntervalSeconds);
        g_config.logIntervalSeconds = (interval < 1) ? 1 : interval;
    }

    // The position reported to the game instead of the real mouse position.
    POINT FakeCursorPosition()
    {
        POINT point = { g_config.offscreenX, g_config.offscreenY };

        if (g_config.mode == CursorMode::WindowCenter)
        {
            HWND window = GetForegroundWindow();
            DWORD processId = 0;
            if (window != nullptr)
                GetWindowThreadProcessId(window, &processId);

            // Only when the game's own window is in the foreground; otherwise stay Offscreen.
            RECT client;
            if (processId == GetCurrentProcessId() && GetClientRect(window, &client))
            {
                POINT center = { (client.right - client.left) / 2, (client.bottom - client.top) / 2 };
                if (ClientToScreen(window, &center))
                    point = center;
            }
        }

        return point;
    }

    // ---------------------------------------------------------------------
    // Hooks
    // ---------------------------------------------------------------------

    BOOL WINAPI Hook_GetCursorPos(LPPOINT lpPoint)
    {
        InterlockedIncrement(&g_cntGetCursorPos);

        if (lpPoint == nullptr)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }

        *lpPoint = FakeCursorPosition();
        return TRUE;
    }

    BOOL WINAPI Hook_SetCursorPos(int /*x*/, int /*y*/)
    {
        InterlockedIncrement(&g_cntSetCursorPos);
        return TRUE; // pretend success, leave the cursor where it is
    }

    int WINAPI Hook_ShowCursor(BOOL /*bShow*/)
    {
        InterlockedIncrement(&g_cntShowCursor);

        // ShowCursor works with a counter: the cursor is visible while it is >= 0.
        // Whatever the game asks for, push the counter to -1 (hidden) and keep it from going lower.
        int counter = o_ShowCursor(FALSE);
        while (counter < -1)
            counter = o_ShowCursor(TRUE);
        return counter;
    }

    BOOL WINAPI Hook_ClipCursor(const RECT* /*lpRect*/)
    {
        InterlockedIncrement(&g_cntClipCursor);
        return TRUE; // ignore confining the cursor to the window
    }

    BOOL WINAPI Hook_GetCursorInfo(PCURSORINFO pci)
    {
        InterlockedIncrement(&g_cntGetCursorInfo);

        BOOL result = o_GetCursorInfo(pci);
        if (result && pci != nullptr)
            pci->ptScreenPos = FakeCursorPosition(); // keep the real flags/hCursor
        return result;
    }

    // ---------------------------------------------------------------------
    // Hook installation
    // ---------------------------------------------------------------------

    template <typename T>
    bool InstallHook(const char* name, LPVOID detour, T* original)
    {
        // MH_CreateHookApiEx resolves the function via GetProcAddress in user32.dll,
        // so we hook the real function, not our own module's import thunk.
        LPVOID target = nullptr;
        MH_STATUS status = MH_CreateHookApiEx(L"user32", name, detour,
                                              reinterpret_cast<LPVOID*>(original), &target);
        if (status != MH_OK)
        {
            Log("  %-14s ERROR MH_CreateHookApiEx: %s", name, MH_StatusToString(status));
            return false;
        }

        status = MH_EnableHook(target);
        if (status != MH_OK)
        {
            Log("  %-14s ERROR MH_EnableHook: %s", name, MH_StatusToString(status));
            return false;
        }

        Log("  %-14s OK (0x%p)", name, target);
        return true;
    }

    void LogEnvironment()
    {
        char buffer[MAX_PATH * 3];

        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        ToUtf8(exePath, buffer, sizeof(buffer));
        Log("Process:  %s (PID %lu)", buffer, GetCurrentProcessId());

        ToUtf8(g_iniPath, buffer, sizeof(buffer));
        bool iniExists = GetFileAttributesW(g_iniPath) != INVALID_FILE_ATTRIBUTES;
        Log("INI:      %s (%s)", buffer, iniExists ? "found" : "not found, using defaults");

        // Wine/Proton exports wine_get_version from ntdll; it does not exist on Windows.
        using wine_get_version_t = const char*(__cdecl*)();
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        auto wineGetVersion = ntdll != nullptr
            ? reinterpret_cast<wine_get_version_t>(GetProcAddress(ntdll, "wine_get_version"))
            : nullptr;
        Log("Wine:     %s", wineGetVersion != nullptr ? wineGetVersion() : "no (native Windows)");

        Log("Settings: Mode=%d OffscreenX=%d OffscreenY=%d LogIntervalSeconds=%d",
            static_cast<int>(g_config.mode), g_config.offscreenX, g_config.offscreenY,
            g_config.logIntervalSeconds);
    }

    // Every LogIntervalSeconds writes the call counts (only when they changed).
    // The log then shows which functions the game actually uses.
    [[noreturn]] void StatsLoop()
    {
        LONG last[5] = { -1, -1, -1, -1, -1 };
        for (;;)
        {
            Sleep(static_cast<DWORD>(g_config.logIntervalSeconds) * 1000);

            LONG now[5] = { g_cntGetCursorPos, g_cntSetCursorPos, g_cntShowCursor,
                            g_cntClipCursor, g_cntGetCursorInfo };
            if (memcmp(now, last, sizeof(now)) != 0)
            {
                Log("Total calls: GetCursorPos=%ld SetCursorPos=%ld ShowCursor=%ld "
                    "ClipCursor=%ld GetCursorInfo=%ld",
                    now[0], now[1], now[2], now[3], now[4]);
                memcpy(last, now, sizeof(now));
            }
        }
    }

    DWORD WINAPI InitThread(LPVOID /*parameter*/)
    {
        if (!BuildPaths())
            g_iniPath[0] = g_logPath[0] = L'\0'; // no paths: default settings, no log

        LoadConfig();
        LogOpen();
        Log("DigimonCursorFix " CURSORFIX_VERSION " — start");
        LogEnvironment();

        MH_STATUS status = MH_Initialize();
        if (status != MH_OK)
        {
            Log("ERROR MH_Initialize: %s — the plugin does nothing.", MH_StatusToString(status));
            return 0;
        }

        Log("Hooks:");
        int installed = 0;
        if (g_config.hookGetCursorPos)
            installed += InstallHook("GetCursorPos", &Hook_GetCursorPos, &o_GetCursorPos);
        if (g_config.hookSetCursorPos)
            installed += InstallHook("SetCursorPos", &Hook_SetCursorPos, &o_SetCursorPos);
        if (g_config.hookShowCursor)
            installed += InstallHook("ShowCursor", &Hook_ShowCursor, &o_ShowCursor);
        if (g_config.hookClipCursor)
            installed += InstallHook("ClipCursor", &Hook_ClipCursor, &o_ClipCursor);
        if (g_config.hookGetCursorInfo)
            installed += InstallHook("GetCursorInfo", &Hook_GetCursorInfo, &o_GetCursorInfo);
        Log("Active hooks: %d", installed);

        if (installed == 0 || g_logFile == INVALID_HANDLE_VALUE)
            return 0;

        StatsLoop();
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*reserved*/)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = hModule;
        DisableThreadLibraryCalls(hModule);

        // Pin the module so nobody can unload it while the hooks and the stats thread
        // are running — after FreeLibrary they would jump into unmapped memory.
        HMODULE pinned = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(&DllMain), &pinned);

        // DllMain runs under the loader lock — install the hooks from a separate thread.
        HANDLE thread = CreateThread(nullptr, 0, &InitThread, nullptr, 0, nullptr);
        if (thread != nullptr)
            CloseHandle(thread);
    }

    // DLL_PROCESS_DETACH: no cleanup. The module is pinned, so we only get here when the
    // process exits, and the system releases everything anyway.
    return TRUE;
}
