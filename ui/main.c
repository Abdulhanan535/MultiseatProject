// ================================================================
//  MultiseatProject -- ui/main.c
//  Control panel:
//    Tab 1: Account — set Seat 2 user, Start/Stop session
//    Tab 2: Games   — register games for IFEO mutex hook
// ================================================================
#pragma comment(lib,"comctl32.lib")
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include "../service/session_manager.h"
#include "../service/dll_injector.h"

// ── Control IDs ──────────────────────────────────────────────────
#define IDC_TAB              10
#define IDC_BTN_START        20
#define IDC_BTN_STOP         21
#define IDC_EDIT_USER        30
#define IDC_EDIT_PASS        31
#define IDC_EDIT_GAME_EXE    40
#define IDC_LIST_GAMES       41
#define IDC_BTN_ADD_GAME     42
#define IDC_BTN_REMOVE_GAME  43
#define IDC_STATUS           60
#define IDC_SUNSHINE_STATUS  70
#define WM_SESSION_RESULT   (WM_APP+2)

// ── Globals ──────────────────────────────────────────────────────
static HWND   g_hWnd;
static HWND   g_Panels[2];        // 0=Account, 1=Games
static HFONT  g_hFont = NULL;
static WNDPROC g_OrigPanelProc = NULL;

// Subclass: STATIC panels swallow WM_COMMAND, forward to main window
static LRESULT CALLBACK PanelSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COMMAND)
        return SendMessageW(g_hWnd, msg, wp, lp);
    return CallWindowProcW(g_OrigPanelProc, hwnd, msg, wp, lp);
}

// ── Forward declarations ─────────────────────────────────────────
LRESULT CALLBACK WndProc(HWND,UINT,WPARAM,LPARAM);
static void CreateAllPanels(HWND);
static void ShowPanel(int idx);
static void OnStart(void);
static void OnStop(void);
static void OnAddGame(void);
static void OnRemoveGame(void);
static void SetStatus(LPCWSTR msg);
static HWND MakeListView(HWND parent, int id, int x, int y, int w, int h);
static void LV_AddCol(HWND hLV, int col, LPCWSTR title, int width);
static void LV_SetText(HWND hLV, int row, int col, LPCWSTR text);
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
    INITCOMMONCONTROLSEX ic = { sizeof(ic), ICC_TAB_CLASSES|ICC_LISTVIEW_CLASSES|ICC_BAR_CLASSES };
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
        CW_USEDEFAULT, CW_USEDEFAULT, 600, 480,
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
        CreateAllPanels(hwnd);
        // Initialize session subsystem (patches termsrv.dll)
        SessionManager_Init();
        // Locate the hook DLL (same dir as exe)
        WCHAR dllPath[MAX_PATH];
        GetModuleFileNameW(NULL, dllPath, MAX_PATH);
        WCHAR* lastSlash = wcsrchr(dllPath, L'\\');
        if (lastSlash) wcscpy_s(lastSlash+1, MAX_PATH-(lastSlash-dllPath+1), L"multiseat_mutex_hook.dll");
        DllInjector_Init(dllPath);
        SetStatus(L"Ready. Configure account and click Start Seat 2.");
        return 0;
    }

    case WM_NOTIFY: {
        LPNMHDR nm = (LPNMHDR)lp;
        if (nm->idFrom == IDC_TAB && nm->code == TCN_SELCHANGE) {
            ShowPanel((int)SendMessageW(nm->hwndFrom, TCM_GETCURSEL, 0, 0));
        }
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BTN_START:       OnStart();         break;
        case IDC_BTN_STOP:        OnStop();          break;
        case IDC_BTN_ADD_GAME:    OnAddGame();       break;
        case IDC_BTN_REMOVE_GAME: OnRemoveGame();    break;
        }
        return 0;

    case WM_SESSION_RESULT:
        EnableWindow(GetDlgItem(g_Panels[0], IDC_BTN_START), TRUE);
        SetStatus(wp ? L"Seat 2 session started! Connect via Moonlight."
                     : L"Failed to start session. Is the service running?");
        return 0;

    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ================================================================
