// Janela de Configurações (controles nativos do Windows). Só existe enquanto está aberta.
#include "app.h"
#include "resource.h"
#include <commctrl.h>
#include <stdio.h>
#include <wchar.h>

static HWND    s_dlg;
static Config* s_backup;
static const int kScales[]    = { 80, 90, 100, 110, 125, 150, 175, 200 };
static const int kIntervals[] = { 1000, 2000, 3000, 5000 };

bool Settings_IsOpen() {
    if (!s_dlg) return false;
    SetForegroundWindow(s_dlg);
    return true;
}

static int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static int Px(HWND dlg, int v) { return MulDiv(v, (int)GetDpiForWindow(dlg), 96); }

static void DayToSystemTime(int day, SYSTEMTIME* st) {
    int y, m, d;
    CivilFromDays(day, &y, &m, &d);
    ZeroMemory(st, sizeof(*st));
    st->wYear = (WORD)y;
    st->wMonth = (WORD)m;
    st->wDay = (WORD)d;
}

// ------------------------------------------------------------------------------ editar um lembrete

static INT_PTR CALLBACK TaskProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        Task* t = (Task*)lp;
        SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)t);
        SetDlgItemTextW(dlg, IDC_T_NAME, t->name);
        SendDlgItemMessageW(dlg, IDC_T_NAME, EM_LIMITTEXT, TASK_NAME_LEN - 1, 0);
        SetDlgItemInt(dlg, IDC_T_EVERY, (UINT)t->every, FALSE);
        SendDlgItemMessageW(dlg, IDC_T_EVERY, EM_LIMITTEXT, 3, 0);
        static const wchar_t* units[] = { L"dias", L"meses", L"anos" };
        for (const wchar_t* u : units) SendDlgItemMessageW(dlg, IDC_T_UNIT, CB_ADDSTRING, 0, (LPARAM)u);
        SendDlgItemMessageW(dlg, IDC_T_UNIT, CB_SETCURSEL, t->unit == L'd' ? 0 : (t->unit == L'a' ? 2 : 1), 0);

        SYSTEMTIME range[2] = {}, last;
        DayToSystemTime(Today(), &range[1]);   // não faz sentido "feito" no futuro
        DayToSystemTime(t->lastDone, &last);
        HWND dtp = GetDlgItem(dlg, IDC_T_LAST);
        DateTime_SetRange(dtp, GDTR_MAX, range);
        DateTime_SetSystemtime(dtp, GDT_VALID, &last);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            Task* t = (Task*)GetWindowLongPtrW(dlg, DWLP_USER);
            wchar_t name[TASK_NAME_LEN];
            GetDlgItemTextW(dlg, IDC_T_NAME, name, _countof(name));
            wchar_t* n = name;
            while (*n == L' ') n++;
            BOOL ok = FALSE;
            int every = (int)GetDlgItemInt(dlg, IDC_T_EVERY, &ok, FALSE);
            if (!*n) {
                MessageBoxW(dlg, L"Dê um nome ao lembrete.", APP_NAME, MB_OK | MB_ICONINFORMATION);
                SetFocus(GetDlgItem(dlg, IDC_T_NAME));
                return TRUE;
            }
            if (!ok || every < 1) {
                MessageBoxW(dlg, L"O intervalo precisa ser pelo menos 1.", APP_NAME, MB_OK | MB_ICONINFORMATION);
                SetFocus(GetDlgItem(dlg, IDC_T_EVERY));
                return TRUE;
            }
            lstrcpynW(t->name, n, TASK_NAME_LEN);
            t->every = every;
            int sel = (int)SendDlgItemMessageW(dlg, IDC_T_UNIT, CB_GETCURSEL, 0, 0);
            t->unit = sel == 0 ? L'd' : (sel == 2 ? L'a' : L'm');
            SYSTEMTIME st;
            if (DateTime_GetSystemtime(GetDlgItem(dlg, IDC_T_LAST), &st) == GDT_VALID) {
                int day = DaysFromCivil(st.wYear, st.wMonth, st.wDay);
                if (day != t->lastDone) { t->lastDone = day; t->snoozeUntil = 0; }   // nova data zera o adiamento
            }
            EndDialog(dlg, 1);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, 0); return TRUE; }
        break;
    }
    return FALSE;
}

