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
// MouseCamera=1 (off by default) keeps the mouse camera working instead:
// the game has two modes and calls one of these functions every frame —
//   - camera mode (world):           SetCursorPos(center), the camera turns by the distance moved,
//   - cursor mode (menus, battles):  ClipCursor(NULL), the in-game cursor follows the mouse.
// The plugin lets the game re-center the cursor and only watches the switch between modes:
// on entering cursor mode it puts the cursor back where it was left the last time
// (or on a fixed position), so it no longer pops up in the center of the screen.
//
// Diagnostics (off by default):
//   - Observe=1  -> the hooks only count and log, every call goes to the original function,
//   - Trace=1    -> logs every SetCursorPos call with its caller, watches raw input,
//   - MarkerKey  -> pressing this key writes a numbered marker into the log.
//
// Settings: <plugin name>.ini next to the .asi (defaults apply when it is missing).
// Log:      <plugin name>.log next to the .asi (overwritten on every game start).

#include <windows.h>
#include <intrin.h>
#include <cstdarg>
#include <cstdio>

#include "MinHook.h"

#pragma intrinsic(_ReturnAddress)

#define CURSORFIX_VERSION "1.1.0"

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

    enum class MenuCursor : int
    {
        LastPosition = 1, // where the cursor was left the last time the in-game cursor was shown
        FixedPosition = 2 // MenuCursorX/MenuCursorY, in percent of the game window
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
        bool       mouseCamera = false;
        MenuCursor menuCursor  = MenuCursor::LastPosition;
        int        menuCursorX = 100; // percent of the game window width
        int        menuCursorY = 100; // percent of the game window height
        bool       observe    = false;
        bool       trace      = false;
        int        markerKey  = 0; // virtual-key code, 0 = off
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
    using RegisterRawInputDevices_t = BOOL(WINAPI*)(PCRAWINPUTDEVICE, UINT, UINT);
    using GetRawInputData_t   = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
    using GetRawInputBuffer_t = UINT(WINAPI*)(PRAWINPUT, PUINT, UINT);

    GetCursorPos_t  o_GetCursorPos  = nullptr;
    SetCursorPos_t  o_SetCursorPos  = nullptr;
    ShowCursor_t    o_ShowCursor    = nullptr;
    ClipCursor_t    o_ClipCursor    = nullptr;
    GetCursorInfo_t o_GetCursorInfo = nullptr;
    RegisterRawInputDevices_t o_RegisterRawInputDevices = nullptr;
    GetRawInputData_t   o_GetRawInputData   = nullptr;
    GetRawInputBuffer_t o_GetRawInputBuffer = nullptr;

    // Call counters — diagnostics only (which functions the game calls and how often).
    volatile LONG g_cntGetCursorPos  = 0;
    volatile LONG g_cntSetCursorPos  = 0;
    volatile LONG g_cntShowCursor    = 0;
    volatile LONG g_cntClipCursor    = 0;
    volatile LONG g_cntGetCursorInfo = 0;
    volatile LONG g_cntRawInputData  = 0;
    volatile LONG g_cntRawInputMouse = 0; // GetRawInputData calls that returned mouse data
    volatile LONG g_cntRawInputBuffer = 0;

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

    // Describes a code address as "module+0xoffset", e.g. "Digimon Story Time Stranger.exe+0x1a2b3c".
    // The offset stays the same between game starts, so it identifies the place in the game code.
    void DescribeAddress(const void* address, char* buffer, int bufferSize)
    {
        HMODULE module = nullptr;
        wchar_t path[MAX_PATH] = {};
        if (address == nullptr
            || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   static_cast<LPCWSTR>(address), &module)
            || GetModuleFileNameW(module, path, MAX_PATH) == 0)
        {
            snprintf(buffer, bufferSize, "0x%p", address);
            return;
        }

        const wchar_t* name = wcsrchr(path, L'\\');
        char utf8Name[MAX_PATH * 3];
        ToUtf8(name != nullptr ? name + 1 : path, utf8Name, sizeof(utf8Name));
        snprintf(buffer, bufferSize, "%s+0x%llx", utf8Name,
                 static_cast<unsigned long long>(static_cast<const char*>(address) - reinterpret_cast<const char*>(module)));
    }

    // ---------------------------------------------------------------------
    // Callers (Trace=1): which places in the game code call a function and how often
    // ---------------------------------------------------------------------

    struct CallerSlot
    {
        void* volatile address;
        volatile LONG  count;
    };

    constexpr int kCallerSlots = 16;
    CallerSlot g_callersGetCursorPos[kCallerSlots] = {};
    CallerSlot g_callersSetCursorPos[kCallerSlots] = {};

    void RecordCaller(CallerSlot* slots, const char* function, void* caller)
    {
        for (int i = 0; i < kCallerSlots; ++i)
        {
            void* current = slots[i].address;
            if (current == nullptr)
            {
                current = InterlockedCompareExchangePointer(&slots[i].address, caller, nullptr);
                if (current == nullptr)
                {
                    char where[MAX_PATH * 3];
                    DescribeAddress(caller, where, sizeof(where));
                    Log("New caller of %s: %s", function, where);
                    current = caller;
                }
            }
            if (current == caller)
            {
                InterlockedIncrement(&slots[i].count);
                return;
            }
        }
        // all slots taken: the call is still counted in the totals
    }

    void LogCallers(const CallerSlot* slots, const char* function)
    {
        for (int i = 0; i < kCallerSlots && slots[i].address != nullptr; ++i)
        {
            char where[MAX_PATH * 3];
            DescribeAddress(slots[i].address, where, sizeof(where));
            Log("  %s caller %s: %ld calls", function, where, slots[i].count);
        }
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
        g_config.observe    = IniInt(L"Observe", defaults.observe) != 0;
        g_config.trace      = IniInt(L"Trace", defaults.trace) != 0;
        g_config.markerKey  = IniInt(L"MarkerKey", defaults.markerKey);
        g_config.mouseCamera = IniInt(L"MouseCamera", defaults.mouseCamera) != 0;

        int menuCursor = IniInt(L"MenuCursor", static_cast<int>(defaults.menuCursor));
        g_config.menuCursor = (menuCursor == static_cast<int>(MenuCursor::FixedPosition))
                                  ? MenuCursor::FixedPosition
                                  : MenuCursor::LastPosition;

        auto percent = [](int value) { return value < 0 ? 0 : (value > 100 ? 100 : value); };
        g_config.menuCursorX = percent(IniInt(L"MenuCursorX", defaults.menuCursorX));
        g_config.menuCursorY = percent(IniInt(L"MenuCursorY", defaults.menuCursorY));

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

    // The real cursor position, bypassing our own GetCursorPos hook.
    POINT RealCursorPosition()
    {
        POINT point = { 0, 0 };
        if (o_GetCursorPos != nullptr)
            o_GetCursorPos(&point);
        else
            GetCursorPos(&point);
        return point;
    }

    // Client area of the game window in screen coordinates — only when the game window is in the foreground.
    bool GameWindowArea(RECT& area)
    {
        HWND window = GetForegroundWindow();
        DWORD processId = 0;
        if (window != nullptr)
            GetWindowThreadProcessId(window, &processId);

        RECT client;
        POINT origin = { 0, 0 };
        if (processId != GetCurrentProcessId() || !GetClientRect(window, &client) || !ClientToScreen(window, &origin))
            return false;

        area = { origin.x, origin.y, origin.x + client.right, origin.y + client.bottom };
        return true;
    }

    // ---------------------------------------------------------------------
    // MouseCamera=1: tracking the game's camera/cursor mode
    // ---------------------------------------------------------------------

    enum GameMode : LONG
    {
        ModeUnknown = 0,
        ModeCamera  = 1, // world: SetCursorPos(center) every frame
        ModeCursor  = 2  // menus, battles: ClipCursor(NULL) every frame
    };

    volatile LONG g_gameMode = ModeUnknown;
    POINT g_menuCursor = { 0, 0 };
    bool  g_menuCursorSaved = false;

    // Called from the SetCursorPos hook, before the call goes to the original function.
    void OnCameraModeFrame()
    {
        LONG previous = InterlockedExchange(&g_gameMode, ModeCamera);
        if (previous == ModeCamera)
            return;

        // Leaving cursor mode: remember where the player left the cursor.
        if (previous == ModeCursor)
        {
            g_menuCursor = RealCursorPosition();
            g_menuCursorSaved = true;
            Log("Mode: cursor -> camera, cursor left at %ld,%ld", g_menuCursor.x, g_menuCursor.y);
        }
        else
        {
            Log("Mode: camera");
        }
    }

    // Called from the ClipCursor(NULL) hook, after the call went to the original function.
    void OnCursorModeFrame()
    {
        LONG previous = InterlockedExchange(&g_gameMode, ModeCursor);
        if (previous == ModeCursor)
            return;

        if (previous != ModeCamera)
        {
            Log("Mode: cursor");
            return;
        }

        // Entering cursor mode from camera mode: the cursor sits in the center because of the camera.
        RECT area;
        if (!GameWindowArea(area))
        {
            Log("Mode: camera -> cursor, game window not in foreground, cursor not moved");
            return;
        }

        POINT target;
        if (g_config.menuCursor == MenuCursor::FixedPosition)
        {
            target.x = area.left + (area.right - area.left - 1) * g_config.menuCursorX / 100;
            target.y = area.top + (area.bottom - area.top - 1) * g_config.menuCursorY / 100;
        }
        else if (g_menuCursorSaved)
        {
            target = g_menuCursor;
        }
        else
        {
            Log("Mode: camera -> cursor, no saved cursor position yet, cursor not moved");
            return;
        }

        if (g_config.observe)
        {
            Log("Mode: camera -> cursor, would move cursor to %ld,%ld (Observe=1, not moved)", target.x, target.y);
            return;
        }

        BOOL moved = o_SetCursorPos(target.x, target.y);
        Log("Mode: camera -> cursor, cursor moved to %ld,%ld%s", target.x, target.y, moved ? "" : " (FAILED)");
    }

    // ---------------------------------------------------------------------
    // Hooks
    // ---------------------------------------------------------------------

    BOOL WINAPI Hook_GetCursorPos(LPPOINT lpPoint)
    {
        InterlockedIncrement(&g_cntGetCursorPos);
        if (g_config.trace)
            RecordCaller(g_callersGetCursorPos, "GetCursorPos", _ReturnAddress());

        // The mouse camera measures how far the real cursor moved from the center.
        if (g_config.observe || g_config.mouseCamera)
            return o_GetCursorPos(lpPoint);

        if (lpPoint == nullptr)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }

        *lpPoint = FakeCursorPosition();
        return TRUE;
    }

    BOOL WINAPI Hook_SetCursorPos(int x, int y)
    {
        InterlockedIncrement(&g_cntSetCursorPos);
        if (g_config.trace)
        {
            void* caller = _ReturnAddress();
            RecordCaller(g_callersSetCursorPos, "SetCursorPos", caller);

            // "from" = where the cursor really was, i.e. how far the mouse moved since the last call.
            POINT from = RealCursorPosition();
            char where[MAX_PATH * 3];
            DescribeAddress(caller, where, sizeof(where));
            Log("SetCursorPos(%d,%d) from %ld,%ld caller %s", x, y, from.x, from.y, where);
        }

        if (g_config.mouseCamera)
            OnCameraModeFrame();

        if (g_config.observe || g_config.mouseCamera)
            return o_SetCursorPos(x, y);

        return TRUE; // pretend success, leave the cursor where it is
    }

    int WINAPI Hook_ShowCursor(BOOL bShow)
    {
        InterlockedIncrement(&g_cntShowCursor);

        if (g_config.observe)
        {
            int result = o_ShowCursor(bShow);
            if (g_config.trace)
                Log("ShowCursor(%s) -> %d", bShow ? "TRUE" : "FALSE", result);
            return result;
        }

        // ShowCursor works with a counter: the cursor is visible while it is >= 0.
        // Whatever the game asks for, push the counter to -1 (hidden) and keep it from going lower.
        int counter = o_ShowCursor(FALSE);
        while (counter < -1)
            counter = o_ShowCursor(TRUE);
        return counter;
    }

    BOOL WINAPI Hook_ClipCursor(const RECT* lpRect)
    {
        InterlockedIncrement(&g_cntClipCursor);

        if (g_config.trace)
        {
            if (lpRect != nullptr)
                Log("ClipCursor(%ld,%ld - %ld,%ld)", lpRect->left, lpRect->top, lpRect->right, lpRect->bottom);
            else
                Log("ClipCursor(NULL)");
        }

        if (g_config.mouseCamera)
        {
            BOOL result = o_ClipCursor(lpRect);
            if (lpRect == nullptr)
                OnCursorModeFrame();
            return result;
        }

        if (g_config.observe)
            return o_ClipCursor(lpRect);

        return TRUE; // ignore confining the cursor to the window
    }

    BOOL WINAPI Hook_GetCursorInfo(PCURSORINFO pci)
    {
        InterlockedIncrement(&g_cntGetCursorInfo);

        BOOL result = o_GetCursorInfo(pci);
        if (result && pci != nullptr && !g_config.observe && !g_config.mouseCamera)
            pci->ptScreenPos = FakeCursorPosition(); // keep the real flags/hCursor
        return result;
    }

    // Raw input hooks (Trace=1 only) never change anything, they just show whether
    // the game reads mouse movement through raw input.
    BOOL WINAPI Hook_RegisterRawInputDevices(PCRAWINPUTDEVICE devices, UINT count, UINT size)
    {
        BOOL result = o_RegisterRawInputDevices(devices, count, size);
        Log("RegisterRawInputDevices(%u devices) -> %s", count, result ? "TRUE" : "FALSE");
        for (UINT i = 0; devices != nullptr && size == sizeof(RAWINPUTDEVICE) && i < count; ++i)
        {
            Log("  usagePage=0x%02x usage=0x%02x flags=0x%08lx hwnd=0x%p%s",
                devices[i].usUsagePage, devices[i].usUsage, devices[i].dwFlags, devices[i].hwndTarget,
                (devices[i].usUsagePage == 0x01 && devices[i].usUsage == 0x02) ? "  (mouse)" : "");
        }
        return result;
    }

    UINT WINAPI Hook_GetRawInputData(HRAWINPUT input, UINT command, LPVOID data, PUINT size, UINT headerSize)
    {
        InterlockedIncrement(&g_cntRawInputData);

        UINT result = o_GetRawInputData(input, command, data, size, headerSize);
        if (command == RID_INPUT && data != nullptr && result != static_cast<UINT>(-1)
            && result >= sizeof(RAWINPUTHEADER)
            && static_cast<const RAWINPUTHEADER*>(data)->dwType == RIM_TYPEMOUSE)
        {
            InterlockedIncrement(&g_cntRawInputMouse);
        }
        return result;
    }

    UINT WINAPI Hook_GetRawInputBuffer(PRAWINPUT data, PUINT size, UINT headerSize)
    {
        InterlockedIncrement(&g_cntRawInputBuffer);
        return o_GetRawInputBuffer(data, size, headerSize);
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
        if (g_config.mouseCamera)
        {
            Log("MouseCamera: on, MenuCursor=%d MenuCursorX=%d%% MenuCursorY=%d%%",
                static_cast<int>(g_config.menuCursor), g_config.menuCursorX, g_config.menuCursorY);
        }
        if (g_config.observe || g_config.trace || g_config.markerKey != 0)
        {
            Log("Diagnostics: Observe=%d Trace=%d MarkerKey=0x%02x%s",
                g_config.observe, g_config.trace, g_config.markerKey,
                g_config.observe ? "  (hooks only watch, the game behaves as without the plugin)" : "");
        }
    }

    // The game window (if it is in the foreground) and the real cursor position — written with each marker.
    void LogWindowState()
    {
        POINT cursor = RealCursorPosition();
        HWND window = GetForegroundWindow();
        DWORD processId = 0;
        if (window != nullptr)
            GetWindowThreadProcessId(window, &processId);

        RECT client;
        if (processId != GetCurrentProcessId() || !GetClientRect(window, &client))
        {
            Log("  game window not in foreground, cursor %ld,%ld", cursor.x, cursor.y);
            return;
        }

        POINT origin = { 0, 0 };
        ClientToScreen(window, &origin);
        Log("  game window %ldx%ld at %ld,%ld (center %ld,%ld), cursor %ld,%ld",
            client.right, client.bottom, origin.x, origin.y,
            origin.x + client.right / 2, origin.y + client.bottom / 2, cursor.x, cursor.y);
    }

    // Polls the marker key; each press writes "===== MARKER n =====" into the log.
    DWORD WINAPI MarkerThread(LPVOID /*parameter*/)
    {
        int marker = 0;
        bool wasDown = false;
        for (;;)
        {
            bool isDown = (GetAsyncKeyState(g_config.markerKey) & 0x8000) != 0;
            if (isDown && !wasDown)
            {
                Log("===== MARKER %d =====", ++marker);
                LogWindowState();
                if (g_config.trace)
                {
                    LogCallers(g_callersGetCursorPos, "GetCursorPos");
                    LogCallers(g_callersSetCursorPos, "SetCursorPos");
                }
            }
            wasDown = isDown;
            Sleep(15);
        }
    }

    // Every LogIntervalSeconds writes the call counts (only when they changed).
    // The log then shows which functions the game actually uses.
    [[noreturn]] void StatsLoop()
    {
        constexpr int kCounters = 8;
        LONG last[kCounters] = { -1, -1, -1, -1, -1, -1, -1, -1 };
        for (;;)
        {
            Sleep(static_cast<DWORD>(g_config.logIntervalSeconds) * 1000);

            LONG now[kCounters] = { g_cntGetCursorPos, g_cntSetCursorPos, g_cntShowCursor,
                                    g_cntClipCursor, g_cntGetCursorInfo,
                                    g_cntRawInputData, g_cntRawInputMouse, g_cntRawInputBuffer };
            if (memcmp(now, last, sizeof(now)) != 0)
            {
                Log("Total calls: GetCursorPos=%ld SetCursorPos=%ld ShowCursor=%ld "
                    "ClipCursor=%ld GetCursorInfo=%ld",
                    now[0], now[1], now[2], now[3], now[4]);
                if (g_config.trace)
                {
                    Log("Raw input:   GetRawInputData=%ld (mouse %ld) GetRawInputBuffer=%ld",
                        now[5], now[6], now[7]);
                }
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
        // MouseCamera=1 recognizes the game's mode from SetCursorPos and ClipCursor, so it needs both hooks.
        if (g_config.hookSetCursorPos || g_config.mouseCamera)
            installed += InstallHook("SetCursorPos", &Hook_SetCursorPos, &o_SetCursorPos);
        if (g_config.hookShowCursor)
            installed += InstallHook("ShowCursor", &Hook_ShowCursor, &o_ShowCursor);
        if (g_config.hookClipCursor || g_config.mouseCamera)
            installed += InstallHook("ClipCursor", &Hook_ClipCursor, &o_ClipCursor);
        if (g_config.hookGetCursorInfo)
            installed += InstallHook("GetCursorInfo", &Hook_GetCursorInfo, &o_GetCursorInfo);
        if (g_config.trace)
        {
            installed += InstallHook("RegisterRawInputDevices", &Hook_RegisterRawInputDevices, &o_RegisterRawInputDevices);
            installed += InstallHook("GetRawInputData", &Hook_GetRawInputData, &o_GetRawInputData);
            installed += InstallHook("GetRawInputBuffer", &Hook_GetRawInputBuffer, &o_GetRawInputBuffer);
        }
        Log("Active hooks: %d", installed);

        if (g_logFile == INVALID_HANDLE_VALUE)
            return 0;

        if (g_config.markerKey != 0)
        {
            HANDLE thread = CreateThread(nullptr, 0, &MarkerThread, nullptr, 0, nullptr);
            if (thread != nullptr)
                CloseHandle(thread);
        }

        if (installed == 0)
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

        // Pin the module so nobody can unload it while the hooks and the worker threads
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