//  CreateAllPanels
// ================================================================
static void CreateAllPanels(HWND hwnd)
{
    HINSTANCE hI = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);
    int W = 572, H = 390;

    // Create UI font
    g_hFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    // ---- Tab control ----
    HWND hTab = CreateWindowW(WC_TABCONTROLW, NULL,
        WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS,
        8, 8, W, H, hwnd, (HMENU)IDC_TAB, hI, NULL);
    SendMessageW(hTab, WM_SETFONT, (WPARAM)g_hFont, TRUE);

    TCITEMW ti = { TCIF_TEXT };
    ti.pszText = (LPWSTR)L"  Account  ";  TabCtrl_InsertItem(hTab, 0, &ti);
    ti.pszText = (LPWSTR)L"  Games  ";    TabCtrl_InsertItem(hTab, 1, &ti);

    int px=16, py=38, pw=542, ph=340;

    // ==== Panel 0: Account ====
    g_Panels[0] = CreateWindowW(L"STATIC", NULL,
        WS_CHILD|WS_VISIBLE, px, py, pw, ph, hwnd, NULL, hI, NULL);
    {
        HWND p = g_Panels[0];
        int y = 10;

        // Sunshine status
        WCHAR sunPath[MAX_PATH];
        BOOL hasSunshine = DetectSunshine(sunPath, MAX_PATH);

        CreateWindowW(L"BUTTON", L" Sunshine ",
            WS_CHILD|WS_VISIBLE|BS_GROUPBOX,
            0, y, pw, 50, p, NULL, hI, NULL);
        CreateWindowW(L"STATIC",
            hasSunshine ? L"Sunshine detected. Your friend can connect via Moonlight."
                        : L"Sunshine NOT found. Install it from github.com/LizardByte/Sunshine",
            WS_CHILD|WS_VISIBLE|SS_CENTERIMAGE,
            14, y+22, pw-28, 20, p, (HMENU)IDC_SUNSHINE_STATUS, hI, NULL);

        // User credentials group
        y = 74;
        CreateWindowW(L"BUTTON", L" Seat 2 User Account ",
            WS_CHILD|WS_VISIBLE|BS_GROUPBOX,
            0, y, pw, 140, p, NULL, hI, NULL);

        y += 28;
        CreateWindowW(L"STATIC",
            L"Create a separate Windows account for the second player.\n"
            L"They will get their own desktop, saves, and game sessions.",
            WS_CHILD|WS_VISIBLE, 16, y, pw-32, 36, p, NULL, hI, NULL);

        y += 46;
        CreateWindowW(L"STATIC", L"Username:",
            WS_CHILD|WS_VISIBLE|SS_CENTERIMAGE, 16, y, 75, 24, p, NULL, hI, NULL);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"Seat2User",
            WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
            95, y, 220, 24, p, (HMENU)IDC_EDIT_USER, hI, NULL);

        y += 36;
        CreateWindowW(L"STATIC", L"Password:",
            WS_CHILD|WS_VISIBLE|SS_CENTERIMAGE, 16, y, 75, 24, p, NULL, hI, NULL);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", NULL,
            WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL|ES_PASSWORD,
            95, y, 220, 24, p, (HMENU)IDC_EDIT_PASS, hI, NULL);

        // Start/Stop buttons
        y = 228;
        CreateWindowW(L"BUTTON", L"Start Seat 2",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_DEFPUSHBUTTON,
            0, y, 140, 34, p, (HMENU)IDC_BTN_START, hI, NULL);
        CreateWindowW(L"BUTTON", L"Stop Seat 2",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            150, y, 140, 34, p, (HMENU)IDC_BTN_STOP, hI, NULL);

        // Instructions
        y += 48;
        CreateWindowW(L"STATIC",
            L"How to use:\n"
            L"1. Click 'Start Seat 2' to create the session\n"
            L"2. Your friend opens Moonlight on their PC\n"
            L"3. They connect to this PC's IP address\n"
            L"4. They get a full Windows desktop as Seat2User",
            WS_CHILD|WS_VISIBLE, 0, y, pw, 80, p, NULL, hI, NULL);
    }

    // ==== Panel 1: Games ====
    g_Panels[1] = CreateWindowW(L"STATIC", NULL,
        WS_CHILD, px, py, pw, ph, hwnd, NULL, hI, NULL);
    {
        HWND p = g_Panels[1];
        int y = 10;

        CreateWindowW(L"BUTTON", L" Register Games ",
            WS_CHILD|WS_VISIBLE|BS_GROUPBOX,
            0, y, pw, 80, p, NULL, hI, NULL);

        y += 22;
        CreateWindowW(L"STATIC",
            L"Register game EXEs so both players can run their own instance.",
            WS_CHILD|WS_VISIBLE, 14, y, pw-28, 20, p, NULL, hI, NULL);

        y += 28;
        CreateWindowW(L"STATIC", L"EXE name:",
            WS_CHILD|WS_VISIBLE|SS_CENTERIMAGE, 14, y, 70, 24, p, NULL, hI, NULL);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
            88, y, 260, 24, p, (HMENU)IDC_EDIT_GAME_EXE, hI, NULL);
        CreateWindowW(L"BUTTON", L"Add",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            356, y, 70, 26, p, (HMENU)IDC_BTN_ADD_GAME, hI, NULL);
        CreateWindowW(L"BUTTON", L"Remove",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            434, y, 80, 26, p, (HMENU)IDC_BTN_REMOVE_GAME, hI, NULL);

        y = 100;
        HWND hLV = MakeListView(p, IDC_LIST_GAMES, 0, y, pw, 230);
        LV_AddCol(hLV, 0, L"Game Executable",  300);
        LV_AddCol(hLV, 1, L"Status",           200);
    }

    // ---- Subclass panels ----
    for (int i = 0; i < 2; i++) {
        WNDPROC orig = (WNDPROC)SetWindowLongPtrW(g_Panels[i], GWLP_WNDPROC, (LONG_PTR)PanelSubclassProc);
        if (!g_OrigPanelProc) g_OrigPanelProc = orig;
    }

    // ---- Status bar ----
    HWND hStatus = CreateWindowW(STATUSCLASSNAMEW, L"",
        WS_CHILD|WS_VISIBLE|SBARS_SIZEGRIP,
        0, 0, 0, 0, hwnd, (HMENU)IDC_STATUS, hI, NULL);
    SendMessageW(hStatus, WM_SETFONT, (WPARAM)g_hFont, TRUE);

    // Apply font
    EnumChildWindows(hwnd, ApplyFontToChild, 0);
    ShowPanel(0);
}

