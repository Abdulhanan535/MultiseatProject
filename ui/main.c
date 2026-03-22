// ================================================================
//  MultiseatProject -- ui/main.c
//  Control panel:
//    Tab 1: Devices  — assign keyboard/mouse to seats
//    Tab 2: Monitors — assign monitors to seats
//    Tab 3: Accounts — set Seat 2 user credentials
//    Tab 4: Games    — register games for IFEO hook
// ================================================================
#pragma comment(lib,"comctl32.lib")
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#pragma comment(lib,"wtsapi32.lib")

#include <windows.h>
#include <commctrl.h>
#include <wtsapi32.h>
#include <stdio.h>
#include "../service/device_manager.h"
#include "../service/display_manager.h"
#include "../service/session_manager.h"
#include "../service/dll_injector.h"

// ── IDs ──────────────────────────────────────────────────────────
#define IDC_TAB              10
#define IDC_LIST_DEV         20
#define IDC_LIST_MON         21
#define IDC_COMBO_SEAT_DEV   30
#define IDC_COMBO_SEAT_MON   31
#define IDC_BTN_ASSIGN_DEV   40
#define IDC_BTN_ASSIGN_MON   41
#define IDC_BTN_REFRESH      42
#define IDC_BTN_START        43
#define IDC_BTN_STOP         44
#define IDC_EDIT_USER        50
#define IDC_EDIT_PASS        51
#define IDC_EDIT_GAME_EXE    52
#define IDC_LIST_GAMES       53
#define IDC_BTN_ADD_GAME     54
#define IDC_BTN_REMOVE_GAME  55
#define IDC_STATUS           60
#define WM_SET_STATUS       (WM_APP+1)
#define WM_SESSION_RESULT   (WM_APP+2)

static HWND g_hWnd;
static HWND g_Panels[4];   // one panel per tab
static int  g_CurrentTab = 0;

// ── Forward declarations ─────────────────────────────────────────
LRESULT CALLBACK WndProc(HWND,UINT,WPARAM,LPARAM);
static void CreateAllPanels(HWND);
static void ShowPanel(int idx);
static void RefreshDevices(void);
static void RefreshMonitors(void);
static void OnAssignDevice(void);
static void OnAssignMonitor(void);
static void OnStart(void);
static void OnStop(void);
static void OnAddGame(void);
static void OnRemoveGame(void);
static void SetStatus(LPCWSTR msg);
static void FillSeatCombo(HWND hCombo, int defaultSeat);
static HWND MakeListView(HWND parent, int id, int x, int y, int w, int h);
static void LV_AddCol(HWND hLV, int col, LPCWSTR title, int width);
static void LV_SetText(HWND hLV, int row, int col, LPCWSTR text);

// ── Colors ───────────────────────────────────────────────────────
#define COLOR_BG    RGB(30,30,30)
#define COLOR_PANEL RGB(40,40,40)
#define COLOR_TEXT  RGB(220,220,220)
#define COLOR_ACCENT RGB(0,120,215)

