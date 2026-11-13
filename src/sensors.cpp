// Leituras de hardware. Tudo aqui é barato o bastante para rodar 1x por segundo para sempre:
//   CPU %    GetSystemTimes
//   RAM      GlobalMemoryStatusEx
//   GPU      D3DKMT (mesma fonte do Gerenciador de Tarefas; não depende de DLL do fabricante)
//   CPU °C   driver PawnIO + módulo AMDFamily17 (registrador SMN de temperatura dos Ryzen)
#include "app.h"
#include <winternl.h>
#include <d3dkmthk.h>
#include <intrin.h>
#include <stdio.h>
#include <string.h>

// ------------------------------------------------------------------------------------------ CPU %

static ULONGLONG s_idle, s_kernel, s_user;
static bool s_cpuPrimed;

static ULONGLONG ToU64(const FILETIME& f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }

static int ReadCpuPct() {
    FILETIME fi, fk, fu;
    if (!GetSystemTimes(&fi, &fk, &fu)) return 0;
    ULONGLONG idle = ToU64(fi), kernel = ToU64(fk), user = ToU64(fu);   // kernel já inclui o tempo ocioso
    ULONGLONG dIdle = idle - s_idle, dTotal = (kernel - s_kernel) + (user - s_user);
    s_idle = idle; s_kernel = kernel; s_user = user;
    if (!s_cpuPrimed) { s_cpuPrimed = true; return 0; }
    if (dTotal == 0 || dIdle > dTotal) return 0;
    return (int)(((dTotal - dIdle) * 100 + dTotal / 2) / dTotal);
}

// -------------------------------------------------------------------------------------------- GPU

static D3DKMT_HANDLE s_gpu;
static LUID     s_gpuLuid;
static ULONG    s_gpuNodes;
static LONGLONG s_gpuRun[64];
static LONGLONG s_gpuQpc, s_qpf;
static bool     s_gpuPrimed;
static DWORD    s_gpuNextTry;

static void GpuCloseHandle(D3DKMT_HANDLE h) {
    D3DKMT_CLOSEADAPTER c = { h };
    D3DKMTCloseAdapter(&c);
}

static bool GpuQuery(D3DKMT_HANDLE h, KMTQUERYADAPTERINFOTYPE type, void* data, UINT size) {
    D3DKMT_QUERYADAPTERINFO q = {};
    q.hAdapter = h;
    q.Type = type;
    q.pPrivateDriverData = data;
    q.PrivateDriverDataSize = size;
    return D3DKMTQueryAdapterInfo(&q) == 0;
}

// Escolhe a placa de vídeo "de verdade": ignora adaptadores de software e prefere a que reporta temperatura.
static bool GpuOpen() {
    D3DKMT_ADAPTERINFO infos[32] = {};
    D3DKMT_ENUMADAPTERS2 ea = {};
    ea.NumAdapters = _countof(infos);
    ea.pAdapters = infos;
    if (D3DKMTEnumAdapters2(&ea) != 0) return false;

    int best = -1, bestScore = -1;
    for (ULONG i = 0; i < ea.NumAdapters; i++) {
        D3DKMT_ADAPTERTYPE type = {};
        if (!GpuQuery(infos[i].hAdapter, KMTQAITYPE_ADAPTERTYPE, &type, sizeof(type)) || type.SoftwareDevice) continue;
        D3DKMT_ADAPTER_PERFDATA perf = {};
        bool hasPerf = GpuQuery(infos[i].hAdapter, KMTQAITYPE_ADAPTERPERFDATA, &perf, sizeof(perf));
        int score = (hasPerf && perf.Temperature > 0 ? 2 : 0) + (type.HybridIntegrated ? 0 : 1);
        if (score > bestScore) { bestScore = score; best = (int)i; }
    }
    for (ULONG i = 0; i < ea.NumAdapters; i++)
        if ((int)i != best) GpuCloseHandle(infos[i].hAdapter);
    if (best < 0) return false;

    s_gpu = infos[best].hAdapter;
    s_gpuLuid = infos[best].AdapterLuid;
    s_gpuPrimed = false;

    D3DKMT_QUERYSTATISTICS qs = {};
    qs.Type = D3DKMT_QUERYSTATISTICS_ADAPTER;
    qs.AdapterLuid = s_gpuLuid;
    s_gpuNodes = D3DKMTQueryStatistics(&qs) == 0 ? qs.QueryResult.AdapterInformation.NodeCount : 0;
    if (s_gpuNodes > _countof(s_gpuRun)) s_gpuNodes = _countof(s_gpuRun);

    D3DKMT_ADAPTERREGISTRYINFO reg = {};
    GpuQuery(s_gpu, KMTQAITYPE_ADAPTERREGISTRYINFO, &reg, sizeof(reg));
    Log(L"GPU: %s (%u engines)", reg.AdapterString[0] ? reg.AdapterString : L"?", s_gpuNodes);
    return true;
}