static void ShowPanel(int idx) {
    for (int i = 0; i < 2; i++)
        ShowWindow(g_Panels[i], (i == idx) ? SW_SHOW : SW_HIDE);
}

// ── ListView helpers ─────────────────────────────────────────────
static HWND MakeListView(HWND p, int id, int x, int y, int w, int h) {
    HWND hLV = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
        WS_CHILD|WS_VISIBLE|LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS,
        x,y,w,h, p, (HMENU)(UINT_PTR)id,
        (HINSTANCE)GetWindowLongPtrW(p, GWLP_HINSTANCE), NULL);
    ListView_SetExtendedListViewStyle(hLV, LVS_EX_FULLROWSELECT|LVS_EX_GRIDLINES|LVS_EX_DOUBLEBUFFER);
    return hLV;
}

static void LV_AddCol(HWND hLV, int col, LPCWSTR title, int width) {
    LVCOLUMNW c = { LVCF_TEXT|LVCF_WIDTH, 0, width, (LPWSTR)title };
    ListView_InsertColumn(hLV, col, &c);
}

static void LV_SetText(HWND hLV, int row, int col, LPCWSTR text) {
    LVITEMW it = { LVIF_TEXT }; it.iItem=row; it.iSubItem=col;
    it.pszText=(LPWSTR)text;
    ListView_SetItem(hLV, &it);
}

