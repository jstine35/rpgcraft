#include "PCH-rpgcraft.h"

#include "msw-redtape.h"

#include "x-string.h"
#include "x-gpu-ifc.h"
#include "x-thread.h"
#include "x-host-ifc.h"
#include "x-chrono.h"
#include "x-pad.h"
#include "fmod-ifc.h"

#include "appConfig.h"
#include "ajek-script.h"
#include "Scene.h"

#include "imgui.h"

#include <direct.h>     // for _getcwd()

DECLARE_MODULE_NAME("winmain");

extern void         LogHostInit();
extern void         MSW_InitChrono();
extern VirtKey_t    ConvertFromMswVK( UINT key );

HINSTANCE               g_hInst                 = nullptr;
HWND                    g_hWnd                  = nullptr;

static INT64            g_Time = 0;
static INT64            g_TicksPerSecond = 0;

static xCountedSemaphore    s_sem_SettingsDirtied;
static volatile s32         s_settings_dirty;

void MarkUserSettingsDirty()
{
    if (!AtomicExchange(s_settings_dirty, 1)) {
        s_sem_SettingsDirtied.PostIfNeeded();
    }
}

static bool s_isMinimized = false;
static bool s_isMaximized = false;
static bool s_msg_moved   = false;
static bool s_msg_sized   = false;

//--------------------------------------------------------------------------------------
// WINDOWS BOILERPLATE
//--------------------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    PAINTSTRUCT ps;
    HDC hdc;

    ImGuiIO& io = ImGui::GetIO();

    switch (msg)
    {
        case WM_MOVE: {
            if (!s_isMinimized) {
                MarkUserSettingsDirty();
            }
        } break;

        case WM_SIZE: {
            s_isMinimized = (wParam & SIZE_MINIMIZED);
            s_isMaximized = (wParam & SIZE_MAXIMIZED);
            if (!s_isMinimized) {
                MarkUserSettingsDirty();
            }
        } break;

        case WM_PAINT:
            hdc = BeginPaint(hWnd, &ps);
            EndPaint(hWnd, &ps);
        break;

        case WM_DESTROY:
            Scene_ShutdownThreads();
            dx11_CleanupDevice();
            PostQuitMessage(0);
        break;

        case WM_SYSKEYDOWN: {
            Scene_PostMessage(SceneMsg_KeyDown, ConvertFromMswVK(wParam));
            char c = MapVirtualKey (wParam, MAPVK_VK_TO_CHAR);
            auto mod = Host_GetKeyModifierInMsg();

            if ((c == 'W' || c == 'w') && mod.Alt()) {
                g_gpu_ForceWireframe = !g_gpu_ForceWireframe;
            }
        } break;

        case WM_KEYDOWN: {
            Scene_PostMessage(SceneMsg_KeyDown, ConvertFromMswVK(wParam));

            char c = MapVirtualKey (wParam, MAPVK_VK_TO_CHAR);

            if (c == 'R' || c == 'r') {
                Scene_PostMessage(SceneMsg_Reload, 0);
                Scene_PostMessage(SceneMsg_StartExec, SceneStopReason_ScriptError);
            }
        } break;

        case WM_KEYUP:
        case WM_SYSKEYUP:
            Scene_PostMessage(SceneMsg_KeyUp, ConvertFromMswVK(wParam));
        break;

        case WM_CHAR: {
            // UI-style User-input (mouse/keyboard), which use buffered input from Windows
            // instead of per-frame polling used by the gameplay input system.
            // You can also use ToAscii()+GetKeyboardState() to retrieve characters.
            if (wParam > 0 && wParam < 0x10000)
                Scene_PostMessage(SceneMsg_KeyChar, (unsigned short)wParam);
        } break;

        case WM_MOUSEWHEEL: {
            double delta = GET_WHEEL_DELTA_WPARAM(wParam) / 120.f;
            Scene_PostMessage(SceneMsg_MouseWheelDelta, (sptr&)delta);
        } break;


        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        {
            // Mouse capture is done via the message pump since it runs on its own "safe" thread
            // which is at low risk for deadlock and should be quick to respond to the user even
            // if the game loop is hung up for some reason.  Important since capturing the mouse
            // is sort of one of those "denial of UI service" things if it's not released. --jstine
            SetCapture(hWnd);
        } break;

        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
        case WM_MBUTTONUP:
        {
            // no value to track capture state here -- errant release messages are rare so just
            // call ReleaseCapture() unconditionally. --jstine
            ReleaseCapture();
        } break;

        default:
            return DefWindowProc(hWnd, msg, wParam, lParam);
    }

    return 0;
}

