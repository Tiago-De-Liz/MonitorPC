// Lógica dos lembretes de manutenção: quando vence, quanto falta, textos.
#include "app.h"
#include <stdio.h>

// daysLeft: dias até o vencimento (negativo = atrasado) ou, se adiado, dias até o fim do adiamento.
Status Task_GetStatus(const Task& t, int today, int warnDays, int* daysLeft, bool* snoozed) {
    bool sn = t.snoozeUntil > today;
    if (snoozed) *snoozed = sn;
    if (sn) {
        // Adiado: fica quieto até a data combinada, depois volta a cobrar.
        if (daysLeft) *daysLeft = t.snoozeUntil - today;
        return ST_OK;
    }
    int left = AddInterval(t.lastDone, t.every, t.unit) - today;
    if (daysLeft) *daysLeft = left;
    if (left <= 0) return ST_DUE;
    if (left <= warnDays) return ST_SOON;
    return ST_OK;
}

Status Tasks_Worst(const Config& c, int today, int* notOkCount) {
    Status worst = ST_OK;
    int n = 0;
    for (int i = 0; i < c.taskCount; i++) {
        Status s = Task_GetStatus(c.tasks[i], today, c.warnDays, NULL, NULL);
        if (s != ST_OK) n++;
        if (s > worst) worst = s;
    }
    if (notOkCount) *notOkCount = n;
    return worst;
}

// "1 dia", "12 dias", "3 meses", "2 anos"
static void FormatSpan(int days, wchar_t* out, int cch) {
    if (days < 0) days = -days;
    if (days == 1) lstrcpynW(out, L"1 dia", cch);
    else if (days < 60) _snwprintf_s(out, cch, _TRUNCATE, L"%d dias", days);
    else if (days < 720) _snwprintf_s(out, cch, _TRUNCATE, L"%d meses", (days + 15) / 30);
    else {
        int years = (days + 182) / 365;
        _snwprintf_s(out, cch, _TRUNCATE, years == 1 ? L"%d ano" : L"%d anos", years);
    }
}

void Task_RemainingText(const Task& t, int today, int warnDays, wchar_t* out, int cch) {
    int left;
    bool snoozed;
    Status st = Task_GetStatus(t, today, warnDays, &left, &snoozed);
    wchar_t span[24];
    FormatSpan(left, span, _countof(span));
    if (snoozed) _snwprintf_s(out, cch, _TRUNCATE, L"adiada %s", span);
    else if (st == ST_DUE) {
        if (left == 0) lstrcpynW(out, L"vence hoje", cch);
        else _snwprintf_s(out, cch, _TRUNCATE, L"há %s", span);
    }
    else if (left == 1) lstrcpynW(out, L"amanhã", cch);
    else lstrcpynW(out, span, cch);
}

// "3 meses", "1 ano", "90 dias"
void Task_IntervalText(const Task& t, wchar_t* out, int cch) {
    const wchar_t* u;
    if (t.unit == L'd') u = t.every == 1 ? L"dia" : L"dias";
    else if (t.unit == L'a') u = t.every == 1 ? L"ano" : L"anos";
    else u = t.every == 1 ? L"mês" : L"meses";
    _snwprintf_s(out, cch, _TRUNCATE, L"%d %s", t.every, u);
}
