// FPS do jogo aberto, medido de fora via ETW (a mesma técnica do PresentMon).
// Nada é injetado no jogo: só contamos os eventos de "Present" que o Windows já emite, por processo.
//   - DXGI / D3D9: uma chamada de Present do jogo = um evento (Direct3D 9/10/11/12)
//   - DxgKrnl:     o Present visto pelo kernel; cobre Vulkan e OpenGL, que não passam pelo DXGI
// Exige administrador (sessões ETW em tempo real não podem ser criadas por usuário comum).
#include "app.h"
#include <evntrace.h>
#include <evntcons.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define SESSION_NAME L"MonitorPC.Fps"

static const GUID kDxgi    = { 0xCA11C036, 0x0102, 0x4A2D, { 0xA6, 0xAD, 0xF0, 0x3C, 0xFE, 0xD5, 0xD3, 0xC9 } };
static const GUID kD3d9    = { 0x783ACA0A, 0x790E, 0x4D7F, { 0x84, 0x51, 0xAA, 0x85, 0x05, 0x11, 0xC6, 0xB9 } };
static const GUID kDxgKrnl = { 0x802EC45A, 0x1E99, 0x4B83, { 0x99, 0x20, 0x87, 0xC9, 0x82, 0x77, 0xBA, 0x9D } };

enum { EV_DXGI_PRESENT = 42, EV_DXGI_PRESENT_MPO = 55, EV_D3D9_PRESENT = 1, EV_KRNL_PRESENT = 184 };

// Presents de uma mesma origem (runtime ou kernel) de um processo.
struct Track {
    LONGLONG first, last;   // timestamps (QPC) do lote ainda não consumido
    LONGLONG prevLast;      // último timestamp do lote anterior, para medir sem emendas
    UINT32   count;
    DWORD    lastTick;
    double   fps;
};

struct Proc {
    DWORD pid;              // 0 = slot livre
    Track rt, kn;           // runtime (DXGI/D3D9) e kernel (DxgKrnl)
    DWORD lastActive;
    double fps;
    bool  resolved, ignored;
    wchar_t exe[24];
};

struct Props { EVENT_TRACE_PROPERTIES p; wchar_t name[64]; };

static Proc     s_procs[48];
static SRWLOCK  s_lock = SRWLOCK_INIT;
static TRACEHANDLE s_session;
static TRACEHANDLE s_trace = INVALID_PROCESSTRACE_HANDLE;
static HANDLE   s_thread;
static bool     s_running;
static LONGLONG s_qpf;
static DWORD    s_gamePid;
static UINT32   s_evRuntime, s_evKernel, s_samples;
static const wchar_t* s_status = L"desligado";

// Chamado na thread do ETW, com o lock tomado.
static Proc* FindOrAdd(DWORD pid) {
    Proc* freeSlot = NULL;
    Proc* oldest = &s_procs[0];
    for (Proc& p : s_procs) {
        if (p.pid == pid) return &p;
        if (!p.pid) { if (!freeSlot) freeSlot = &p; }
        else if ((LONG)(p.lastActive - oldest->lastActive) < 0 || !oldest->pid) oldest = &p;
    }
    Proc* p = freeSlot ? freeSlot : oldest;
    if (p->pid == s_gamePid && p->pid) return NULL;   // nunca despeja o jogo atual
    ZeroMemory(p, sizeof(*p));
    p->pid = pid;
    p->lastActive = GetTickCount();
    return p;
}

static void WINAPI OnEvent(PEVENT_RECORD e) {
    const EVENT_HEADER& h = e->EventHeader;
    const USHORT id = h.EventDescriptor.Id;
    bool kernel;
    if (IsEqualGUID(h.ProviderId, kDxgKrnl)) {
        if (id != EV_KRNL_PRESENT) return;
        kernel = true;
    }
    else if (IsEqualGUID(h.ProviderId, kDxgi)) {
        if (id != EV_DXGI_PRESENT && id != EV_DXGI_PRESENT_MPO) return;
        // Dados: ponteiro da swapchain + Flags. DXGI_PRESENT_TEST (0x1) não apresenta quadro nenhum.
        USHORT ptr = (h.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER) ? 4 : 8;
        if (e->UserData && e->UserDataLength >= ptr + 4) {
            UINT32 flags;
            memcpy(&flags, (const BYTE*)e->UserData + ptr, sizeof(flags));
            if (flags & 0x1) return;
        }
        kernel = false;
    }
    else if (IsEqualGUID(h.ProviderId, kD3d9)) {
        if (id != EV_D3D9_PRESENT) return;
        kernel = false;
    }
    else return;

    const LONGLONG ts = h.TimeStamp.QuadPart;
    AcquireSRWLockExclusive(&s_lock);
    if (kernel) s_evKernel++; else s_evRuntime++;
    Proc* p = FindOrAdd(h.ProcessId);
    if (p) {
        Track& t = kernel ? p->kn : p->rt;
        if (t.count == 0) { t.first = ts; t.last = ts; }
        else {
            if (ts < t.first) t.first = ts;
            if (ts > t.last) t.last = ts;
        }
        t.count++;
    }
    ReleaseSRWLockExclusive(&s_lock);
}