//std::vector<RECT>  s_msw_monitors;
//BOOL MonitorEnumerator(HMONITOR monitor, HDC hdc, LPRECT rect, LPARAM lparam)
//{
//  log_host("MonitorEnumerator: rect = %d, %d, %d, %d", rect->left, rect->top, rect->right, rect->bottom);
//  s_msw_monitors.push_back(*rect);
//  return true;
//}

template< typename T >
void table_get_xy(T& dest, LuaTableScope& table)
{
    auto xs = table.get_s32("x");
    auto ys = table.get_s32("y");

    if (xs.isnil()) { xs = table.get_s32(1); }
    if (ys.isnil()) { ys = table.get_s32(2); }

    if (xs.isnil() || ys.isnil()) {
        bug_qa("Invalid table layout for expected Int2 parameter.");
        // Todo: expose a ThrowError that pushes message onto Lua stack and invokes longjmp.
        // Pre-req: must have a single global lua state for all scene magic.
        //ThrowError
    }

    dest.x = xs;
    dest.y = ys;
}

template< typename T >
bool table_get_xy(T& dest, LuaTableScope& table, const char* subtable)
{
    bug_on(!subtable);
    if (auto& subtab = table.get_table(subtable)) {
        table_get_xy(dest, subtab);
        return true;
    }
    return false;
}

template< typename T >
void table_get_rect(T& dest, LuaTableScope& table)
{
    if (auto& subtab = table.get_table(1)) {
        table_get_xy(dest.xy, subtab);
    }

    if (auto& subtab = table.get_table(2)) {
        table_get_xy(dest.zw, subtab);
    }
}

static RECT s_lastknown_window_rect     = { };

static bool validate_window_pos(RECT& window, bool tryFix)
{
    // This logic mimics Win10's built-in movement restrictions, which aren't applied when using
    // ::ShowWindow() -- so we have to do it manually in case the inputs from the config file are
    // garbage.

    // * the "top" of a window should always be visible on load, because it's important for being
    //   able to grab and move the window.
    // * The rest of the window position isn't too important, however at least 240px or 1/3rd of
    //   the window along the X (horizontal) axis should be visble, otherwise it might be hard to
    //   grab the title bar.

    // get the work area of the monitor that best matches our window position:
    auto hMonitor = ::MonitorFromRect(&window, MONITOR_DEFAULTTONEAREST);
    MONITORINFO  mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfo(hMonitor, &mi);
    RECT best_monitor = mi.rcWork;

    // Determine the non-client area cleverly, without having to go through all the stupid
    // windows metrics APIs:

    RECT nonclientarea = { 0,0,1,1 };
    POINT titlebarSize = { 8, 8    };       // default assumption in case AdjustWindowRect() fails for any reaosn
    auto style = ::GetWindowLong(g_hWnd, GWL_STYLE);
    if (::AdjustWindowRect(&nonclientarea, style, FALSE)) {
        titlebarSize = { -nonclientarea.left, -nonclientarea.top };
    }

    RECT dest;
    RECT window_important_area = {  window.left, window.top, window.right, window.top + titlebarSize.y };
    int importantWidth = min(240, window.right-window.left);

    if (::IntersectRect(&dest, &window_important_area, &best_monitor)) {
        bool horizCheckOk = (dest.bottom - dest.top) >= titlebarSize.y;
        bool vertCheckOk  = (dest.right - dest.left) >= importantWidth;
        if (horizCheckOk && vertCheckOk) {
            // Seems OK, let the position through unmodified...
            return true;
        }
    }

    if (!tryFix) {
        return false;
    }

    POINT moveby = {
        best_monitor.left       - window.left,
        best_monitor.top        - window.top,
    };

    window.left     += moveby.x;
    window.top      += moveby.y;
    window.right    += moveby.x;
    window.bottom   += moveby.y;

    return true;
}