static bool EditTask(HWND parent, Task* t) {
    return DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_TASK), parent, TaskProc, (LPARAM)t) == 1;
}

// --------------------------------------------------------------------------------- janela principal

static void FillList(HWND dlg, int select) {
    HWND lv = GetDlgItem(dlg, IDC_TASKS);
    ListView_DeleteAllItems(lv);
    const int today = Today();
    for (int i = 0; i < g_cfg.taskCount; i++) {
        const Task& t = g_cfg.tasks[i];
        wchar_t s[64];
        LVITEMW it = {};
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.pszText = (LPWSTR)t.name;
        ListView_InsertItem(lv, &it);
        Task_IntervalText(t, s, _countof(s));
        ListView_SetItemText(lv, i, 1, s);
        FormatDateBr(t.lastDone, s, _countof(s));
        ListView_SetItemText(lv, i, 2, s);
        Task_RemainingText(t, today, g_cfg.warnDays, s, _countof(s));
        ListView_SetItemText(lv, i, 3, s);
    }
    if (select >= g_cfg.taskCount) select = g_cfg.taskCount - 1;
    if (select >= 0) ListView_SetItemState(lv, select, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
}

static int Selected(HWND dlg) { return ListView_GetNextItem(GetDlgItem(dlg, IDC_TASKS), -1, LVNI_SELECTED); }

static void UpdateOpacityLabel(HWND dlg) {
    wchar_t s[16];
    _snwprintf_s(s, _countof(s), _TRUNCATE, L"%d%%", g_cfg.opacity);
    SetDlgItemTextW(dlg, IDC_OPACITY_LBL, s);
}

static void InitDialog(HWND dlg) {
    s_dlg = dlg;
    HICON ico = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_APP));
    SendMessageW(dlg, WM_SETICON, ICON_BIG, (LPARAM)ico);
    SendMessageW(dlg, WM_SETICON, ICON_SMALL, (LPARAM)ico);

    HWND lv = GetDlgItem(dlg, IDC_TASKS);
    ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    static const struct { const wchar_t* title; int width; } cols[] = {
        { L"Lembrete", 158 }, { L"A cada", 66 }, { L"Última vez", 78 }, { L"Faltam", 82 },
    };
    for (int i = 0; i < (int)_countof(cols); i++) {
        LVCOLUMNW c = {};
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = (LPWSTR)cols[i].title;
        c.cx = Px(dlg, cols[i].width);
        ListView_InsertColumn(lv, i, &c);
    }
    FillList(dlg, 0);
    SetDlgItemInt(dlg, IDC_WARN, (UINT)g_cfg.warnDays, FALSE);
    SendDlgItemMessageW(dlg, IDC_WARN, EM_LIMITTEXT, 3, 0);

    SendDlgItemMessageW(dlg, IDC_OPACITY, TBM_SETRANGE, TRUE, MAKELPARAM(30, 100));
    SendDlgItemMessageW(dlg, IDC_OPACITY, TBM_SETPAGESIZE, 0, 10);
    SendDlgItemMessageW(dlg, IDC_OPACITY, TBM_SETPOS, TRUE, g_cfg.opacity);
    UpdateOpacityLabel(dlg);

    wchar_t s[160];
    int sel = 2;
    for (int i = 0; i < (int)_countof(kScales); i++) {
        _snwprintf_s(s, _countof(s), _TRUNCATE, L"%d%%", kScales[i]);
        SendDlgItemMessageW(dlg, IDC_SCALE, CB_ADDSTRING, 0, (LPARAM)s);
        if (kScales[i] <= g_cfg.scale) sel = i;
    }
    SendDlgItemMessageW(dlg, IDC_SCALE, CB_SETCURSEL, sel, 0);
    CheckDlgButton(dlg, IDC_HOVER, g_cfg.hoverOpaque ? BST_CHECKED : BST_UNCHECKED);

    sel = 0;
    for (int i = 0; i < (int)_countof(kIntervals); i++) {
        _snwprintf_s(s, _countof(s), _TRUNCATE, L"%d s", kIntervals[i] / 1000);
        SendDlgItemMessageW(dlg, IDC_INTERVAL, CB_ADDSTRING, 0, (LPARAM)s);
        if (kIntervals[i] <= g_cfg.intervalMs) sel = i;
    }
    SendDlgItemMessageW(dlg, IDC_INTERVAL, CB_SETCURSEL, sel, 0);
    CheckDlgButton(dlg, IDC_SHOWRAM, g_cfg.showRam ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_SHOWFPS, g_cfg.showFps ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_FPSWINDOWED, g_cfg.fpsWindowed ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemInt(dlg, IDC_CPUWARN, (UINT)g_cfg.cpuWarn, FALSE);
    SetDlgItemInt(dlg, IDC_CPUCRIT, (UINT)g_cfg.cpuCrit, FALSE);
    SetDlgItemInt(dlg, IDC_GPUWARN, (UINT)g_cfg.gpuWarn, FALSE);
    SetDlgItemInt(dlg, IDC_GPUCRIT, (UINT)g_cfg.gpuCrit, FALSE);
    static const int tempIds[] = { IDC_CPUWARN, IDC_CPUCRIT, IDC_GPUWARN, IDC_GPUCRIT };
    for (int id : tempIds) SendDlgItemMessageW(dlg, id, EM_LIMITTEXT, 3, 0);
    CheckDlgButton(dlg, IDC_AUTOSTART, g_cfg.autostart ? BST_CHECKED : BST_UNCHECKED);

    _snwprintf_s(s, _countof(s), _TRUNCATE, L"FPS: %s\r\nTemperatura da CPU: %s", Fps_Status(), Sensors_CpuTempStatus());
    SetDlgItemTextW(dlg, IDC_STATUS, s);
}