static DWORD WINAPI TraceThread(LPVOID) {
    ProcessTrace(&s_trace, 1, NULL, NULL);   // bloqueia até a sessão ser parada
    return 0;
}

// Consome o lote acumulado e atualiza o FPS da origem. Retorna true se houve eventos.
static bool UpdateTrack(Track& t, DWORD now) {
    if (t.count == 0) {
        // Parou de renderizar: zera e esquece a referência, para a volta não ser medida sobre a pausa.
        if (t.prevLast && now - t.lastTick > 2500) { t.fps = 0; t.prevLast = 0; }
        return false;
    }
    // Com o fim do lote anterior, N eventos cobrem exatamente (last - prevLast): a conta fica certa mesmo
    // que o ETW entregue os eventos em rajadas.
    LONGLONG t0 = t.prevLast ? t.prevLast : t.first;
    UINT32 n = t.prevLast ? t.count : t.count - 1;
    LONGLONG dt = t.last - t0;
    if (n > 0 && dt > 0) t.fps = (double)n * (double)s_qpf / (double)dt;
    t.prevLast = t.last;
    t.count = 0;
    t.lastTick = now;
    return true;
}

static bool InIgnoreList(const wchar_t* exe) {
    const size_t len = wcslen(exe);
    const wchar_t* p = g_cfg.fpsIgnore;
    while (*p) {
        while (*p == L';' || *p == L' ') p++;
        const wchar_t* e = p;
        while (*e && *e != L';') e++;
        const wchar_t* t = e;
        while (t > p && t[-1] == L' ') t--;
        if ((size_t)(t - p) == len && len && _wcsnicmp(p, exe, len) == 0) return true;
        p = e;
    }
    return false;
}

static void Resolve(Proc& p) {
    p.resolved = true;
    p.ignored = false;
    p.exe[0] = 0;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p.pid);
    if (!h) return;
    wchar_t path[MAX_PATH];
    DWORD n = MAX_PATH;
    if (QueryFullProcessImageNameW(h, 0, path, &n)) {
        const wchar_t* base = wcsrchr(path, L'\\');
        base = base ? base + 1 : path;
        p.ignored = InIgnoreList(base);
        lstrcpynW(p.exe, base, _countof(p.exe));
        wchar_t* dot = wcsrchr(p.exe, L'.');
        if (dot) *dot = 0;
    }
    CloseHandle(h);
}