// ── Sunshine detection ───────────────────────────────────────────
static BOOL DetectSunshine(WCHAR* pathOut, DWORD pathLen) {
    WCHAR testPaths[][MAX_PATH] = {
        L"C:\\Program Files\\Sunshine\\sunshine.exe",
        L"C:\\Program Files (x86)\\Sunshine\\sunshine.exe",
    };
    for (int i = 0; i < 2; i++) {
        if (GetFileAttributesW(testPaths[i]) != INVALID_FILE_ATTRIBUTES) {
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
    GetDlgItemTextW(g_Panels[0], IDC_EDIT_USER, user, 64);
    GetDlgItemTextW(g_Panels[0], IDC_EDIT_PASS, pass, 64);

    BOOL ok = SessionManager_CreateSeat(1, user, pass, L"");
    if (ok) {
        ULONG sid = SessionManager_GetSessionId(1);
        DllInjector_InjectSession(sid);
        DllInjector_WatchSession(sid);
    }
    PostMessageW(hwnd, WM_SESSION_RESULT, ok, 0);
    return 0;
}

static void OnStart(void) {
    WCHAR user[64];
    GetDlgItemTextW(g_Panels[0], IDC_EDIT_USER, user, 64);
    if (!user[0]) { SetStatus(L"Enter a username."); return; }
    SetStatus(L"Starting Seat 2 session...");
    EnableWindow(GetDlgItem(g_Panels[0], IDC_BTN_START), FALSE);
    CreateThread(NULL, 0, StartThread, g_hWnd, 0, NULL);
}

static void OnStop(void) {
    SessionManager_TerminateSeat(1);
    EnableWindow(GetDlgItem(g_Panels[0], IDC_BTN_START), TRUE);
    SetStatus(L"Seat 2 stopped.");
}

static void OnAddGame(void) {
    WCHAR exe[128];
    GetDlgItemTextW(g_Panels[1], IDC_EDIT_GAME_EXE, exe, 128);
    if (!exe[0]) return;

    // Build DLL path (same dir as this exe)
    WCHAR dllPath[MAX_PATH];
    GetModuleFileNameW(NULL, dllPath, MAX_PATH);
    WCHAR* last = wcsrchr(dllPath, L'\\');
    if (last) wcscpy_s(last+1, MAX_PATH-(last-dllPath+1), L"multiseat_mutex_hook.dll");

    // Register in IFEO
    WCHAR keyPath[512];
    swprintf_s(keyPath, _countof(keyPath),
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\"
        L"Image File Execution Options\\%ws", exe);
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, keyPath, 0, NULL,
            REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, L"VerifierDlls", 0, REG_SZ,
            (BYTE*)dllPath, (DWORD)((wcslen(dllPath)+1)*sizeof(WCHAR)));
        DWORD globalFlag = 0x100;
        RegSetValueExW(hKey, L"GlobalFlag", 0, REG_DWORD, (BYTE*)&globalFlag, sizeof(globalFlag));
        RegCloseKey(hKey);

        HWND hLV = GetDlgItem(g_Panels[1], IDC_LIST_GAMES);
        LVITEMW item = {LVIF_TEXT}; item.iItem = ListView_GetItemCount(hLV);
        item.pszText = exe; ListView_InsertItem(hLV, &item);
        LV_SetText(hLV, item.iItem, 1, L"Hook registered");
        SetStatus(L"Game added. Both players can now run their own instance.");
    } else {
        SetStatus(L"Failed to register game. Run as Administrator.");
    }
}

static void OnRemoveGame(void) {
    HWND hLV = GetDlgItem(g_Panels[1], IDC_LIST_GAMES);
    int sel = ListView_GetNextItem(hLV, -1, LVNI_SELECTED);
    if (sel < 0) return;
    WCHAR exe[128]; ListView_GetItemText(hLV, sel, 0, exe, 128);

    WCHAR keyPath[512];
    swprintf_s(keyPath, _countof(keyPath),
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\"
        L"Image File Execution Options\\%ws", exe);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, keyPath);

    ListView_DeleteItem(hLV, sel);
    SetStatus(L"Game removed from hook list.");
}

static void SetStatus(LPCWSTR msg) {
    SetWindowTextW(GetDlgItem(g_hWnd, IDC_STATUS), msg);
}