static int ReadInt(HWND dlg, int id, int fallback, int lo, int hi) {
    BOOL ok = FALSE;
    int v = (int)GetDlgItemInt(dlg, id, &ok, FALSE);
    return ok ? Clamp(v, lo, hi) : fallback;
}

static void ReadFields(HWND dlg) {
    g_cfg.warnDays = ReadInt(dlg, IDC_WARN, g_cfg.warnDays, 0, 365);
    g_cfg.cpuWarn = ReadInt(dlg, IDC_CPUWARN, g_cfg.cpuWarn, 30, 120);
    g_cfg.cpuCrit = ReadInt(dlg, IDC_CPUCRIT, g_cfg.cpuCrit, 30, 120);
    g_cfg.gpuWarn = ReadInt(dlg, IDC_GPUWARN, g_cfg.gpuWarn, 30, 120);
    g_cfg.gpuCrit = ReadInt(dlg, IDC_GPUCRIT, g_cfg.gpuCrit, 30, 120);
    if (g_cfg.cpuCrit < g_cfg.cpuWarn) g_cfg.cpuCrit = g_cfg.cpuWarn;
    if (g_cfg.gpuCrit < g_cfg.gpuWarn) g_cfg.gpuCrit = g_cfg.gpuWarn;
    int sel = (int)SendDlgItemMessageW(dlg, IDC_INTERVAL, CB_GETCURSEL, 0, 0);
    if (sel >= 0 && sel < (int)_countof(kIntervals)) g_cfg.intervalMs = kIntervals[sel];
    g_cfg.showRam = IsDlgButtonChecked(dlg, IDC_SHOWRAM) == BST_CHECKED;
    g_cfg.showFps = IsDlgButtonChecked(dlg, IDC_SHOWFPS) == BST_CHECKED;
    g_cfg.fpsWindowed = IsDlgButtonChecked(dlg, IDC_FPSWINDOWED) == BST_CHECKED;
    g_cfg.hoverOpaque = IsDlgButtonChecked(dlg, IDC_HOVER) == BST_CHECKED;
    g_cfg.autostart = IsDlgButtonChecked(dlg, IDC_AUTOSTART) == BST_CHECKED;
}

static void EditSelected(HWND dlg) {
    int i = Selected(dlg);
    if (i < 0 || i >= g_cfg.taskCount) return;
    Task t = g_cfg.tasks[i];
    if (EditTask(dlg, &t)) {
        g_cfg.tasks[i] = t;
        FillList(dlg, i);
        App_Refresh();
    }
}