void ApplyDesktopSettings()
{
    auto& script = g_scriptEnv;
    if (auto& deskset = script.glob_open_table("UserSettings", false))
    {
        int2 client_pos     = { 64, 64 };
        int2 client_size    = { 1280, 720 };
        bool has_pos        = table_get_xy(client_pos,  deskset, "WindowClientPos");
        bool has_size       = table_get_xy(client_size, deskset, "WindowClientSize");

        RECT rc = { client_pos.x, client_pos.y, client_pos.x + client_size.u, client_pos.y + client_size.v };
        auto style = ::GetWindowLong(g_hWnd, GWL_STYLE);
        ::AdjustWindowRect(&rc, style, FALSE);

        // always validate window pos even if has_pos is false, since it also validates window size.
        validate_window_pos(rc, true);
        u32 posflags = SWP_NOOWNERZORDER | SWP_NOZORDER | (has_pos ? 0 : SWP_NOMOVE);
        ::SetWindowPos(g_hWnd, nullptr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, posflags);
    }
}

static void UpdateLastKnownWindowPosition()
{
    if (!s_isMinimized && !s_isMaximized) {
        RECT rc;
        ::GetClientRect(g_hWnd, &rc);

        POINT topleft           = { rc.left, rc.top };
        POINT bottomright       = { rc.right, rc.bottom };

        ::ClientToScreen(g_hWnd, &topleft);
        ::ClientToScreen(g_hWnd, &bottomright);

        RECT newpos = { topleft.x, topleft.y, bottomright.x, bottomright.y };

        if (validate_window_pos(newpos, false)) {
            s_lastknown_window_rect     = newpos;
        }
    }
}

void GenDesktopSettings(xString& dest)
{
    // This version ensures the client position stays consistent even if the decaling sizes change between sessions,
    // (eg, user changes window title bar sizes or similar) -- and generally makes more sense from a human-readable
    // perspective.

    UpdateLastKnownWindowPosition();
    dest.Clear();
    dest.AppendFmt("UserSettings.WindowClientPos  = { %d,%d }\n",
        s_lastknown_window_rect.left, s_lastknown_window_rect.top
    );

    dest.AppendFmt("UserSettings.WindowClientSize = { %d,%d }\n",
        g_client_size_pix.x, g_client_size_pix.y
    );
}

static xString s_settings_content_KnownVersion;
static xString s_settings_content_NewVersion;

bool SaveDesktopSettings(bool isMarkedDirty)
{
    GenDesktopSettings(s_settings_content_NewVersion);

    if (s_settings_content_NewVersion == s_settings_content_KnownVersion) {
        return false;
    }

    if (!isMarkedDirty) {
        // periodic save detected unexpected changes, which means there's something
        // changing in the settings env that's not issuing dirty pings...
        // TODO : print a diff of the specific lines that changed, so we can identify what needs
        //        to be fixed to call the Dirty function.
        log_host( "Unexpected saved settings occured." );
    }

    if (FILE* f = fopen("saved-settings.lua", "wb")) {
        fputs("-- Machine-generated user saved-settings file.\n",f);
        fputs("-- Any modifications here will be lost!\n\n",f);
        fputs("if UserSettings == nil then UserSettings = {} end\n\n",f);
        fputs(s_settings_content_NewVersion, f);
        s_settings_content_KnownVersion = s_settings_content_NewVersion;
        fclose(f);
    }
    return true;
}