// ================================================================
//  WinMain
// ================================================================
int WINAPI wWinMain(HINSTANCE hI, HINSTANCE hP, LPWSTR cmd, int show)
{
    UNREFERENCED_PARAMETER(hP); UNREFERENCED_PARAMETER(cmd);
    INITCOMMONCONTROLSEX ic = { sizeof(ic), ICC_TAB_CLASSES|ICC_LISTVIEW_CLASSES };
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
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 720, 520,
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
        CreateDirectoryW(L"C:\\ProgramData\\MultiseatProject", NULL);
        SessionManager_Init();
        DeviceManager_Enumerate();
        DisplayManager_Enumerate();
        DeviceManager_LoadConfig(L"C:\\ProgramData\\MultiseatProject\\config.json");
        {
            WCHAR dllPath[MAX_PATH];
            GetModuleFileNameW(NULL,dllPath,MAX_PATH);
            WCHAR* last=wcsrchr(dllPath,L'\\');
            if(last) wcscpy_s(last+1,MAX_PATH-(last-dllPath+1),L"multiseat_mutex_hook.dll");
            DllInjector_Init(dllPath);
        }
        RefreshDevices();
        RefreshMonitors();
        SetStatus(L"Ready. Assign devices and monitors, then click Start Seat 2.");
        return 0;
    }

    case WM_NOTIFY: {
        LPNMHDR nm = (LPNMHDR)lp;
        if (nm->idFrom == IDC_TAB && nm->code == TCN_SELCHANGE) {
            g_CurrentTab = TabCtrl_GetCurSel(nm->hwndFrom);
            ShowPanel(g_CurrentTab);
        }
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BTN_REFRESH:     DeviceManager_Enumerate(); RefreshDevices();
                                  DisplayManager_Enumerate(); RefreshMonitors(); break;
        case IDC_BTN_ASSIGN_DEV:  OnAssignDevice();  break;
        case IDC_BTN_ASSIGN_MON:  OnAssignMonitor(); break;
        case IDC_BTN_START:       OnStart();         break;
        case IDC_BTN_STOP:        OnStop();          break;
        case IDC_BTN_ADD_GAME:    OnAddGame();       break;
        case IDC_BTN_REMOVE_GAME: OnRemoveGame();    break;
        }
        return 0;

    case WM_SESSION_RESULT:
        EnableWindow(GetDlgItem(hwnd, IDC_BTN_START), TRUE);
        SetStatus(wp ? L"Seat 2 session started!" : L"Failed to start session.");
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
    int W = 700, H = 430;

    // ── Tab control ──────────────────────────────────────────
    HWND hTab = CreateWindowW(WC_TABCONTROLW, NULL,
        WS_CHILD|WS_VISIBLE|TCS_FLATBUTTONS,
        5, 5, W, H, hwnd, (HMENU)IDC_TAB, hI, NULL);

    TCITEMW ti = { TCIF_TEXT };
    ti.pszText = (LPWSTR)L"  Devices  ";  TabCtrl_InsertItem(hTab, 0, &ti);
    ti.pszText = (LPWSTR)L"  Monitors  "; TabCtrl_InsertItem(hTab, 1, &ti);
    ti.pszText = (LPWSTR)L"  Account  ";  TabCtrl_InsertItem(hTab, 2, &ti);
    ti.pszText = (LPWSTR)L"  Games  ";    TabCtrl_InsertItem(hTab, 3, &ti);

    // Panel rect (inside tab)
    int px=10, py=34, pw=682, ph=380;

    // ── Panel 0: Devices ─────────────────────────────────────
    g_Panels[0] = CreateWindowW(L"STATIC", NULL,
        WS_CHILD|WS_VISIBLE, px, py, pw, ph, hwnd, NULL, hI, NULL);
    {
        HWND p = g_Panels[0];
        HWND hLV = MakeListView(p, IDC_LIST_DEV, 0, 0, pw, 270);
        LV_AddCol(hLV, 0, L"Device Description", 340);
        LV_AddCol(hLV, 1, L"Type",   80);
        LV_AddCol(hLV, 2, L"Seat",   80);
        LV_AddCol(hLV, 3, L"ID",     160);

        CreateWindowW(L"STATIC",L"Assign selected →",WS_CHILD|WS_VISIBLE,
            0,278,120,22,p,NULL,hI,NULL);
        HWND hC = CreateWindowW(L"COMBOBOX",NULL,
            WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST,
            124,276,90,100,p,(HMENU)IDC_COMBO_SEAT_DEV,hI,NULL);
        FillSeatCombo(hC, 0);

        CreateWindowW(L"BUTTON",L"Assign",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            220,275,70,24,p,(HMENU)IDC_BTN_ASSIGN_DEV,hI,NULL);
        CreateWindowW(L"BUTTON",L"Refresh",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            300,275,70,24,p,(HMENU)IDC_BTN_REFRESH,hI,NULL);
    }

    // ── Panel 1: Monitors ────────────────────────────────────
    g_Panels[1] = CreateWindowW(L"STATIC",NULL,
        WS_CHILD, px, py, pw, ph, hwnd, NULL, hI, NULL);
    {
        HWND p = g_Panels[1];
        HWND hLV = MakeListView(p, IDC_LIST_MON, 0, 0, pw, 270);
        LV_AddCol(hLV, 0, L"Device Name",  200);
        LV_AddCol(hLV, 1, L"Resolution",   120);
        LV_AddCol(hLV, 2, L"Primary",       70);
        LV_AddCol(hLV, 3, L"Seat",          70);

        CreateWindowW(L"STATIC",L"Assign selected →",WS_CHILD|WS_VISIBLE,
            0,278,120,22,p,NULL,hI,NULL);
        HWND hC = CreateWindowW(L"COMBOBOX",NULL,
            WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST,
            124,276,90,100,p,(HMENU)IDC_COMBO_SEAT_MON,hI,NULL);
        FillSeatCombo(hC, 1);
        CreateWindowW(L"BUTTON",L"Assign",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            220,275,70,24,p,(HMENU)IDC_BTN_ASSIGN_MON,hI,NULL);
    }

    // ── Panel 2: Account ─────────────────────────────────────
    g_Panels[2] = CreateWindowW(L"STATIC",NULL,
        WS_CHILD, px, py, pw, ph, hwnd, NULL, hI, NULL);
    {
        HWND p = g_Panels[2];
        int y = 20;
        CreateWindowW(L"STATIC",
            L"A separate Windows user account is created for Seat 2.\n"
            L"It gets its own desktop, saves, and game session.",
            WS_CHILD|WS_VISIBLE, 10,y, 500, 40, p, NULL, hI, NULL);
        y += 60;
        CreateWindowW(L"STATIC",L"Username:",WS_CHILD|WS_VISIBLE,10,y,80,22,p,NULL,hI,NULL);
        CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"Seat2User",
            WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
            95,y,200,22,p,(HMENU)IDC_EDIT_USER,hI,NULL);
        y += 34;
        CreateWindowW(L"STATIC",L"Password:",WS_CHILD|WS_VISIBLE,10,y,80,22,p,NULL,hI,NULL);
        CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",NULL,
            WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL|ES_PASSWORD,
            95,y,200,22,p,(HMENU)IDC_EDIT_PASS,hI,NULL);
        y += 50;
        CreateWindowW(L"BUTTON",L"Start Seat 2",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_DEFPUSHBUTTON,
            10,y,140,32,p,(HMENU)IDC_BTN_START,hI,NULL);
        CreateWindowW(L"BUTTON",L"Stop Seat 2",
            WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            160,y,130,32,p,(HMENU)IDC_BTN_STOP,hI,NULL);
    }

    // ── Panel 3: Games ───────────────────────────────────────
    g_Panels[3] = CreateWindowW(L"STATIC",NULL,
        WS_CHILD, px, py, pw, ph, hwnd, NULL, hI, NULL);
    {
        HWND p = g_Panels[3];
        int y = 10;
        CreateWindowW(L"STATIC",
            L"Register game EXEs so Seat 2 can run its own instance.\n"
            L"This adds them to the mutex-hook IFEO list.",
            WS_CHILD|WS_VISIBLE, 10,y,560,36, p,NULL,hI,NULL);
        y += 44;
        CreateWindowW(L"STATIC",L"Game EXE name:",WS_CHILD|WS_VISIBLE,10,y,110,22,p,NULL,hI,NULL);
        CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"GameName.exe",
            WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,
            125,y,260,22,p,(HMENU)IDC_EDIT_GAME_EXE,hI,NULL);
        CreateWindowW(L"BUTTON",L"Add",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            392,y,60,22,p,(HMENU)IDC_BTN_ADD_GAME,hI,NULL);
        CreateWindowW(L"BUTTON",L"Remove",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
            458,y,70,22,p,(HMENU)IDC_BTN_REMOVE_GAME,hI,NULL);
        y += 34;
        HWND hLV = MakeListView(p, IDC_LIST_GAMES, 0, y, pw, 220);
        LV_AddCol(hLV, 0, L"Game Executable", 300);
        LV_AddCol(hLV, 1, L"Status",          150);
    }

    // ── Status bar ───────────────────────────────────────────
    CreateWindowExW(WS_EX_CLIENTEDGE,L"STATIC",L"",
        WS_CHILD|WS_VISIBLE|SS_LEFT,
        5, H+10, W, 22, hwnd, (HMENU)IDC_STATUS, hI, NULL);

    ShowPanel(0);
}

