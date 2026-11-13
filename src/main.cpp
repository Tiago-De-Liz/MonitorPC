// Janela do overlay: sempre no topo, translúcida, arrastável, com três tamanhos (expandido / linha / mini).
#include "app.h"
#include "resource.h"
#include <shellapi.h>
#include <windowsx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

enum {
    IDM_MODE_FULL = 100, IDM_MODE_LINE, IDM_MODE_MINI,
    IDM_SETTINGS = 110, IDM_AUTOSTART, IDM_ELEVATE, IDM_EXIT, IDM_TASK_DONE,
    IDM_OPACITY = 200,   // + índice em kOpacity
    IDM_SNOOZE = 300,    // + índice em kSnooze
};
static const int kOpacity[] = { 100, 90, 80, 70, 60, 50, 40 };
static const struct { int days; const wchar_t* label; } kSnooze[] = {
    { 1, L"1 dia" }, { 3, L"3 dias" }, { 7, L"1 semana" }, { 14, L"2 semanas" }, { 30, L"1 mês" },
};

static HINSTANCE s_inst;
static HWND     s_hwnd;
static UINT     s_msgQuit;
static Readings s_r;
static int      s_dpi = 96;
static SIZE     s_size;
static bool     s_elevated, s_demo;
static bool     s_hover, s_tracking, s_down, s_dragging;
static POINT    s_downPt;
static RECT     s_downRc;
static int      s_downKind, s_downParam;
static int      s_hotKind, s_hotParam;
static int      s_today, s_menuTask = -1;
static UINT     s_ticks;

// ------------------------------------------------------------------------------------- posição

// A posição é guardada como um canto "ancorado": se o widget está no canto inferior direito, é esse canto
// que fica parado quando ele muda de tamanho (expandir cresce para cima e para a esquerda).
static POINT PosFor(SIZE sz) {
    POINT p = { (g_cfg.anchor & 1) ? g_cfg.x - sz.cx : g_cfg.x, (g_cfg.anchor & 2) ? g_cfg.y - sz.cy : g_cfg.y };
    return p;
}

static void AnchorFromRect(const RECT& rc) {
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST), &mi)) return;
    const bool right  = (rc.left + rc.right) / 2 > (mi.rcWork.left + mi.rcWork.right) / 2;
    const bool bottom = (rc.top + rc.bottom) / 2 > (mi.rcWork.top + mi.rcWork.bottom) / 2;
    g_cfg.anchor = (right ? 1 : 0) | (bottom ? 2 : 0);
    g_cfg.x = right ? rc.right : rc.left;
    g_cfg.y = bottom ? rc.bottom : rc.top;
    g_cfg.hasPos = true;
}

// Garante que o widget inteiro está dentro de algum monitor (ex.: o segundo monitor foi desligado).
static void ClampToMonitor(SIZE sz) {
    POINT p = PosFor(sz);
    RECT rc = { p.x, p.y, p.x + sz.cx, p.y + sz.cy };
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST), &mi)) return;
    int dx = 0, dy = 0;
    if (rc.right > mi.rcMonitor.right) dx = mi.rcMonitor.right - rc.right;
    if (rc.left + dx < mi.rcMonitor.left) dx = mi.rcMonitor.left - rc.left;
    if (rc.bottom > mi.rcMonitor.bottom) dy = mi.rcMonitor.bottom - rc.bottom;
    if (rc.top + dy < mi.rcMonitor.top) dy = mi.rcMonitor.top - rc.top;
    g_cfg.x += dx;
    g_cfg.y += dy;
}

struct MonitorPick { RECT work; bool found; };

static BOOL CALLBACK PickSecondary(HMONITOR mon, HDC, LPRECT, LPARAM lp) {
    MONITORINFO mi = { sizeof(mi) };
    if (GetMonitorInfoW(mon, &mi) && !(mi.dwFlags & MONITORINFOF_PRIMARY)) {
        MonitorPick* pick = (MonitorPick*)lp;
        pick->work = mi.rcWork;
        pick->found = true;
        return FALSE;
    }
    return TRUE;
}