// Optional thread to ping settings periodically, to cover handling settings that aren't correctly
// pinging s_sem_SettingsDirtied.
void* PingSaveSettingsThread(void*)
{
    while (1) {
        s_sem_SettingsDirtied.PostIfNeeded();
        xThreadSleep(10000);
    }
}

void* SaveSettingsThread(void*)
{
    while (1) {
        s_sem_SettingsDirtied.Wait();
        auto isMarkedDirty = (AtomicExchange(s_settings_dirty,0) >= 1);
        if (SaveDesktopSettings(isMarkedDirty)) {
            xThreadSleep(3000);
        }
        else {
            xThreadSleep(250);
        }
    }
}


HRESULT InitWindow(HINSTANCE hInstance, int nCmdShow)
{
    // note: current unused: MonitorFromRect() combined with GetMonitorInfo() is a much more useful
    // set of functions.

    //s_msw_monitors.clear();
    //EnumDisplayMonitors(nullptr, nullptr, MonitorEnumerator, 0);

    // Register class
    WNDCLASSEX wcex;
    wcex.cbSize         = sizeof(WNDCLASSEX);
    wcex.style          = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc    = WndProc;
    wcex.cbClsExtra     = 0;
    wcex.cbWndExtra     = 0;
    wcex.hInstance      = hInstance;
    wcex.hIcon          = LoadIcon(NULL, IDI_APPLICATION); //LoadIcon(hInstance, (LPCTSTR)IDI_TUTORIAL1);
    wcex.hIconSm        = LoadIcon(NULL, IDI_APPLICATION); //LoadIcon(wcex.hInstance, (LPCTSTR)IDI_TUTORIAL1);
    wcex.hCursor        = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground  = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszMenuName   = nullptr;
    wcex.lpszClassName  = L"RpgCraftGame";
    if (!RegisterClassEx(&wcex))
        return E_FAIL;

    // Create window
    g_hInst = hInstance;
    RECT rc = { 0, 0, 1280, 720 };
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    g_hWnd = CreateWindow(L"RpgCraftGame", L"RPG Craft: Bloody Spectacular Edition",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hInstance,
        nullptr);
    if (!g_hWnd)
        return E_FAIL;

    ShowWindow(g_hWnd, nCmdShow);

    return S_OK;
}

static inline bool _sopt_isWhitespace( char ch )
{
    return !ch
        || (ch == '\r')
        || (ch == '\n')
        || (ch == ' ')
        || (ch == '\t');
}

xString Host_GetCWD()
{
    char   buff[1024];
    char*  ret = _getcwd(buff, 1024);
    return ret ? xString(buff) : xString();
}

bool Msw_DrainMsgQueue()
{
    MSG msg = { 0 };

    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);

        if (msg.message == WM_QUIT) {
            return false;
        }
    }

    return true;
}