static void ShowPanel(int idx) {
    for (int i = 0; i < 4; i++)
        ShowWindow(g_Panels[i], (i == idx) ? SW_SHOW : SW_HIDE);
}

// ── ListView helpers ─────────────────────────────────────────────
static HWND MakeListView(HWND p, int id, int x, int y, int w, int h) {
    HWND hLV = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
        WS_CHILD|WS_VISIBLE|LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS,
        x,y,w,h, p, (HMENU)(UINT_PTR)id,
        (HINSTANCE)GetWindowLongPtrW(p, GWLP_HINSTANCE), NULL);
    ListView_SetExtendedListViewStyle(hLV, LVS_EX_FULLROWSELECT|LVS_EX_GRIDLINES);
    return hLV;
}
static void LV_AddCol(HWND hLV, int col, LPCWSTR title, int width) {
    LVCOLUMNW c = { LVCF_TEXT|LVCF_WIDTH };
    c.pszText = (LPWSTR)title; c.cx = width;
    ListView_InsertColumn(hLV, col, &c);
}
static void LV_SetText(HWND hLV, int row, int col, LPCWSTR text) {
    ListView_SetItemText(hLV, row, col, (LPWSTR)text);
}
static void FillSeatCombo(HWND hC, int def) {
    SendMessageW(hC, CB_ADDSTRING, 0, (LPARAM)L"Seat 1");
    SendMessageW(hC, CB_ADDSTRING, 0, (LPARAM)L"Seat 2");
    SendMessageW(hC, CB_SETCURSEL, def, 0);
}

