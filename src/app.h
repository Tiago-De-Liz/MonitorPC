// MonitorPC - overlay leve de monitoramento (CPU/GPU/RAM/FPS) + lembretes de manutenção.
#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdlib.h>   // _countof

#define APP_NAME     L"MonitorPC"
#define APP_VERSION   L"1.0"
#define WND_CLASS     L"MonitorPC.Overlay"
#define QUIT_MESSAGE  L"MonitorPC.Quit"
#define SCHED_TASK    L"MonitorPC"

#define MAX_TASKS     12
#define TASK_NAME_LEN 48

enum Mode   { MODE_FULL = 0, MODE_LINE = 1, MODE_MINI = 2 };
enum Status { ST_OK = 0, ST_SOON = 1, ST_DUE = 2 };

// Um lembrete de manutenção. Datas são "número do dia" (dias desde 1970-01-01, data local).
struct Task {
    wchar_t name[TASK_NAME_LEN];
    int     every;        // quantidade de unidades entre uma execução e outra
    wchar_t unit;         // L'd' dias, L'm' meses, L'a' anos
    int     lastDone;     // dia em que foi feito pela última vez
    int     snoozeUntil;  // dia até o qual está adiado; 0 = não adiado
};

struct Config {
    // [janela]
    int  x, y;            // ponto de ancoragem em coordenadas da área de trabalho virtual
    int  anchor;          // bit0 = ancora pela direita, bit1 = ancora por baixo
    bool hasPos;
    int  mode;
    int  opacity;         // 30..100 (%)
    int  scale;           // 80..200 (%)
    bool hoverOpaque;
    // [monitor]
    int  intervalMs;
    bool showRam, showFps;
    int  cpuWarn, cpuCrit, gpuWarn, gpuCrit;   // limites de temperatura (°C)
    bool fpsWindowed;     // também medir FPS de apps em janela (não só tela cheia)
    bool fpsKernel;       // usar o provedor DxgKrnl (pega Vulkan/OpenGL)
    wchar_t fpsIgnore[1024];
    // [geral]
    bool autostart;
    bool elevate;         // pedir administrador ao abrir
    int  warnDays;        // avisar com quantos dias de antecedência
    wchar_t autostartPath[MAX_PATH];
    // [tarefaN]
    Task tasks[MAX_TASKS];
    int  taskCount;
};

struct Readings {
    int cpuPct;           // 0..100
    int cpuTemp;          // °C; -1 = indisponível
    int gpuPct;           // -1 = indisponível
    int gpuTemp;          // -1 = indisponível
    int ramPct;
    int ramUsedGb10;      // GB usados x10
    int fps;              // 0 = sem jogo
    wchar_t game[24];
};

// ---- util.cpp ----
void  Log(const wchar_t* fmt, ...);
void  Log_Rotate();
const wchar_t* ExePath();
const wchar_t* ExeDir();
const wchar_t* DataDir();
bool  IsElevated();
int   Today();
int   DaysFromCivil(int y, int m, int d);
void  CivilFromDays(int z, int* y, int* m, int* d);
int   AddInterval(int day, int n, wchar_t unit);
int   ParseDate(const wchar_t* s);                         // "AAAA-MM-DD" -> dia; 0 se inválido
void  FormatDateIso(int day, wchar_t* out, int cch);       // "AAAA-MM-DD"; vazio se day <= 0
void  FormatDateBr(int day, wchar_t* out, int cch);        // "DD/MM/AAAA"

// ---- config.cpp ----
extern Config g_cfg;
void  Config_Defaults(Config& c);
bool  Config_Load(Config& c);          // false = arquivo não existia (padrões aplicados)
bool  Config_Save(const Config& c);

// ---- tasks.cpp ----
Status Task_GetStatus(const Task& t, int today, int warnDays, int* daysLeft, bool* snoozed);
Status Tasks_Worst(const Config& c, int today, int* notOkCount);
void   Task_RemainingText(const Task& t, int today, int warnDays, wchar_t* out, int cch);
void   Task_IntervalText(const Task& t, wchar_t* out, int cch);

// ---- sensors.cpp ----
void  Sensors_Init();
void  Sensors_Read(Readings& r);
void  Sensors_Shutdown();
const wchar_t* Sensors_CpuTempStatus();

// ---- fps.cpp ----
bool  Fps_Start(bool kernelProvider);
void  Fps_Stop();
bool  Fps_Running();
int   Fps_Sample(wchar_t* game, int cch);   // FPS do jogo detectado; 0 = nenhum
void  Fps_ConfigChanged();
const wchar_t* Fps_Status();

// ---- render.cpp ----
enum HitKind { HIT_NONE = 0, HIT_COLLAPSE, HIT_EXPAND, HIT_TOMINI, HIT_FROMMINI, HIT_DONE, HIT_SNOOZE, HIT_TASK };
struct Hit { RECT rc; int kind; int param; };
#define MAX_HITS 48
extern Hit g_hits[MAX_HITS];
extern int g_hitCount;

struct View {
    const Config*   cfg;
    const Readings* r;
    int dpi;              // DPI efetivo: DPI do monitor x escala do usuário
    int hotKind, hotParam;
};
void  Render_Measure(const View& v, SIZE* size);
HDC   Render_Draw(const View& v, SIZE size);
void  Render_Shutdown();

// ---- settings.cpp ----
bool  Settings_Show(HINSTANCE inst);   // true = usuário confirmou (g_cfg já alterado)
bool  Settings_IsOpen();

// ---- autostart.cpp ----
int   Autostart_Apply(bool enable);    // roda no processo filho (--autostart-on/off); 0 = ok
bool  Autostart_TaskExists();

// ---- main.cpp ----
void  App_Refresh();
