// Configuração em %APPDATA%\MonitorPC\config.ini (UTF-8, formato INI simples).
#include "app.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

Config g_cfg;

// Processos que renderizam o tempo todo mas não são jogos; nunca viram "o jogo" no contador de FPS.
static const wchar_t kDefaultIgnore[] =
    L"dwm.exe;explorer.exe;monitorpc.exe;chrome.exe;msedge.exe;firefox.exe;brave.exe;opera.exe;vivaldi.exe;"
    L"msedgewebview2.exe;discord.exe;code.exe;claude.exe;spotify.exe;steam.exe;steamwebhelper.exe;"
    L"applicationframehost.exe;shellexperiencehost.exe;startmenuexperiencehost.exe;searchapp.exe;searchhost.exe;"
    L"textinputhost.exe;lockapp.exe;systemsettings.exe;taskmgr.exe;windowsterminal.exe;obs64.exe;vlc.exe;"
    L"whatsapp.exe;telegram.exe;slack.exe;ms-teams.exe;notion.exe;figma.exe;devenv.exe;photoshop.exe;"
    L"epicgameslauncher.exe;battle.net.exe;nvidia app.exe;nvidia overlay.exe";

static int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void ConfigPath(wchar_t* out, const wchar_t* suffix) {
    _snwprintf_s(out, MAX_PATH, _TRUNCATE, L"%s\\config.ini%s", DataDir(), suffix);
}

void Config_Defaults(Config& c) {
    ZeroMemory(&c, sizeof(c));
    c.mode = MODE_FULL;
    c.opacity = 88;
    c.scale = 100;
    c.hoverOpaque = true;
    c.intervalMs = 1000;
    c.showRam = true;
    c.showFps = true;
    c.cpuWarn = 80;
    c.cpuCrit = 90;
    c.gpuWarn = 78;
    c.gpuCrit = 86;
    c.fpsWindowed = false;
    c.fpsKernel = true;
    lstrcpynW(c.fpsIgnore, kDefaultIgnore, _countof(c.fpsIgnore));
    c.autostart = true;
    c.elevate = true;
    c.warnDays = 7;
}

static void AddTask(Config& c, const wchar_t* name, int every, wchar_t unit, int lastDone) {
    if (c.taskCount >= MAX_TASKS) return;
    Task& t = c.tasks[c.taskCount++];
    ZeroMemory(&t, sizeof(t));
    lstrcpynW(t.name, name, TASK_NAME_LEN);
    t.every = every;
    t.unit = unit;
    t.lastDone = lastDone;
}

static void DefaultTasks(Config& c) {
    int today = Today();
    AddTask(c, L"Limpeza do PC (poeira)", 3, L'm', today);
    AddTask(c, L"Trocar pasta térmica", 2, L'a', today);
    AddTask(c, L"Manutenção geral", 6, L'm', today);
}

static wchar_t* Trim(wchar_t* s) {
    while (*s == L' ' || *s == L'\t') s++;
    wchar_t* e = s + wcslen(s);
    while (e > s && (e[-1] == L' ' || e[-1] == L'\t')) *--e = 0;
    return s;
}

static bool Is(const wchar_t* a, const wchar_t* b) { return lstrcmpiW(a, b) == 0; }