static void ReadGpu(int* pct, int* temp) {
    *pct = *temp = -1;
    if (!s_gpu) {
        DWORD now = GetTickCount();
        if ((LONG)(now - s_gpuNextTry) < 0) return;
        s_gpuNextTry = now + 15000;
        if (!GpuOpen()) return;
    }

    D3DKMT_ADAPTER_PERFDATA perf = {};
    if (!GpuQuery(s_gpu, KMTQAITYPE_ADAPTERPERFDATA, &perf, sizeof(perf))) {
        // Handle inválido (driver reiniciou/atualizou): reabre na próxima tentativa.
        // Se a GPU simplesmente não reporta temperatura, o uso continua funcionando abaixo.
        D3DKMT_ADAPTERTYPE type = {};
        if (!GpuQuery(s_gpu, KMTQAITYPE_ADAPTERTYPE, &type, sizeof(type))) {
            GpuCloseHandle(s_gpu);
            s_gpu = 0;
            return;
        }
    }
    else if (perf.Temperature > 0) *temp = (int)((perf.Temperature + 5) / 10);   // décimos de °C

    // Uso = engine mais ocupada no intervalo (igual ao número principal do Gerenciador de Tarefas).
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    LONGLONG maxDelta = 0;
    for (ULONG n = 0; n < s_gpuNodes; n++) {
        D3DKMT_QUERYSTATISTICS qs = {};
        qs.Type = D3DKMT_QUERYSTATISTICS_NODE;
        qs.AdapterLuid = s_gpuLuid;
        qs.QueryNode.NodeId = n;
        if (D3DKMTQueryStatistics(&qs) != 0) continue;
        LONGLONG run = qs.QueryResult.NodeInformation.GlobalInformation.RunningTime.QuadPart;   // unidades de 100 ns
        LONGLONG d = run - s_gpuRun[n];
        s_gpuRun[n] = run;
        if (d > maxDelta) maxDelta = d;
    }
    if (s_gpuPrimed && s_gpuNodes) {
        double elapsed = (double)(now.QuadPart - s_gpuQpc) * 1e7 / (double)s_qpf;
        if (elapsed > 0) {
            int p = (int)((double)maxDelta * 100.0 / elapsed + 0.5);
            *pct = p < 0 ? 0 : (p > 100 ? 100 : p);
        }
    }
    s_gpuQpc = now.QuadPart;
    s_gpuPrimed = true;
}

// -------------------------------------------------------------------------- CPU °C (PawnIO, AMD Zen)

// Interface do driver PawnIO (ver PawnIo.cs do LibreHardwareMonitor).
#define PAWNIO_DEVICE       L"\\\\?\\GLOBALROOT\\Device\\PawnIO"
#define PAWNIO_LOAD_BINARY  0xA1B22084u   // (41394 << 16) | (0x821 << 2)
#define PAWNIO_EXECUTE_FN   0xA1B22104u   // (41394 << 16) | (0x841 << 2)
#define SMN_THM_TCON_CUR_TMP 0x00059800u  // temperatura de controle (Tctl/Tdie), bits 31:21 em passos de 0,125 °C
#define TEMP_RANGE_SEL      0x00080000u   // quando ligado, subtrair 49 °C
#define TEMP_TJ_SEL         0x00030000u