int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow)
{
    s_sem_SettingsDirtied.Create("SettingsDirtied");

    HostClockTick::Init();
    MSW_InitChrono();
    LogHostInit();

    fmod_CheckLib();
    fmod_InitSystem();

    // Tokenizer.
    //   * only care about double dash "--" anything lacking a double-dash prefix is an error.
    //   * all switches require assignment operator '=', eg:
    //        --option=value
    //   * Boolean toggles are required to specify --arg=0 or --arg=1
    //   * whitespace in options is disallowed
    //   * Whitespace is handled by the shell command line processor, so any whitespace is assumed part of the value.
    //   * environment variable expansion not supported (expected to be performed by shell)
    //
    // Windows-sepcific notes:
    //   * __argv has double-quotes already processed, but unfortunately Windows has some rather dodgy quotes parsing,
    //     so it's likely there'll be spurious quotes lying around when injecting Visual Studio EnvVars.  For now it's
    //     responsibility of user to fix those.
    //   * __argv does not process single quotes. These will be present and will result in malformed CLI syntax.
    //
    // The goal is to use lua for the bulk of cmdline parsing.  The only command line options to be
    // parsed here are things that we want to be applied *before* the lua engine has been started.
    //   * Configuration of log file things
    //   * config-local.lua override
    //   * visual studio script debug mode

    // an assist to microsoft for being kind enough to just provide these as globals already.

    auto g_argc = __argc;

    for (int a=1; a<g_argc; ++a) {
        //OutputDebugString( g_argv[a] );
        //OutputDebugString( TEXT("\n") );

#ifdef UNICODE
        xString utf8(__wargv[a]);
#else
        xString utf8(__argv[a]);
#endif
        char optmp[128] = { 0 };

        x_abort_on(!utf8.StartsWith("--"), "Invalid CLI option specified: %s", utf8.c_str());
        int readpos  = 2;
        int writepos = 0;
        for(;;) {
            x_abort_on(!utf8.data()[readpos] || readpos >= utf8.GetLength(),
                "Unexpected end of CLI option while searching for '='\n   Option Text: %s", utf8.c_str()
            );
            x_abort_on(_sopt_isWhitespace(utf8.data()[readpos]),
                "Invalid whitespace detected in CLI lvalue: %s", utf8.c_str()
            );
            x_abort_on(writepos >= bulkof(optmp)-1, "CLI option text is too long!");

            if (utf8.data()[readpos] == '=') break;
            optmp[writepos] = utf8.data()[readpos];
            ++writepos;
            ++readpos;
        }
        x_abort_on(!writepos, "Invalid zero-length option: %s", utf8.c_str());
        optmp[writepos+1] = 0;
        xString val     (utf8.data() + readpos + 1);        // forward past '='
        xString option  (optmp);
        if (0) {
            // start of list
        }
        elif(option == "script-dbg-relpath") {
            // Visual Studio script debug mode:
            // Script error messages should be printed relative to the solution directory.
            AjekScript_SetDebugRelativePath(val);
        }
        elif (0) {
            // end of list.
        }
    }

    AjekScript_InitAlloc();
    g_pkg_config_filename = "config-package-msw.lua";
    LoadPkgConfigFromMain(g_scriptEnv);

    // -----------------------------------------------------------
    // Init message recievers asap
    //   .. so that things that might want to queue msgs during init can do so...
    Scene_InitMessages();
    // -----------------------------------------------------------

    if (FAILED(InitWindow(hInstance, false)))
        return 0;

    ApplyDesktopSettings();
    dx11_InitDevice();
    ShowWindow(g_hWnd, true);
    UpdateLastKnownWindowPosition();

    KPad_SetMapping(g_kpad_btn_map_default);
    KPad_SetMapping(g_kpad_axs_map_default);

    // TODO: pulled from imgui - repeal and replace with existing msw-chrono stuff.
    if (!QueryPerformanceFrequency((LARGE_INTEGER *)&g_TicksPerSecond))
        return false;
    if (!QueryPerformanceCounter((LARGE_INTEGER *)&g_Time))
        return false;

    // Drain the message queue first before starting game threads.
    // No especially good reason for this -- there's just a bunch of poo in the windows
    // message queue after window creation and stuff, and I like to drain it all in as
    // synchronous an environment as possible.  --jstine

    thread_t s_thr_save_settings;
    thread_t s_thr_save_settings_ping;
    thread_create(s_thr_save_settings, SaveSettingsThread, "SaveSettings");
    thread_create(s_thr_save_settings_ping, PingSaveSettingsThread, "PingSaveSettings");

    if (Msw_DrainMsgQueue()) {

        KPad_CreateThread();
        Scene_CreateThreads();

        // Main message loop
        // Just handle messages here.  Don't want the msg loop doing anything that might take more
        // than a couple milliseconds.  Long jobs should always be dispatched to other threads.

        while (Msw_DrainMsgQueue()) {
            WaitMessage();
        }
    }
    Scene_ShutdownThreads();
    dx11_CleanupDevice();

    return 0;
}

// --------------------------------------------------------------------------------------
bool xIsDebuggerAttached()
{
    return ::IsDebuggerPresent();
}