// ── Refresh ──────────────────────────────────────────────────────
static void RefreshDevices(void) {
    HWND hLV = GetDlgItem(g_Panels[0], IDC_LIST_DEV);
    ListView_DeleteAllItems(hLV);
    ULONG n; MULTISEAT_DEVICE_INFO* devs = DeviceManager_GetDevices(&n);
    for (ULONG i = 0; i < n; i++) {
        LVITEMW item = { LVIF_TEXT|LVIF_PARAM };
        item.iItem = (int)i; item.lParam = (LPARAM)i;
        item.pszText = devs[i].Description;
        ListView_InsertItem(hLV, &item);
        LV_SetText(hLV,(int)i,1, devs[i].IsKeyboard?L"Keyboard":L"Mouse");
        WCHAR s[16];
        if (devs[i].SeatIndex==0xFF) wcscpy_s(s,16,L"Unassigned");
        else swprintf_s(s,16,L"Seat %lu",devs[i].SeatIndex+1);
        LV_SetText(hLV,(int)i,2,s);
        LV_SetText(hLV,(int)i,3,devs[i].InstanceId);
    }
}

static void RefreshMonitors(void) {
    HWND hLV = GetDlgItem(g_Panels[1], IDC_LIST_MON);
    ListView_DeleteAllItems(hLV);
    ULONG n; MONITOR_INFO* mons = DisplayManager_GetMonitors(&n);
    for (ULONG i = 0; i < n; i++) {
        LVITEMW item = { LVIF_TEXT|LVIF_PARAM };
        item.iItem = (int)i; item.lParam = (LPARAM)i;
        item.pszText = mons[i].DeviceName;
        ListView_InsertItem(hLV, &item);
        WCHAR res[32];
        swprintf_s(res,32,L"%ldx%ld",
            mons[i].WorkRect.right-mons[i].WorkRect.left,
            mons[i].WorkRect.bottom-mons[i].WorkRect.top);
        LV_SetText(hLV,(int)i,1,res);
        LV_SetText(hLV,(int)i,2,mons[i].IsPrimary?L"Yes":L"No");
        WCHAR s[16];
        if (mons[i].SeatIndex==0xFF) wcscpy_s(s,16,L"Unassigned");
        else swprintf_s(s,16,L"Seat %lu",mons[i].SeatIndex+1);
        LV_SetText(hLV,(int)i,3,s);
    }
}

// ── Actions ──────────────────────────────────────────────────────
static void OnAssignDevice(void) {
    HWND hLV = GetDlgItem(g_Panels[0], IDC_LIST_DEV);
    int sel = ListView_GetNextItem(hLV,-1,LVNI_SELECTED);
    if (sel<0){SetStatus(L"Select a device first.");return;}
    int si = (int)SendMessageW(GetDlgItem(g_Panels[0],IDC_COMBO_SEAT_DEV),CB_GETCURSEL,0,0);
    if (DeviceManager_AssignToSeat((ULONG)sel,(ULONG)(si<0?0:si))) {
        DeviceManager_SaveConfig(L"C:\\ProgramData\\MultiseatProject\\config.json");
        RefreshDevices();
    }
    SetStatus(L"Device assigned.");
}