static HANDLE s_pio = INVALID_HANDLE_VALUE;
static HANDLE s_pciMutex;
static bool   s_cpuChecked, s_cpuSupported;
static float  s_tctlOffset;
static DWORD  s_pioNextTry;
static DWORD  s_pioLastErr = 0xFFFFFFFF;
static int    s_pioFails, s_lastTemp = -1;
static const wchar_t* s_pioStatus = L"aguardando";

static void DetectCpu() {
    s_cpuChecked = true;
    int r[4];
    __cpuid(r, 0);
    bool amd = r[1] == 0x68747541 && r[3] == 0x69746E65 && r[2] == 0x444D4163;   // "AuthenticAMD"
    __cpuid(r, 1);
    int family = (r[0] >> 8) & 0xF;
    if (family == 0xF) family += (r[0] >> 20) & 0xFF;

    char brand[49] = {};
    __cpuid(r, (int)0x80000000);
    if ((unsigned)r[0] >= 0x80000004u) {
        for (int i = 0; i < 3; i++) {
            __cpuid(r, (int)(0x80000002u + i));
            memcpy(brand + i * 16, r, 16);
        }
    }
    s_cpuSupported = amd && family >= 0x17 && family <= 0x1A;

    // Alguns Ryzen antigos reportam Tctl com um deslocamento fixo (tabela do driver k10temp do Linux).
    if (strstr(brand, "1600X") || strstr(brand, "1700X") || strstr(brand, "1800X")) s_tctlOffset = -20.0f;
    else if (strstr(brand, "Threadripper 19") || strstr(brand, "Threadripper 29")) s_tctlOffset = -27.0f;
    else if (strstr(brand, "2700X")) s_tctlOffset = -10.0f;

    Log(L"CPU: %S (família %Xh)%s", brand, family,
        s_cpuSupported ? L"" : L" - temperatura da CPU só é suportada em AMD Zen");
}

static void PioFail(const wchar_t* status, DWORD err, const wchar_t* what) {
    s_pioStatus = status;
    if (err != s_pioLastErr) {   // loga só quando o motivo muda, para não encher o log a cada tentativa
        s_pioLastErr = err;
        Log(L"Temp. CPU: %s (erro %u)", what, err);
    }
}

static bool PawnIoOpen() {
    if (!s_cpuChecked) DetectCpu();
    if (!s_cpuSupported) { s_pioStatus = L"CPU não suportada (só AMD Zen)"; return false; }

    HANDLE dev = CreateFileW(PAWNIO_DEVICE, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (dev == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_ACCESS_DENIED) PioFail(L"requer administrador", e, L"sem permissão para abrir o driver PawnIO");
        else PioFail(L"driver PawnIO não instalado", e, L"driver PawnIO não encontrado");
        return false;
    }

    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _countof(path), _TRUNCATE, L"%s\\AMDFamily17.bin", ExeDir());
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        PioFail(L"falta o arquivo AMDFamily17.bin", 100000 + GetLastError(), L"módulo AMDFamily17.bin não encontrado ao lado do exe");
        CloseHandle(dev);
        return false;
    }
    DWORD size = GetFileSize(f, NULL), got = 0;
    BYTE* blob = (size && size < 4 * 1024 * 1024) ? (BYTE*)HeapAlloc(GetProcessHeap(), 0, size) : NULL;
    bool loaded = false;
    DWORD err = 0;
    if (blob && ReadFile(f, blob, size, &got, NULL) && got == size) {
        DWORD ret = 0;
        loaded = DeviceIoControl(dev, PAWNIO_LOAD_BINARY, blob, size, NULL, 0, &ret, NULL) != FALSE;
        if (!loaded) err = GetLastError();
    }
    if (blob) HeapFree(GetProcessHeap(), 0, blob);
    CloseHandle(f);
    if (!loaded) {
        PioFail(L"módulo recusado pelo driver", 200000 + err, L"o driver PawnIO recusou o módulo AMDFamily17.bin");
        CloseHandle(dev);
        return false;
    }

    // Mutex que as ferramentas de monitoramento combinam entre si para não se atropelarem no barramento PCI.
    if (!s_pciMutex) {
        SECURITY_DESCRIPTOR sd;
        InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
        SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);
        SECURITY_ATTRIBUTES sa = { sizeof(sa), &sd, FALSE };
        s_pciMutex = CreateMutexW(&sa, FALSE, L"Global\\Access_PCI");
        if (!s_pciMutex) s_pciMutex = OpenMutexW(SYNCHRONIZE, FALSE, L"Global\\Access_PCI");
    }

    s_pio = dev;
    s_pioFails = 0;
    s_pioLastErr = 0xFFFFFFFF;
    s_pioStatus = L"ok (PawnIO)";
    Log(L"Temp. CPU: PawnIO aberto e módulo AMDFamily17 carregado");
    return true;
}