// Primeira execução: canto inferior direito do segundo monitor (ou do principal, se só houver um).
static void DefaultPosition() {
    MonitorPick pick = {};
    EnumDisplayMonitors(NULL, NULL, PickSecondary, (LPARAM)&pick);
    if (!pick.found) SystemParametersInfoW(SPI_GETWORKAREA, 0, &pick.work, 0);
    g_cfg.x = pick.work.right - 8;
    g_cfg.y = pick.work.bottom - 8;
    g_cfg.anchor = 3;
    g_cfg.hasPos = true;
}

// ------------------------------------------------------------------------------------- desenho

void App_Refresh() {
    if (!s_hwnd) return;
    View v = { &g_cfg, &s_r, MulDiv(s_dpi, g_cfg.scale, 100), s_hotKind, s_hotParam };
    SIZE sz;
    Render_Measure(v, &sz);
    if ((sz.cx != s_size.cx || sz.cy != s_size.cy) && !s_dragging) ClampToMonitor(sz);
    HDC dc = Render_Draw(v, sz);
    if (!dc) return;
    POINT pos = PosFor(sz), src = { 0, 0 };
    const bool opaque = (s_hover && g_cfg.hoverOpaque) || s_dragging;
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, (BYTE)(opaque ? 255 : g_cfg.opacity * 255 / 100), AC_SRC_ALPHA };
    UpdateLayeredWindow(s_hwnd, NULL, &pos, &sz, dc, &src, 0, &bf, ULW_ALPHA);
    s_size = sz;
}