static INT_PTR CALLBACK SettingsProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG:
        InitDialog(dlg);
        return TRUE;

    case WM_HSCROLL:   // opacidade: aplica na hora, para ver o resultado no widget
        if ((HWND)lp == GetDlgItem(dlg, IDC_OPACITY)) {
            g_cfg.opacity = Clamp((int)SendDlgItemMessageW(dlg, IDC_OPACITY, TBM_GETPOS, 0, 0), 30, 100);
            UpdateOpacityLabel(dlg);
            App_Refresh();
        }
        return TRUE;

    case WM_NOTIFY: {
        const NMHDR* hdr = (const NMHDR*)lp;
        if (hdr->idFrom == IDC_TASKS && hdr->code == NM_DBLCLK) EditSelected(dlg);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_ADD: {
            if (g_cfg.taskCount >= MAX_TASKS) {
                MessageBoxW(dlg, L"Limite de lembretes atingido.", APP_NAME, MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            Task t = {};
            t.every = 3;
            t.unit = L'm';
            t.lastDone = Today();
            if (EditTask(dlg, &t)) {
                g_cfg.tasks[g_cfg.taskCount++] = t;
                FillList(dlg, g_cfg.taskCount - 1);
                App_Refresh();
            }
            return TRUE;
        }
        case IDC_EDIT:
            EditSelected(dlg);
            return TRUE;
        case IDC_DEL: {
            int i = Selected(dlg);
            if (i < 0 || i >= g_cfg.taskCount) return TRUE;
            wchar_t q[128];
            _snwprintf_s(q, _countof(q), _TRUNCATE, L"Remover o lembrete \"%s\"?", g_cfg.tasks[i].name);
            if (MessageBoxW(dlg, q, APP_NAME, MB_YESNO | MB_ICONQUESTION) != IDYES) return TRUE;
            for (int k = i; k < g_cfg.taskCount - 1; k++) g_cfg.tasks[k] = g_cfg.tasks[k + 1];
            g_cfg.taskCount--;
            FillList(dlg, i);
            App_Refresh();
            return TRUE;
        }
        case IDC_DONE: {
            int i = Selected(dlg);
            if (i < 0 || i >= g_cfg.taskCount) return TRUE;
            g_cfg.tasks[i].lastDone = Today();
            g_cfg.tasks[i].snoozeUntil = 0;
            FillList(dlg, i);
            App_Refresh();
            return TRUE;
        }
        case IDC_SCALE:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int sel = (int)SendDlgItemMessageW(dlg, IDC_SCALE, CB_GETCURSEL, 0, 0);
                if (sel >= 0 && sel < (int)_countof(kScales)) { g_cfg.scale = kScales[sel]; App_Refresh(); }
            }
            return TRUE;
        case IDC_SHOWRAM:
        case IDC_HOVER:
            g_cfg.showRam = IsDlgButtonChecked(dlg, IDC_SHOWRAM) == BST_CHECKED;
            g_cfg.hoverOpaque = IsDlgButtonChecked(dlg, IDC_HOVER) == BST_CHECKED;
            App_Refresh();
            return TRUE;
        case IDOK:
            ReadFields(dlg);
            EndDialog(dlg, 1);
            return TRUE;
        case IDCANCEL: {
            // Desfaz tudo, menos posição e modo (o widget pode ter sido arrastado com a janela aberta).
            Config* now = (Config*)HeapAlloc(GetProcessHeap(), 0, sizeof(Config));
            if (now) {
                *now = g_cfg;
                g_cfg = *s_backup;
                g_cfg.x = now->x; g_cfg.y = now->y; g_cfg.anchor = now->anchor; g_cfg.hasPos = now->hasPos;
                g_cfg.mode = now->mode;
                HeapFree(GetProcessHeap(), 0, now);
            }
            App_Refresh();
            EndDialog(dlg, 0);
            return TRUE;
        }
        }
        break;
    }
    return FALSE;
}

bool Settings_Show(HINSTANCE inst) {
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_DATE_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    s_backup = (Config*)HeapAlloc(GetProcessHeap(), 0, sizeof(Config));
    if (!s_backup) return false;
    *s_backup = g_cfg;
    INT_PTR r = DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_SETTINGS), NULL, SettingsProc, 0);
    if (r == -1) Log(L"Configurações: não abriu a janela (erro %u)", GetLastError());
    HeapFree(GetProcessHeap(), 0, s_backup);
    s_backup = NULL;
    s_dlg = NULL;
    return r == 1;
}
