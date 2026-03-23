// ================================================================
//  MultiseatProject -- ui/main.c
//  Control panel — one-click second seat setup via Sunshine/Moonlight
// ================================================================
#pragma comment(lib,"comctl32.lib")
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include "../service/session_manager.h"

// ── Control IDs ──────────────────────────────────────────────────
#define IDC_BTN_START        20
#define IDC_BTN_STOP         21
#define IDC_EDIT_USER        30
#define IDC_EDIT_PASS        31
#define IDC_STATUS           60
#define IDC_SUNSHINE_STATUS  70
#define WM_SESSION_RESULT   (WM_APP+2)

// ── Globals ──────────────────────────────────────────────────────
static HWND   g_hWnd;
static HFONT  g_hFont = NULL;

// ── Forward declarations ─────────────────────────────────────────
LRESULT CALLBACK WndProc(HWND,UINT,WPARAM,LPARAM);
static void CreateControls(HWND);
static void OnStart(void);
static void OnStop(void);
static void SetStatus(LPCWSTR msg);
static BOOL DetectSunshine(WCHAR* pathOut, DWORD pathLen);

// ── Font helper ──────────────────────────────────────────────────
static BOOL CALLBACK ApplyFontToChild(HWND hwnd, LPARAM lp) {
    SendMessageW(hwnd, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    return TRUE;
}

// ================================================================
//  WinMain
// ================================================================
int WINAPI wWinMain(HINSTANCE hI, HINSTANCE hP, LPWSTR cmd, int show)
{
    UNREFERENCED_PARAMETER(hP); UNREFERENCED_PARAMETER(cmd);
    INITCOMMONCONTROLSEX ic = { sizeof(ic), ICC_BAR_CLASSES };
    InitCommonControlsEx(&ic);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.hInstance     = hI;
    wc.lpszClassName = L"MultiseatUI";
    wc.lpfnWndProc   = WndProc;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassExW(&wc);

    g_hWnd = CreateWindowExW(0, L"MultiseatUI",
        L"Multiseat Control Panel",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 520, 420,
        NULL, NULL, hI, NULL);
    ShowWindow(g_hWnd, show);
    UpdateWindow(g_hWnd);

    MSG msg;
    while (GetMessageW(&msg,NULL,0,0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}

// ================================================================
//  WndProc
// ================================================================
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        CreateControls(hwnd);
        SessionManager_Init();
        SetStatus(L"Ready. Enter credentials and click Start Seat 2.");
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BTN_START:  OnStart();  break;
        case IDC_BTN_STOP:   OnStop();   break;
        }
        return 0;

    case WM_SESSION_RESULT:
        EnableWindow(GetDlgItem(g_hWnd, IDC_BTN_START), TRUE);
        SetStatus(wp ? L"Seat 2 started! Friend connects via Moonlight to port 48100."
                     : L"Failed to start session. Check credentials and try again.");
        return 0;

    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ================================================================
//  CreateControls
// ================================================================
static void CreateControls(HWND hwnd)
{
    HINSTANCE hI = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);

    g_hFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    int x = 16, y = 12, w = 470;

    // ── Sunshine status ──────────────────────────────────────────
    WCHAR sunPath[MAX_PATH];
    BOOL hasSunshine = DetectSunshine(sunPath, MAX_PATH);

    CreateWindowW(L"BUTTON", L" Sunshine ",
        WS_CHILD|WS_VISIBLE|BS_GROUPBOX,
        x, y, w, 50, hwnd, NULL, hI, NULL);
    CreateWindowW(L"STATIC",
        hasSunshine ? L"\x2705 Sunshine detected. Your friend can connect via Moonlight."
                    : L"\x274C Sunshine NOT found. Install from github.com/LizardByte/Sunshine",
        WS_CHILD|WS_VISIBLE|SS_CENTERIMAGE,
        x+14, y+22, w-28, 20, hwnd, (HMENU)IDC_SUNSHINE_STATUS, hI, NULL);

    // ── User credentials ─────────────────────────────────────────
    y = 76;
    CreateWindowW(L"BUTTON", L" Seat 2 User Account ",
        WS_CHILD|WS_VISIBLE|BS_GROUPBOX,
        x, y, w, 130, hwnd, NULL, hI, NULL);

    y += 28;
    CreateWindowW(L"STATIC",
        L"Create a separate Windows account for the second player.",
        WS_CHILD|WS_VISIBLE, x+14, y, w-28, 18, hwnd, NULL, hI, NULL);

    y += 30;
    CreateWindowW(L"STATIC", L"Username:",
        WS_CHILD|WS_VISIBLE|SS_CENTERIMAGE, x+14, y, 70, 24, hwnd, NULL, hI, NULL);
    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"Seat2User",
        WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
        x+90, y, 200, 24, hwnd, (HMENU)IDC_EDIT_USER, hI, NULL);

    y += 34;
    CreateWindowW(L"STATIC", L"Password:",
        WS_CHILD|WS_VISIBLE|SS_CENTERIMAGE, x+14, y, 70, 24, hwnd, NULL, hI, NULL);
    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", NULL,
        WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL|ES_PASSWORD,
        x+90, y, 200, 24, hwnd, (HMENU)IDC_EDIT_PASS, hI, NULL);

    // ── Start / Stop buttons ─────────────────────────────────────
    y = 222;
    CreateWindowW(L"BUTTON", L"Start Seat 2",
        WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_DEFPUSHBUTTON,
        x, y, 140, 36, hwnd, (HMENU)IDC_BTN_START, hI, NULL);
    CreateWindowW(L"BUTTON", L"Stop Seat 2",
        WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
        x+150, y, 140, 36, hwnd, (HMENU)IDC_BTN_STOP, hI, NULL);

    // ── Instructions ─────────────────────────────────────────────
    y = 274;
    CreateWindowW(L"STATIC",
        L"How it works:\n"
        L"1. Click 'Start Seat 2' — creates account + session automatically\n"
        L"2. Your friend opens Moonlight on their PC\n"
        L"3. Connects to this PC's IP, port 48100\n"
        L"4. They get a full Windows desktop as Seat2User\n\n"
        L"All apps are automatically isolated between sessions.\n"
        L"Games with Global mutexes are handled automatically.",
        WS_CHILD|WS_VISIBLE, x, y, w, 120, hwnd, NULL, hI, NULL);

    // ── Status bar ───────────────────────────────────────────────
    HWND hStatus = CreateWindowW(STATUSCLASSNAMEW, L"",
        WS_CHILD|WS_VISIBLE|SBARS_SIZEGRIP,
        0, 0, 0, 0, hwnd, (HMENU)IDC_STATUS, hI, NULL);
    SendMessageW(hStatus, WM_SETFONT, (WPARAM)g_hFont, TRUE);

    EnumChildWindows(hwnd, ApplyFontToChild, 0);
}