static void ApplyKey(Config& c, const wchar_t* sec, Task* task, const wchar_t* k, const wchar_t* v) {
    int n = _wtoi(v);
    if (task) {
        if (Is(k, L"nome")) lstrcpynW(task->name, v, TASK_NAME_LEN);
        else if (Is(k, L"intervalo")) task->every = Clamp(n, 1, 999);
        else if (Is(k, L"unidade")) {
            wchar_t u = (wchar_t)towlower(v[0]);
            task->unit = (u == L'd' || u == L'a') ? u : L'm';
        }
        else if (Is(k, L"ultima")) { int d = ParseDate(v); if (d) task->lastDone = d; }
        else if (Is(k, L"adiada_ate")) task->snoozeUntil = ParseDate(v);
        return;
    }
    if (Is(sec, L"janela")) {
        if (Is(k, L"x")) c.x = n;
        else if (Is(k, L"y")) c.y = n;
        else if (Is(k, L"ancora")) c.anchor = n & 3;
        else if (Is(k, L"posicionada")) c.hasPos = n != 0;
        else if (Is(k, L"modo")) c.mode = Clamp(n, MODE_FULL, MODE_MINI);
        else if (Is(k, L"opacidade")) c.opacity = Clamp(n, 30, 100);
        else if (Is(k, L"escala")) c.scale = Clamp(n, 80, 200);
        else if (Is(k, L"opaco_no_mouse")) c.hoverOpaque = n != 0;
    }
    else if (Is(sec, L"monitor")) {
        if (Is(k, L"intervalo_ms")) c.intervalMs = Clamp(n, 500, 10000);
        else if (Is(k, L"mostrar_ram")) c.showRam = n != 0;
        else if (Is(k, L"mostrar_fps")) c.showFps = n != 0;
        else if (Is(k, L"cpu_alerta")) c.cpuWarn = Clamp(n, 30, 120);
        else if (Is(k, L"cpu_critico")) c.cpuCrit = Clamp(n, 30, 120);
        else if (Is(k, L"gpu_alerta")) c.gpuWarn = Clamp(n, 30, 120);
        else if (Is(k, L"gpu_critico")) c.gpuCrit = Clamp(n, 30, 120);
        else if (Is(k, L"fps_em_janela")) c.fpsWindowed = n != 0;
        else if (Is(k, L"fps_kernel")) c.fpsKernel = n != 0;
        else if (Is(k, L"fps_ignorar")) lstrcpynW(c.fpsIgnore, v, _countof(c.fpsIgnore));
    }
    else if (Is(sec, L"geral")) {
        if (Is(k, L"iniciar_com_windows")) c.autostart = n != 0;
        else if (Is(k, L"pedir_admin")) c.elevate = n != 0;
        else if (Is(k, L"avisar_dias")) c.warnDays = Clamp(n, 0, 365);
        else if (Is(k, L"caminho_autostart")) lstrcpynW(c.autostartPath, v, _countof(c.autostartPath));
    }
}

bool Config_Load(Config& c) {
    Config_Defaults(c);

    wchar_t path[MAX_PATH];
    ConfigPath(path, L"");
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DefaultTasks(c);
        return false;
    }
    DWORD size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size > 256 * 1024) size = 0;
    HANDLE heap = GetProcessHeap();
    char* raw = (char*)HeapAlloc(heap, 0, (SIZE_T)size + 1);
    DWORD got = 0;
    if (raw) ReadFile(h, raw, size, &got, NULL);
    CloseHandle(h);
    if (!raw) return true;

    int wlen = got ? MultiByteToWideChar(CP_UTF8, 0, raw, (int)got, NULL, 0) : 0;
    wchar_t* text = (wchar_t*)HeapAlloc(heap, 0, ((SIZE_T)wlen + 1) * sizeof(wchar_t));
    if (!text) { HeapFree(heap, 0, raw); return true; }
    if (wlen) MultiByteToWideChar(CP_UTF8, 0, raw, (int)got, text, wlen);
    text[wlen] = 0;
    HeapFree(heap, 0, raw);

    wchar_t section[32] = L"";
    Task* task = NULL;
    wchar_t* line = text;
    if (*line == 0xFEFF) line++;   // BOM
    while (*line) {
        wchar_t* eol = line;
        while (*eol && *eol != L'\n' && *eol != L'\r') eol++;
        wchar_t* next = eol;
        while (*next == L'\n' || *next == L'\r') next++;
        *eol = 0;

        wchar_t* s = Trim(line);
        if (*s == L'[') {
            wchar_t* close = wcschr(s, L']');
            if (close) *close = 0;
            lstrcpynW(section, s + 1, _countof(section));
            task = NULL;
            if (_wcsnicmp(section, L"tarefa", 6) == 0 && c.taskCount < MAX_TASKS) {
                task = &c.tasks[c.taskCount++];
                ZeroMemory(task, sizeof(*task));
                lstrcpynW(task->name, L"Lembrete", TASK_NAME_LEN);
                task->every = 3;
                task->unit = L'm';
                task->lastDone = Today();
            }
        }
        else if (*s && *s != L';' && *s != L'#') {
            wchar_t* eq = wcschr(s, L'=');
            if (eq) {
                *eq = 0;
                // Seção de tarefa além do limite: ignora as chaves em vez de aplicá-las como globais.
                bool overflowTask = !task && _wcsnicmp(section, L"tarefa", 6) == 0;
                if (!overflowTask) ApplyKey(c, section, task, Trim(s), Trim(eq + 1));
            }
        }
        line = next;
    }
    HeapFree(heap, 0, text);

    if (c.cpuCrit < c.cpuWarn) c.cpuCrit = c.cpuWarn;
    if (c.gpuCrit < c.gpuWarn) c.gpuCrit = c.gpuWarn;
    return true;
}