static void PawnIoClose() {
    if (s_pio != INVALID_HANDLE_VALUE) { CloseHandle(s_pio); s_pio = INVALID_HANDLE_VALUE; }
    s_lastTemp = -1;
}

static int ReadCpuTemp() {
    if (s_pio == INVALID_HANDLE_VALUE) {
        DWORD now = GetTickCount();
        if ((LONG)(now - s_pioNextTry) < 0) return -1;
        s_pioNextTry = now + 30000;   // tenta de novo a cada 30 s: basta instalar o driver, sem reiniciar o app
        if (!PawnIoOpen()) return -1;
    }

    DWORD w = s_pciMutex ? WaitForSingleObject(s_pciMutex, 10) : WAIT_OBJECT_0;
    if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED) return s_lastTemp;   // barramento ocupado: mantém a última leitura

    struct { char name[32]; ULONG64 in; } req = {};
    lstrcpyA(req.name, "ioctl_read_smn");
    req.in = SMN_THM_TCON_CUR_TMP;
    ULONG64 out = 0;
    DWORD ret = 0;
    BOOL ok = DeviceIoControl(s_pio, PAWNIO_EXECUTE_FN, &req, sizeof(req), &out, sizeof(out), &ret, NULL);
    DWORD err = ok ? 0 : GetLastError();
    if (s_pciMutex) ReleaseMutex(s_pciMutex);

    if (!ok || ret < sizeof(out)) {
        if (++s_pioFails >= 5) {
            PioFail(L"falha na leitura", 300000 + err, L"leitura do registrador de temperatura falhou");
            PawnIoClose();
        }
        return s_lastTemp;
    }
    s_pioFails = 0;

    UINT32 raw = (UINT32)out;
    float t = (float)(raw >> 21) * 0.125f;
    if ((raw & TEMP_RANGE_SEL) || (raw & TEMP_TJ_SEL) == TEMP_TJ_SEL) t -= 49.0f;
    t += s_tctlOffset;
    if (t <= 0.0f || t >= 125.0f) return s_lastTemp;
    if (s_lastTemp < 0) Log(L"Temp. CPU: primeira leitura = %d °C (bruto 0x%08X)", (int)(t + 0.5f), raw);
    s_lastTemp = (int)(t + 0.5f);
    return s_lastTemp;
}

const wchar_t* Sensors_CpuTempStatus() { return s_pioStatus; }

// -------------------------------------------------------------------------------------------------

void Sensors_Init() {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    s_qpf = f.QuadPart;
    Readings warmup;
    Sensors_Read(warmup);   // primeira amostra só serve de referência para os deltas
}

void Sensors_Read(Readings& r) {
    r.cpuPct = ReadCpuPct();
    r.cpuTemp = ReadCpuTemp();
    ReadGpu(&r.gpuPct, &r.gpuTemp);

    MEMORYSTATUSEX ms = { sizeof(ms) };
    GlobalMemoryStatusEx(&ms);
    r.ramPct = (int)ms.dwMemoryLoad;
    r.ramUsedGb10 = (int)(((ms.ullTotalPhys - ms.ullAvailPhys) * 10 + (1ull << 29)) >> 30);
}

void Sensors_Shutdown() {
    PawnIoClose();
    if (s_pciMutex) { CloseHandle(s_pciMutex); s_pciMutex = NULL; }
    if (s_gpu) { GpuCloseHandle(s_gpu); s_gpu = 0; }
}