void xOutputStringError(const char* str)
{
#if MSW_ENABLE_DEBUG_OUTPUT
    if (::IsDebuggerPresent())
        OutputDebugStringA(str);
    else
#endif
    {
        fputs(str, stderr);
    }
}

void xOutputString(const char* str)
{
#if MSW_ENABLE_DEBUG_OUTPUT
    if (::IsDebuggerPresent())
        OutputDebugStringA(str);
    else
#endif
    {
        fputs(str, stdout);
    }
}

static __ai assert_t DoAssertionDialog( const xString& title, const xString& content )
{
//  AtomicInc( s_open_assertion_counter );

    auto result = MessageBox(nullptr,
        toUTF16(content).wc_str(), toUTF16(title).wc_str(), MB_ABORTRETRYIGNORE
    );

//  AtomicDec( s_open_assertion_counter );

    if (result == IDRETRY)
    {
        xPrintLn( "*** DEBUG BREAK REQUESTED ***" );
        return assert_break;
    }
    else if (result == IDABORT)
    {
        xPrintLn( "*** TERMINATION REQUESTED ***" );
        return assert_terminate_app;
    }
    else if (result == IDIGNORE)
    {
        xPrintLn( "*** all assertions of this type ignored! ***" );
        return assert_ignore_all;
    }
    return assert_none;
}

__eai assert_t Host_AssertionDialog( const xString& title, const xString& message, const xString& context )
{
//  if (Host_IsUsingCLI())      return assert_break;
    if (IsDebuggerPresent())    return assert_break;

    return DoAssertionDialog( title, message + "\n\nAssertionContext:\n" + context );
}

void _hostImpl_ImGui_NewFrame()
{
    ImGuiIO& io = ImGui::GetIO();

    // Setup display size (every frame to accommodate for window resizing)
    RECT rect;
    GetClientRect(g_hWnd, &rect);
    io.ImeWindowHandle  = g_hWnd;
    io.DisplaySize      = ImVec2((float)(rect.right - rect.left), (float)(rect.bottom - rect.top));

    // Setup time step
    INT64 current_time;
    QueryPerformanceCounter((LARGE_INTEGER *)&current_time);
    io.DeltaTime = (float)(current_time - g_Time) / g_TicksPerSecond;
    g_Time = current_time;

    // io.MouseWheel : filled by WM_MOUSEWHEEL events

    // Read keyboard modifiers inputs
    io.KeyCtrl  = (::GetAsyncKeyState(VK_CONTROL)   & 0x8000) != 0;
    io.KeyShift = (::GetAsyncKeyState(VK_SHIFT  )   & 0x8000) != 0;
    io.KeyAlt   = (::GetAsyncKeyState(VK_MENU   )   & 0x8000) != 0;
    io.KeySuper = (::GetAsyncKeyState(VK_LWIN   )   & 0x8000) != 0;

    // Read Mouse position.
    // Use polling instead of WM_MOUSEMOVE since the windows msg pump isn't on this thread.

    auto mouseState = HostMouseImm_GetState();
    io.MousePos = ImVec2(-FLT_MAX,-FLT_MAX);

    if (mouseState.isValid) {
        io.MousePos = float2(mouseState.clientPos);

        io.MouseDown[0] = mouseState.pressed.LBUTTON;
        io.MouseDown[1] = mouseState.pressed.RBUTTON;
        io.MouseDown[2] = mouseState.pressed.MBUTTON;

        // Set OS mouse position if requested last frame by io.WantMoveMouse flag (used when io.NavMovesTrue is enabled by user and using directional navigation)
        if (io.WantMoveMouse)
        {
            POINT pos = { (int)io.MousePos.x, (int)io.MousePos.y };
            ::ClientToScreen(g_hWnd, &pos);
            ::SetCursorPos(pos.x, pos.y);
        }

        // Hide OS mouse cursor if ImGui is drawing it
        if (io.MouseDrawCursor) {
            ::SetCursor(NULL);
        }
    }

    // Start the frame
    ImGui::NewFrame();
}