// Tela cheia (exclusiva ou "sem bordas"): a janela ocupa exatamente o monitor. Janela maximizada não conta.
static bool IsFullscreen(HWND w) {
    if (!w || w == GetDesktopWindow() || w == GetShellWindow() || !IsWindowVisible(w)) return false;
    RECT r;
    MONITORINFO mi = { sizeof(mi) };
    if (!GetWindowRect(w, &r) || !GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    return EqualRect(&r, &mi.rcMonitor) != FALSE;
}

int Fps_Sample(wchar_t* game, int cch) {
    if (game && cch > 0) game[0] = 0;
    if (!s_running) return 0;

    const DWORD now = GetTickCount();
    HWND fg = GetForegroundWindow();
    DWORD fgPid = 0;
    if (fg) GetWindowThreadProcessId(fg, &fgPid);
    const bool fgEligible = fg && (g_cfg.fpsWindowed || IsFullscreen(fg));

    int result = 0;
    bool newGame = false;
    wchar_t logName[24] = L"";
    UINT32 evRt, evKn;

    AcquireSRWLockExclusive(&s_lock);
    Proc* fgProc = NULL;
    Proc* gameProc = NULL;
    for (Proc& p : s_procs) {
        if (!p.pid) continue;
        bool a = UpdateTrack(p.rt, now), b = UpdateTrack(p.kn, now);
        if (a || b) p.lastActive = now;
        else if (now - p.lastActive > 20000) { p.pid = 0; continue; }
        // O runtime é a contagem exata; o kernel entra quando não há runtime (Vulkan/OpenGL).
        p.fps = p.rt.fps > 0 ? p.rt.fps : p.kn.fps;
        if (p.pid == fgPid) fgProc = &p;
        if (p.pid == s_gamePid) gameProc = &p;
    }
    // Um app em tela cheia, em primeiro plano e renderizando vira "o jogo" e continua sendo enquanto
    // renderizar, mesmo que você clique em outra janela no segundo monitor.
    if (fgProc && fgEligible && fgProc->fps >= 5.0 && fgProc != gameProc) {
        if (!fgProc->resolved) Resolve(*fgProc);
        if (!fgProc->ignored) {
            s_gamePid = fgProc->pid;
            gameProc = fgProc;
            newGame = true;
            lstrcpynW(logName, fgProc->exe, _countof(logName));
        }
    }
    if (gameProc && gameProc->fps > 0) {
        result = (int)(gameProc->fps + 0.5);
        if (game && cch > 0) lstrcpynW(game, gameProc->exe, cch);
    }
    else s_gamePid = 0;
    evRt = s_evRuntime;
    evKn = s_evKernel;
    ReleaseSRWLockExclusive(&s_lock);

    if (newGame) Log(L"FPS: jogo detectado: %s (pid %u), %d fps", logName[0] ? logName : L"?", s_gamePid, result);
    if (++s_samples == 15) Log(L"FPS: eventos recebidos até agora: runtime=%u kernel=%u", evRt, evKn);
    return result;
}

static void FillProps(Props* props, ULONG extraMode) {
    ZeroMemory(props, sizeof(*props));
    EVENT_TRACE_PROPERTIES& p = props->p;
    p.Wnode.BufferSize = sizeof(*props);
    p.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    p.Wnode.ClientContext = 1;                    // timestamps em QueryPerformanceCounter
    p.LogFileMode = EVENT_TRACE_REAL_TIME_MODE | extraMode;
    p.BufferSize = 32;                            // KB; a taxa de eventos é baixa
    p.MinimumBuffers = 4;
    p.MaximumBuffers = 32;
    p.FlushTimer = 1;                             // entrega os eventos a cada 1 s
    p.LoggerNameOffset = offsetof(Props, name);
}

// Liga um provedor pedindo ao ETW que só entregue os IDs que interessam (o resto é descartado na origem).
static ULONG EnableProvider(const GUID& guid, ULONGLONG keywords, const USHORT* ids, USHORT count) {
    BYTE fbuf[sizeof(EVENT_FILTER_EVENT_ID) + 8 * sizeof(USHORT)] = {};
    EVENT_FILTER_EVENT_ID* f = (EVENT_FILTER_EVENT_ID*)fbuf;
    f->FilterIn = TRUE;
    f->Count = count;
    for (USHORT i = 0; i < count; i++) f->Events[i] = ids[i];

    EVENT_FILTER_DESCRIPTOR fd = {};
    fd.Ptr = (ULONGLONG)(ULONG_PTR)f;
    fd.Size = (ULONG)(FIELD_OFFSET(EVENT_FILTER_EVENT_ID, Events) + count * sizeof(USHORT));
    fd.Type = EVENT_FILTER_TYPE_EVENT_ID;

    ENABLE_TRACE_PARAMETERS ep = {};
    ep.Version = ENABLE_TRACE_PARAMETERS_VERSION_2;
    ep.EnableFilterDesc = &fd;
    ep.FilterDescCount = 1;

    ULONG st = EnableTraceEx2(s_session, &guid, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_INFORMATION, keywords,
                              0, 0, &ep);
    if (st != ERROR_SUCCESS)   // sem filtro de ID (sistemas antigos); o callback filtra do mesmo jeito
        st = EnableTraceEx2(s_session, &guid, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_INFORMATION, keywords,
                            0, 0, NULL);
    return st;
}

bool Fps_Start(bool kernelProvider) {
    if (s_running) return true;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    s_qpf = f.QuadPart;

    // Buffers compartilhados (em vez de um conjunto por núcleo) gastam menos memória; se o sistema recusar, usa o padrão.
    const ULONG modes[2] = { EVENT_TRACE_NO_PER_PROCESSOR_BUFFERING, 0 };
    Props props;
    ULONG st = ERROR_GEN_FAILURE;
    for (int i = 0; i < 2; i++) {
        FillProps(&props, modes[i]);
        st = StartTraceW(&s_session, SESSION_NAME, &props.p);
        if (st == ERROR_ALREADY_EXISTS) {
            // Sessão órfã de uma execução anterior que foi morta sem fechar: encerra e recomeça.
            FillProps(&props, 0);
            ControlTraceW(0, SESSION_NAME, &props.p, EVENT_TRACE_CONTROL_STOP);
            FillProps(&props, modes[i]);
            st = StartTraceW(&s_session, SESSION_NAME, &props.p);
        }
        if (st != ERROR_INVALID_PARAMETER) break;
    }
    if (st != ERROR_SUCCESS) {
        s_session = 0;
        s_status = st == ERROR_ACCESS_DENIED ? L"requer administrador" : L"falha ao iniciar a sessão ETW";
        Log(L"FPS: StartTrace falhou (erro %u) - %s", st, s_status);
        return false;
    }

    // Keywords conferidos com "wevtutil gp": DXGI/D3D9 Present = 0x2, DxgKrnl Present = 0x1 (Base).
    static const USHORT dxgiIds[] = { EV_DXGI_PRESENT, EV_DXGI_PRESENT_MPO };
    static const USHORT d3d9Ids[] = { EV_D3D9_PRESENT };
    static const USHORT krnlIds[] = { EV_KRNL_PRESENT };
    ULONG e1 = EnableProvider(kDxgi, 0x2, dxgiIds, _countof(dxgiIds));
    ULONG e2 = EnableProvider(kD3d9, 0x2, d3d9Ids, _countof(d3d9Ids));
    ULONG e3 = kernelProvider ? EnableProvider(kDxgKrnl, 0x1, krnlIds, _countof(krnlIds)) : ERROR_SUCCESS;

    EVENT_TRACE_LOGFILEW lf = {};
    lf.LoggerName = (LPWSTR)SESSION_NAME;
    lf.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    lf.EventRecordCallback = OnEvent;
    s_trace = OpenTraceW(&lf);
    if (s_trace == INVALID_PROCESSTRACE_HANDLE) {
        DWORD err = GetLastError();
        Log(L"FPS: OpenTrace falhou (erro %u)", err);
        FillProps(&props, 0);
        ControlTraceW(s_session, NULL, &props.p, EVENT_TRACE_CONTROL_STOP);
        s_session = 0;
        s_status = L"falha ao abrir a sessão ETW";
        return false;
    }
    s_thread = CreateThread(NULL, 0, TraceThread, NULL, 0, NULL);
    s_running = true;
    s_samples = 0;
    s_status = L"ativo";
    Log(L"FPS: sessão ETW ativa (DXGI=%u D3D9=%u DxgKrnl=%u%s)", e1, e2, e3, kernelProvider ? L"" : L" desligado");
    return true;
}

void Fps_Stop() {
    if (!s_session && !s_thread) return;
    Props props;
    if (s_session) {
        FillProps(&props, 0);
        ControlTraceW(s_session, NULL, &props.p, EVENT_TRACE_CONTROL_STOP);   // faz o ProcessTrace retornar
        s_session = 0;
    }
    if (s_trace != INVALID_PROCESSTRACE_HANDLE) {
        CloseTrace(s_trace);
        s_trace = INVALID_PROCESSTRACE_HANDLE;
    }
    if (s_thread) {
        WaitForSingleObject(s_thread, 3000);
        CloseHandle(s_thread);
        s_thread = NULL;
    }
    s_running = false;
    s_gamePid = 0;
    AcquireSRWLockExclusive(&s_lock);
    ZeroMemory(s_procs, sizeof(s_procs));
    s_evRuntime = s_evKernel = 0;
    ReleaseSRWLockExclusive(&s_lock);
    s_status = L"desligado";
}

bool Fps_Running() { return s_running; }

// A lista de processos ignorados mudou: reavalia os nomes já resolvidos.
void Fps_ConfigChanged() {
    AcquireSRWLockExclusive(&s_lock);
    for (Proc& p : s_procs) p.resolved = false;
    ReleaseSRWLockExclusive(&s_lock);
}

const wchar_t* Fps_Status() { return s_status; }