// ── Sunshine detection ───────────────────────────────────────────
static BOOL DetectSunshine(WCHAR* pathOut, DWORD pathLen) {
    WCHAR localSun[MAX_PATH];
    GetModuleFileNameW(NULL, localSun, MAX_PATH);
    WCHAR* lastSlash = wcsrchr(localSun, L'\\');
    if (lastSlash) wcscpy_s(lastSlash + 1, MAX_PATH - (lastSlash - localSun + 1), L"Sunshine\\sunshine.exe");
    else localSun[0] = 0;

    LPCWSTR testPaths[] = {
        localSun,
        L"C:\\Program Files\\Sunshine\\sunshine.exe",
        L"C:\\Program Files (x86)\\Sunshine\\sunshine.exe",
    };
    for (int i = 0; i < 3; i++) {
        if (testPaths[i][0] && GetFileAttributesW(testPaths[i]) != INVALID_FILE_ATTRIBUTES) {
            wcscpy_s(pathOut, pathLen, testPaths[i]);
            return TRUE;
        }
    }
    pathOut[0] = 0;
    return FALSE;
}

// ── Session start thread ─────────────────────────────────────────
static DWORD WINAPI StartThread(LPVOID p) {
    HWND hwnd = (HWND)p;
    WCHAR user[64], pass[64];
    GetDlgItemTextW(g_hWnd, IDC_EDIT_USER, user, 64);
    GetDlgItemTextW(g_hWnd, IDC_EDIT_PASS, pass, 64);

    BOOL ok = SessionManager_CreateSeat(1, user, pass, L"");
    PostMessageW(hwnd, WM_SESSION_RESULT, ok, 0);
    return 0;
}

static void OnStart(void) {
    WCHAR user[64];
    GetDlgItemTextW(g_hWnd, IDC_EDIT_USER, user, 64);
    if (!user[0]) { SetStatus(L"Enter a username."); return; }
    WCHAR pass[64];
    GetDlgItemTextW(g_hWnd, IDC_EDIT_PASS, pass, 64);
    if (!pass[0]) { SetStatus(L"Enter a password."); return; }
    SetStatus(L"Starting Seat 2 session...");
    EnableWindow(GetDlgItem(g_hWnd, IDC_BTN_START), FALSE);
    CreateThread(NULL, 0, StartThread, g_hWnd, 0, NULL);
}

static void OnStop(void) {
    SessionManager_TerminateSeat(1);
    EnableWindow(GetDlgItem(g_hWnd, IDC_BTN_START), TRUE);
    SetStatus(L"Seat 2 stopped.");
}

static void SetStatus(LPCWSTR msg) {
    SetWindowTextW(GetDlgItem(g_hWnd, IDC_STATUS), msg);
}