// "Sempre no topo" de verdade: se outra janela topmost passou por cima do widget, volta para a frente.
// Menus e dicas (do próprio usuário) são respeitados, e overlays "fantasma" que deixam o clique passar são ignorados.
static void KeepOnTop() {
    RECT me;
    if (!GetWindowRect(s_hwnd, &me)) return;
    int guard = 0;
    for (HWND h = GetWindow(s_hwnd, GW_HWNDPREV); h && guard < 256; h = GetWindow(h, GW_HWNDPREV), guard++) {
        RECT r, inter;
        if (!IsWindowVisible(h) || !GetWindowRect(h, &r) || !IntersectRect(&inter, &r, &me)) continue;
        if (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TRANSPARENT) continue;
        wchar_t cls[40] = L"";
        GetClassNameW(h, cls, _countof(cls));
        if (!lstrcmpW(cls, L"#32768") || !lstrcmpW(cls, L"tooltips_class32")) return;
        SetWindowPos(s_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return;
    }
}

static void Tick() {
    Readings r;
    ZeroMemory(&r, sizeof(r));
    Sensors_Read(r);
    if (g_cfg.showFps) r.fps = Fps_Sample(r.game, _countof(r.game));
    if (s_demo) {   // só para conferir o layout sem jogo aberto / sem admin
        if (r.cpuTemp < 0) r.cpuTemp = 62;
        r.fps = 144;
        lstrcpynW(r.game, L"jogo", _countof(r.game));
    }
    const int today = Today();
    const bool changed = memcmp(&r, &s_r, sizeof(r)) != 0 || today != s_today;
    s_r = r;
    s_today = today;
    if (changed) App_Refresh();
    KeepOnTop();
    // Depois de estabilizar, devolve ao sistema as páginas usadas só na inicialização.
    if (++s_ticks == 8) SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

// --------------------------------------------------------------------------------------- ações

static void SetMode(int mode) {
    if (g_cfg.mode == mode) return;
    g_cfg.mode = mode;
    s_hotKind = HIT_NONE;
    s_hotParam = 0;
    App_Refresh();
    Config_Save(g_cfg);
}

static void TaskDone(int i) {
    if (i < 0 || i >= g_cfg.taskCount) return;
    g_cfg.tasks[i].lastDone = Today();
    g_cfg.tasks[i].snoozeUntil = 0;
    Config_Save(g_cfg);
    Log(L"Lembrete \"%s\": marcado como feito", g_cfg.tasks[i].name);
    App_Refresh();
}

static void TaskSnooze(int i, int days) {
    if (i < 0 || i >= g_cfg.taskCount) return;
    g_cfg.tasks[i].snoozeUntil = Today() + days;
    Config_Save(g_cfg);
    Log(L"Lembrete \"%s\": adiado por %d dia(s)", g_cfg.tasks[i].name, days);
    App_Refresh();
}

static bool RunSelfElevated(const wchar_t* args) {
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"runas";
    sei.lpFile = ExePath();
    sei.lpParameters = args;
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei) != FALSE;
}

// O registro da tarefa agendada usa COM; roda num processo filho de vida curta para o processo
// principal (que fica 24/7) nunca carregar essas DLLs.
static void RequestAutostart(bool on) {
    const wchar_t* arg = on ? L"--autostart-on" : L"--autostart-off";
    if (s_elevated) {
        wchar_t cmd[MAX_PATH + 48];
        _snwprintf_s(cmd, _countof(cmd), _TRUNCATE, L"\"%s\" %s", ExePath(), arg);
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi;
        if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
    else RunSelfElevated(arg);
    if (on) lstrcpynW(g_cfg.autostartPath, ExePath(), _countof(g_cfg.autostartPath));
}

// Mantém a tarefa "iniciar com o Windows" apontando para este exe (primeira execução, ou se o exe mudou de pasta).
static void SyncAutostart() {
    if (!s_elevated || !g_cfg.autostart) return;
    if (lstrcmpiW(g_cfg.autostartPath, ExePath()) == 0 && Autostart_TaskExists()) return;
    RequestAutostart(true);
    Config_Save(g_cfg);
    Log(L"Iniciar com o Windows: tarefa agendada registrada para %s", ExePath());
}

static void ApplySettings(const Config& before) {
    if (g_cfg.intervalMs != before.intervalMs) SetTimer(s_hwnd, 1, (UINT)g_cfg.intervalMs, NULL);
    if (g_cfg.showFps != before.showFps) {
        if (g_cfg.showFps) Fps_Start(g_cfg.fpsKernel);
        else Fps_Stop();
    }
    if (g_cfg.autostart != before.autostart) RequestAutostart(g_cfg.autostart);
    Fps_ConfigChanged();
    Config_Save(g_cfg);
}

static void OpenSettings() {
    if (Settings_IsOpen()) return;
    Config* before = (Config*)HeapAlloc(GetProcessHeap(), 0, sizeof(Config));
    if (!before) return;
    *before = g_cfg;
    if (Settings_Show(s_inst)) ApplySettings(*before);
    HeapFree(GetProcessHeap(), 0, before);
    App_Refresh();
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

static void OnCommand(int cmd) {
    switch (cmd) {
    case IDM_MODE_FULL: SetMode(MODE_FULL); return;
    case IDM_MODE_LINE: SetMode(MODE_LINE); return;
    case IDM_MODE_MINI: SetMode(MODE_MINI); return;
    case IDM_SETTINGS:  OpenSettings(); return;
    case IDM_AUTOSTART:
        g_cfg.autostart = !g_cfg.autostart;
        RequestAutostart(g_cfg.autostart);
        Config_Save(g_cfg);
        return;
    case IDM_ELEVATE:   RunSelfElevated(L"--replace"); return;   // a nova instância pede para esta sair
    case IDM_EXIT:      DestroyWindow(s_hwnd); return;
    case IDM_TASK_DONE: TaskDone(s_menuTask); return;
    }
    if (cmd >= IDM_OPACITY && cmd < IDM_OPACITY + (int)_countof(kOpacity)) {
        g_cfg.opacity = kOpacity[cmd - IDM_OPACITY];
        Config_Save(g_cfg);
        App_Refresh();
    }
    else if (cmd >= IDM_SNOOZE && cmd < IDM_SNOOZE + (int)_countof(kSnooze)) {
        TaskSnooze(s_menuTask, kSnooze[cmd - IDM_SNOOZE].days);
    }
}

static void RunMenu(HMENU menu, POINT screenPt) {
    SetForegroundWindow(s_hwnd);   // sem isso o menu não fecha ao clicar fora
    const int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, screenPt.x, screenPt.y, 0,
                                        s_hwnd, NULL);
    PostMessageW(s_hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (cmd) OnCommand(cmd);

    // O mouse pode ter saído do widget enquanto o menu estava aberto.
    POINT cur;
    RECT rc;
    if (s_hwnd && GetCursorPos(&cur) && GetWindowRect(s_hwnd, &rc) && !PtInRect(&rc, cur) && s_hover) {
        s_hover = false;
        s_hotKind = HIT_NONE;
        App_Refresh();
    }
}

static void AddSnoozeItems(HMENU menu) {
    for (int i = 0; i < (int)_countof(kSnooze); i++) AppendMenuW(menu, MF_STRING, IDM_SNOOZE + i, kSnooze[i].label);
}

static void ShowContextMenu(POINT screenPt, int task) {
    HMENU menu = CreatePopupMenu();
    s_menuTask = task;
    if (task >= 0 && task < g_cfg.taskCount) {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, g_cfg.tasks[task].name);
        AppendMenuW(menu, MF_STRING, IDM_TASK_DONE, L"Marcar como feito hoje");
        HMENU snooze = CreatePopupMenu();
        AddSnoozeItems(snooze);
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)snooze, L"Adiar");
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    }
    AppendMenuW(menu, MF_STRING, IDM_MODE_FULL, L"Expandido");
    AppendMenuW(menu, MF_STRING, IDM_MODE_LINE, L"Linha");
    AppendMenuW(menu, MF_STRING, IDM_MODE_MINI, L"Mini");
    CheckMenuRadioItem(menu, IDM_MODE_FULL, IDM_MODE_MINI, IDM_MODE_FULL + g_cfg.mode, MF_BYCOMMAND);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);

    HMENU opacity = CreatePopupMenu();
    for (int i = 0; i < (int)_countof(kOpacity); i++) {
        wchar_t s[16];
        _snwprintf_s(s, _countof(s), _TRUNCATE, L"%d%%", kOpacity[i]);
        AppendMenuW(opacity, MF_STRING | (g_cfg.opacity == kOpacity[i] ? MF_CHECKED : 0), IDM_OPACITY + i, s);
    }
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)opacity, L"Opacidade");
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"Configurações…");
    AppendMenuW(menu, MF_STRING | (g_cfg.autostart ? MF_CHECKED : 0), IDM_AUTOSTART, L"Iniciar com o Windows");
    if (!s_elevated) AppendMenuW(menu, MF_STRING, IDM_ELEVATE, L"Reiniciar como administrador (FPS e temp. da CPU)");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Sair");
    RunMenu(menu, screenPt);
}