static void OnAssignMonitor(void) {
    HWND hLV = GetDlgItem(g_Panels[1], IDC_LIST_MON);
    int sel = ListView_GetNextItem(hLV,-1,LVNI_SELECTED);
    if (sel<0){SetStatus(L"Select a monitor first.");return;}
    int si = (int)SendMessageW(GetDlgItem(g_Panels[1],IDC_COMBO_SEAT_MON),CB_GETCURSEL,0,0);
    if (DisplayManager_AssignToSeat((ULONG)sel,(ULONG)(si<0?1:si))) RefreshMonitors();
    SetStatus(L"Monitor assigned.");
}

static DWORD WINAPI StartThread(LPVOID p) {
    HWND hwnd = (HWND)p;
    WCHAR user[64], pass[64], mon[64];
    GetDlgItemTextW(g_Panels[2],IDC_EDIT_USER,user,64);
    GetDlgItemTextW(g_Panels[2],IDC_EDIT_PASS,pass,64);
    // Get monitor assigned to seat 1
    ULONG n; MONITOR_INFO* mons = DisplayManager_GetMonitors(&n);
    wcscpy_s(mon,64,L"\\\\.\\DISPLAY2");
    for (ULONG i=0;i<n;i++) if(mons[i].SeatIndex==1){wcscpy_s(mon,64,mons[i].DeviceName);break;}

    // Save config
    DeviceManager_SaveConfig(L"C:\\ProgramData\\MultiseatProject\\config.json");

    BOOL ok = SessionManager_CreateSeat(1,user,pass,mon);
    if (ok) {
        // Inject mutex hook into the new session
        ULONG sid = SessionManager_GetSessionId(1);
        DllInjector_InjectSession(sid);
        DllInjector_WatchSession(sid);
    }
    PostMessageW(hwnd, WM_SESSION_RESULT, ok, 0);
    return 0;
}

static void OnStart(void) {
    WCHAR user[64];
    GetDlgItemTextW(g_Panels[2],IDC_EDIT_USER,user,64);
    if(!user[0]){SetStatus(L"Enter a username.");return;}
    SetStatus(L"Starting Seat 2 session...");
    EnableWindow(GetDlgItem(g_Panels[2],IDC_BTN_START),FALSE);
    CreateThread(NULL,0,StartThread,g_hWnd,0,NULL);
}

static void OnStop(void) {
    SessionManager_TerminateSeat(1);
    SetStatus(L"Seat 2 stopped.");
}

static void OnAddGame(void) {
    WCHAR exe[128];
    GetDlgItemTextW(g_Panels[3],IDC_EDIT_GAME_EXE,exe,128);
    if(!exe[0]) return;
    WCHAR dllPath[MAX_PATH];
    GetModuleFileNameW(NULL,dllPath,MAX_PATH);
    WCHAR* last=wcsrchr(dllPath,L'\\');
    if(last) wcscpy_s(last+1,MAX_PATH-(last-dllPath+1),L"multiseat_mutex_hook.dll");
    DllInjector_Init(dllPath);
    if(DllInjector_SetupIFEO(exe)) {
        HWND hLV = GetDlgItem(g_Panels[3],IDC_LIST_GAMES);
        LVITEMW item={LVIF_TEXT}; item.iItem=ListView_GetItemCount(hLV);
        item.pszText=exe; ListView_InsertItem(hLV,&item);
        LV_SetText(hLV,item.iItem,1,L"Hook registered");
        SetStatus(L"Game added. Seat 2 can now run its own instance.");
    }
}

static void OnRemoveGame(void) {
    HWND hLV = GetDlgItem(g_Panels[3],IDC_LIST_GAMES);
    int sel = ListView_GetNextItem(hLV,-1,LVNI_SELECTED);
    if(sel<0) return;
    WCHAR exe[128]; ListView_GetItemText(hLV,sel,0,exe,128);
    DllInjector_RemoveIFEO(exe);
    ListView_DeleteItem(hLV,sel);
    SetStatus(L"Game removed from hook list.");
}

static void SetStatus(LPCWSTR msg) {
    SetWindowTextW(GetDlgItem(g_hWnd,IDC_STATUS),msg);
}
