#include "app.h"
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>

static wchar_t s_exePath[MAX_PATH], s_exeDir[MAX_PATH], s_dataDir[MAX_PATH];

const wchar_t* ExePath() {
    if (!s_exePath[0]) GetModuleFileNameW(NULL, s_exePath, MAX_PATH);
    return s_exePath;
}

const wchar_t* ExeDir() {
    if (!s_exeDir[0]) {
        lstrcpynW(s_exeDir, ExePath(), MAX_PATH);
        wchar_t* p = wcsrchr(s_exeDir, L'\\');
        if (p) *p = 0;
    }
    return s_exeDir;
}

// %APPDATA%\MonitorPC (cai para a pasta do exe se APPDATA não existir).
const wchar_t* DataDir() {
    if (!s_dataDir[0]) {
        wchar_t base[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"APPDATA", base, MAX_PATH);
        if (n == 0 || n >= MAX_PATH - 16) lstrcpynW(base, ExeDir(), MAX_PATH - 16);
        _snwprintf_s(s_dataDir, _countof(s_dataDir), _TRUNCATE, L"%s\\MonitorPC", base);
        CreateDirectoryW(s_dataDir, NULL);
    }
    return s_dataDir;
}

static void LogPath(wchar_t* out) {
    _snwprintf_s(out, MAX_PATH, _TRUNCATE, L"%s\\monitorpc.log", DataDir());
}

// Mantém o log pequeno: se passar de 256 KB, recomeça.
void Log_Rotate() {
    wchar_t path[MAX_PATH];
    LogPath(path);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &fa) && (fa.nFileSizeHigh || fa.nFileSizeLow > 256 * 1024))
        DeleteFileW(path);
}

void Log(const wchar_t* fmt, ...) {
    wchar_t msg[768];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(msg, _countof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t line[900];
    int n = _snwprintf_s(line, _countof(line), _TRUNCATE, L"%04d-%02d-%02d %02d:%02d:%02d  %s\r\n",
                         st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, msg);
    if (n < 0) n = (int)wcslen(line);

    char utf8[2800];
    int len = WideCharToMultiByte(CP_UTF8, 0, line, n, utf8, sizeof(utf8), NULL, NULL);
    if (len <= 0) return;

    wchar_t path[MAX_PATH];
    LogPath(path);
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(h, utf8, (DWORD)len, &written, NULL);
        CloseHandle(h);
    }
}

bool IsElevated() {
    HANDLE tok;
    bool elevated = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        TOKEN_ELEVATION e;
        DWORD cb;
        if (GetTokenInformation(tok, TokenElevation, &e, sizeof(e), &cb)) elevated = e.TokenIsElevated != 0;
        CloseHandle(tok);
    }
    return elevated;
}

// ---------------------------------------------------------------------------------------------
// Datas. Algoritmos de calendário civil (Howard Hinnant); dia 0 = 1970-01-01.

int DaysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (unsigned)(m + (m > 2 ? -3 : 9)) + 2) / 5 + (unsigned)d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int)doe - 719468;
}

void CivilFromDays(int z, int* y, int* m, int* d) {
    z += 719468;
    const int era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)yoe + era * 400 + (*m <= 2);
}

int Today() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return DaysFromCivil(st.wYear, st.wMonth, st.wDay);
}

static int DaysInMonth(int y, int m) {
    static const int k[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return k[m - 1];
}

// Soma n unidades a um dia. Meses/anos usam aritmética de calendário (31/jan + 1 mês = 28/fev).
int AddInterval(int day, int n, wchar_t unit) {
    if (unit == L'd') return day + n;
    int months = unit == L'a' ? n * 12 : n;
    int y, m, d;
    CivilFromDays(day, &y, &m, &d);
    int total = y * 12 + (m - 1) + months;
    y = total / 12;
    m = total % 12 + 1;
    int dim = DaysInMonth(y, m);
    if (d > dim) d = dim;
    return DaysFromCivil(y, m, d);
}

int ParseDate(const wchar_t* s) {
    int y = 0, m = 0, d = 0;
    if (!s || swscanf_s(s, L"%d-%d-%d", &y, &m, &d) != 3) return 0;
    if (y < 1990 || y > 2200 || m < 1 || m > 12 || d < 1 || d > DaysInMonth(y, m)) return 0;
    return DaysFromCivil(y, m, d);
}

void FormatDateIso(int day, wchar_t* out, int cch) {
    if (day <= 0) { out[0] = 0; return; }
    int y, m, d;
    CivilFromDays(day, &y, &m, &d);
    _snwprintf_s(out, cch, _TRUNCATE, L"%04d-%02d-%02d", y, m, d);
}

void FormatDateBr(int day, wchar_t* out, int cch) {
    if (day <= 0) { out[0] = 0; return; }
    int y, m, d;
    CivilFromDays(day, &y, &m, &d);
    _snwprintf_s(out, cch, _TRUNCATE, L"%02d/%02d/%04d", d, m, y);
}