static bool HitAt(POINT pt, int* kind, int* param) {
    for (int i = g_hitCount - 1; i >= 0; i--) {
        if (PtInRect(&g_hits[i].rc, pt)) {
            *kind = g_hits[i].kind;
            *param = g_hits[i].param;
            return true;
        }
    }
    *kind = HIT_NONE;
    *param = 0;
    return false;
}

static void OnClick(int kind, int param) {
    switch (kind) {
    case HIT_COLLAPSE: SetMode(MODE_LINE); break;
    case HIT_EXPAND:   SetMode(MODE_FULL); break;
    case HIT_TOMINI:   SetMode(MODE_MINI); break;
    case HIT_FROMMINI: SetMode(MODE_FULL); break;
    case HIT_DONE:     TaskDone(param); break;
    case HIT_SNOOZE: {
        // Menu de prazos logo abaixo do botão "Adiar".
        POINT pt = { 0, 0 };
        for (int i = 0; i < g_hitCount; i++)
            if (g_hits[i].kind == HIT_SNOOZE && g_hits[i].param == param) { pt.x = g_hits[i].rc.left; pt.y = g_hits[i].rc.bottom; }
        ClientToScreen(s_hwnd, &pt);
        HMENU menu = CreatePopupMenu();
        AddSnoozeItems(menu);
        s_menuTask = param;
        RunMenu(menu, pt);
        break;
    }
    }
}