static void Appendf(wchar_t* buf, int cap, int* len, const wchar_t* fmt, ...) {
    if (*len >= cap - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnwprintf_s(buf + *len, (size_t)(cap - *len), _TRUNCATE, fmt, ap);
    va_end(ap);
    *len = n < 0 ? cap - 1 : *len + n;
}

bool Config_Save(const Config& c) {
    const int CAP = 12 * 1024;
    HANDLE heap = GetProcessHeap();
    wchar_t* buf = (wchar_t*)HeapAlloc(heap, 0, CAP * sizeof(wchar_t));
    if (!buf) return false;
    int n = 0;

    Appendf(buf, CAP, &n, L"; MonitorPC - use o menu Configurações (botão direito no widget).\r\n"
                          L"; Se editar à mão, feche o app antes: ele regrava este arquivo.\r\n\r\n");
    Appendf(buf, CAP, &n, L"[janela]\r\nx=%d\r\ny=%d\r\nancora=%d\r\nposicionada=%d\r\nmodo=%d\r\nopacidade=%d\r\n"
                          L"escala=%d\r\nopaco_no_mouse=%d\r\n\r\n",
            c.x, c.y, c.anchor, c.hasPos ? 1 : 0, c.mode, c.opacity, c.scale, c.hoverOpaque ? 1 : 0);
    Appendf(buf, CAP, &n, L"[monitor]\r\nintervalo_ms=%d\r\nmostrar_ram=%d\r\nmostrar_fps=%d\r\ncpu_alerta=%d\r\n"
                          L"cpu_critico=%d\r\ngpu_alerta=%d\r\ngpu_critico=%d\r\nfps_em_janela=%d\r\nfps_kernel=%d\r\n"
                          L"fps_ignorar=%s\r\n\r\n",
            c.intervalMs, c.showRam ? 1 : 0, c.showFps ? 1 : 0, c.cpuWarn, c.cpuCrit, c.gpuWarn, c.gpuCrit,
            c.fpsWindowed ? 1 : 0, c.fpsKernel ? 1 : 0, c.fpsIgnore);
    Appendf(buf, CAP, &n, L"[geral]\r\niniciar_com_windows=%d\r\npedir_admin=%d\r\navisar_dias=%d\r\n"
                          L"caminho_autostart=%s\r\n\r\n",
            c.autostart ? 1 : 0, c.elevate ? 1 : 0, c.warnDays, c.autostartPath);
    for (int i = 0; i < c.taskCount; i++) {
        const Task& t = c.tasks[i];
        wchar_t last[16], snooze[16];
        FormatDateIso(t.lastDone, last, _countof(last));
        FormatDateIso(t.snoozeUntil, snooze, _countof(snooze));
        Appendf(buf, CAP, &n, L"[tarefa%d]\r\nnome=%s\r\nintervalo=%d\r\nunidade=%c\r\nultima=%s\r\nadiada_ate=%s\r\n\r\n",
                i + 1, t.name, t.every, t.unit, last, snooze);
    }

    int ulen = WideCharToMultiByte(CP_UTF8, 0, buf, n, NULL, 0, NULL, NULL);
    char* utf8 = (char*)HeapAlloc(heap, 0, (SIZE_T)ulen + 1);
    bool ok = false;
    if (utf8) {
        WideCharToMultiByte(CP_UTF8, 0, buf, n, utf8, ulen, NULL, NULL);
        // Grava em arquivo temporário e troca de uma vez: queda de energia não corrompe a configuração.
        wchar_t path[MAX_PATH], tmp[MAX_PATH];
        ConfigPath(path, L"");
        ConfigPath(tmp, L".tmp");
        HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            ok = WriteFile(h, utf8, (DWORD)ulen, &written, NULL) && written == (DWORD)ulen;
            CloseHandle(h);
            if (ok) ok = MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
            if (!ok) DeleteFileW(tmp);
        }
        HeapFree(heap, 0, utf8);
    }
    HeapFree(heap, 0, buf);
    if (!ok) Log(L"config: falha ao salvar (erro %u)", GetLastError());
    return ok;
}