// --------------------------------------------------------------------------------------- janela

static void Shutdown() {
    static bool done;
    if (done) return;
    done = true;
    KillTimer(s_hwnd, 1);
    Fps_Stop();   // importante: uma sessão ETW esquecida continuaria viva depois do processo
    Sensors_Shutdown();
    Config_Save(g_cfg);
    Render_Shutdown();
    Log(L"Encerrado");
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (s_msgQuit && msg == s_msgQuit) {
        DestroyWindow(hwnd);
        return 0;
    }
    switch (msg) {
    case WM_TIMER:
        if (wp == 1) Tick();
        return 0;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;   // clicar no widget não tira o foco do jogo

    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        HitAt(pt, &s_downKind, &s_downParam);
        GetCursorPos(&s_downPt);
        GetWindowRect(hwnd, &s_downRc);
        s_down = true;
        s_dragging = false;
        SetCapture(hwnd);
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (s_down) {
            POINT cur;
            GetCursorPos(&cur);
            const int dx = cur.x - s_downPt.x, dy = cur.y - s_downPt.y;
            if (!s_dragging && (abs(dx) > GetSystemMetrics(SM_CXDRAG) || abs(dy) > GetSystemMetrics(SM_CYDRAG))) {
                s_dragging = true;
                s_hotKind = HIT_NONE;
            }
            if (s_dragging) {
                RECT rc = s_downRc;
                OffsetRect(&rc, dx, dy);
                AnchorFromRect(rc);
                App_Refresh();
            }
            return 0;
        }
        if (!s_tracking) {
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, hwnd, 0 };
            s_tracking = TrackMouseEvent(&t) != FALSE;
        }
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int kind, param;
        HitAt(pt, &kind, &param);
        if (kind == HIT_TASK) { kind = HIT_NONE; param = 0; }   // linha de lembrete não tem realce
        const bool changed = !s_hover || kind != s_hotKind || param != s_hotParam;
        s_hover = true;
        s_hotKind = kind;
        s_hotParam = param;
        if (changed) App_Refresh();
        return 0;
    }

    case WM_LBUTTONUP: {
        if (!s_down) return 0;
        const bool wasDrag = s_dragging;
        s_down = false;
        s_dragging = false;
        ReleaseCapture();
        if (wasDrag) {
            ClampToMonitor(s_size);
            Config_Save(g_cfg);
            App_Refresh();
            return 0;
        }
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int kind, param;
        HitAt(pt, &kind, &param);
        if (kind == s_downKind && param == s_downParam) OnClick(kind, param);
        return 0;
    }

    case WM_CAPTURECHANGED:
        if (s_down) {   // captura perdida no meio de um arraste
            const bool wasDrag = s_dragging;
            s_down = false;
            s_dragging = false;
            if (wasDrag) { Config_Save(g_cfg); App_Refresh(); }
        }
        return 0;

    case WM_MOUSELEAVE:
        s_tracking = false;
        if (s_hover || s_hotKind != HIT_NONE) {
            s_hover = false;
            s_hotKind = HIT_NONE;
            App_Refresh();
        }
        return 0;

    case WM_RBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int kind, param;
        HitAt(pt, &kind, &param);
        const bool onTask = kind == HIT_TASK || kind == HIT_DONE || kind == HIT_SNOOZE;
        ClientToScreen(hwnd, &pt);
        ShowContextMenu(pt, onTask ? param : -1);
        return 0;
    }

    case WM_DPICHANGED:
        s_dpi = HIWORD(wp);
        App_Refresh();
        return 0;

    case WM_DISPLAYCHANGE:
        ClampToMonitor(s_size);
        App_Refresh();
        return 0;

    case WM_SETTINGCHANGE:
        if (wp == SPI_SETWORKAREA) { ClampToMonitor(s_size); App_Refresh(); }
        return 0;

    case WM_ENDSESSION:
        if (wp) Shutdown();
        return 0;

    case WM_DESTROY:
        Shutdown();
        s_hwnd = NULL;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(_In_ HINSTANCE inst, _In_opt_ HINSTANCE, _In_ PWSTR cmd, _In_ int) {
    s_inst = inst;
    if (wcsstr(cmd, L"--autostart-on")) return Autostart_Apply(true);
    if (wcsstr(cmd, L"--autostart-off")) return Autostart_Apply(false);

    s_msgQuit = RegisterWindowMessageW(QUIT_MESSAGE);
    s_demo = wcsstr(cmd, L"--demo") != NULL;
    const bool replace = wcsstr(cmd, L"--replace") != NULL;
    const bool noElevate = wcsstr(cmd, L"--no-elevate") != NULL;

    // Instância única. "--quit" fecha a que estiver aberta; "--replace" toma o lugar dela.
    HWND other = FindWindowW(WND_CLASS, NULL);
    if (wcsstr(cmd, L"--quit")) {
        if (other) PostMessageW(other, s_msgQuit, 0, 0);
        return 0;
    }
    if (other) {
        if (!replace) return 0;
        PostMessageW(other, s_msgQuit, 0, 0);
        for (int i = 0; i < 100 && FindWindowW(WND_CLASS, NULL); i++) Sleep(50);
        if (FindWindowW(WND_CLASS, NULL)) return 1;
    }

    Log_Rotate();
    const bool hadConfig = Config_Load(g_cfg);
    s_elevated = IsElevated();
    if (!s_elevated && g_cfg.elevate && !noElevate) {
        // FPS e temperatura da CPU exigem administrador: reabre elevado (um UAC). Se recusar, segue limitado.
        if (RunSelfElevated(L"--replace")) return 0;
        Log(L"Elevação recusada: rodando sem FPS e sem temperatura da CPU");
    }

    Log(L"---- %s %s iniciando (%s) ----", APP_NAME, APP_VERSION, s_elevated ? L"administrador" : L"usuário comum");
    if (!g_cfg.hasPos) DefaultPosition();

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = WND_CLASS;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    if (!RegisterClassExW(&wc)) return 1;

    // WS_EX_TOOLWINDOW: fora da barra de tarefas e do Alt+Tab. WS_EX_NOACTIVATE: nunca rouba o foco.
    s_hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, WND_CLASS, APP_NAME,
                             WS_POPUP, g_cfg.x - ((g_cfg.anchor & 1) ? 16 : 0), g_cfg.y - ((g_cfg.anchor & 2) ? 16 : 0),
                             16, 16, NULL, NULL, inst, NULL);
    if (!s_hwnd) return 1;
    ChangeWindowMessageFilterEx(s_hwnd, s_msgQuit, MSGFLT_ALLOW, NULL);   // deixa um processo não elevado pedir para fechar
    s_dpi = (int)GetDpiForWindow(s_hwnd);
    if (s_dpi <= 0) s_dpi = 96;

    Sensors_Init();
    if (g_cfg.showFps) Fps_Start(g_cfg.fpsKernel);
    SyncAutostart();
    if (!hadConfig) Config_Save(g_cfg);

    s_today = Today();
    s_r.cpuTemp = s_r.gpuTemp = s_r.gpuPct = -1;
    App_Refresh();
    ShowWindow(s_hwnd, SW_SHOWNOACTIVATE);
    SetTimer(s_hwnd, 1, (UINT)g_cfg.intervalMs, NULL);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
