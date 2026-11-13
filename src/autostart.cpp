// "Iniciar com o Windows" como tarefa agendada no logon, com privilégio de administrador.
// É assim (e não pela chave Run do registro) porque o Windows não abre apps elevados pelo Run, e porque a
// tarefa agendada sobe já elevada sem mostrar o UAC a cada boot.
//
// Autostart_Apply roda num processo filho ("MonitorPC.exe --autostart-on/off"): o COM fica fora do
// processo principal.
#include "app.h"
#include <objbase.h>
#include <oleauto.h>
#include <taskschd.h>
#define SECURITY_WIN32
#include <security.h>
#include <stdio.h>

static void XmlEscape(const wchar_t* in, wchar_t* out, int cch) {
    int n = 0;
    for (; *in && n < cch - 8; in++) {
        const wchar_t* rep = NULL;
        switch (*in) {
        case L'&':  rep = L"&amp;"; break;
        case L'<':  rep = L"&lt;"; break;
        case L'>':  rep = L"&gt;"; break;
        case L'"':  rep = L"&quot;"; break;
        case L'\'': rep = L"&apos;"; break;
        }
        if (rep) while (*rep) out[n++] = *rep++;
        else out[n++] = *in;
    }
    out[n] = 0;
}

// ExecutionTimeLimit PT0S: sem isso o Agendador mata a tarefa depois de 3 dias ligada.
static const wchar_t kTaskXml[] =
    L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">"
    L"<RegistrationInfo><Description>Abre o MonitorPC (overlay de monitoramento) ao entrar no Windows.</Description></RegistrationInfo>"
    L"<Triggers><LogonTrigger><Enabled>true</Enabled><UserId>%s</UserId></LogonTrigger></Triggers>"
    L"<Principals><Principal id=\"Author\"><UserId>%s</UserId><LogonType>InteractiveToken</LogonType>"
    L"<RunLevel>HighestAvailable</RunLevel></Principal></Principals>"
    L"<Settings>"
    L"<MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
    L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
    L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
    L"<AllowHardTerminate>true</AllowHardTerminate>"
    L"<StartWhenAvailable>false</StartWhenAvailable>"
    L"<RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>"
    L"<IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd><RestartOnIdle>false</RestartOnIdle></IdleSettings>"
    L"<AllowStartOnDemand>true</AllowStartOnDemand>"
    L"<Enabled>true</Enabled>"
    L"<Hidden>false</Hidden>"
    L"<RunOnlyIfIdle>false</RunOnlyIfIdle>"
    L"<WakeToRun>false</WakeToRun>"
    L"<ExecutionTimeLimit>PT0S</ExecutionTimeLimit>"
    L"<Priority>5</Priority>"
    L"</Settings>"
    L"<Actions Context=\"Author\"><Exec><Command>%s</Command></Exec></Actions>"
    L"</Task>";

int Autostart_Apply(bool enable) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        Log(L"Iniciar com o Windows: CoInitializeEx falhou (0x%08X)", (unsigned)hr);
        return 1;
    }

    ITaskService* svc = NULL;
    ITaskFolder* root = NULL;
    BSTR bRoot = SysAllocString(L"\\");
    BSTR bName = SysAllocString(SCHED_TASK);
    VARIANT none;
    VariantInit(&none);

    hr = CoCreateInstance(CLSID_TaskScheduler, NULL, CLSCTX_INPROC_SERVER, IID_ITaskService, (void**)&svc);
    if (SUCCEEDED(hr)) hr = svc->Connect(none, none, none, none);
    if (SUCCEEDED(hr)) hr = svc->GetFolder(bRoot, &root);
    if (SUCCEEDED(hr)) {
        if (!enable) {
            hr = root->DeleteTask(bName, 0);
            if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) hr = S_OK;   // já não existia
        }
        else {
            wchar_t user[300] = L"";
            ULONG n = _countof(user);
            if (!GetUserNameExW(NameSamCompatible, user, &n)) {
                DWORD m = _countof(user);
                GetUserNameW(user, &m);
            }
            wchar_t userX[640], exeX[MAX_PATH * 2];
            XmlEscape(user, userX, _countof(userX));
            XmlEscape(ExePath(), exeX, _countof(exeX));

            const int cap = 4096;
            wchar_t* xml = (wchar_t*)HeapAlloc(GetProcessHeap(), 0, cap * sizeof(wchar_t));
            hr = E_OUTOFMEMORY;
            if (xml) {
                _snwprintf_s(xml, cap, _TRUNCATE, kTaskXml, userX, userX, exeX);
                BSTR bXml = SysAllocString(xml);
                VARIANT sddl;
                VariantInit(&sddl);
                sddl.vt = VT_BSTR;
                sddl.bstrVal = SysAllocString(L"");
                IRegisteredTask* reg = NULL;
                hr = root->RegisterTask(bName, bXml, TASK_CREATE_OR_UPDATE, none, none, TASK_LOGON_INTERACTIVE_TOKEN,
                                        sddl, &reg);
                if (reg) reg->Release();
                VariantClear(&sddl);
                SysFreeString(bXml);
                HeapFree(GetProcessHeap(), 0, xml);
            }
        }
    }
    Log(L"Iniciar com o Windows: %s -> %s (0x%08X)", enable ? L"registrar tarefa" : L"remover tarefa",
        SUCCEEDED(hr) ? L"ok" : L"falhou", (unsigned)hr);

    if (root) root->Release();
    if (svc) svc->Release();
    SysFreeString(bRoot);
    SysFreeString(bName);
    CoUninitialize();
    return SUCCEEDED(hr) ? 0 : 1;
}

// Checagem barata (registro) usada pelo processo principal, sem COM.
bool Autostart_TaskExists() {
    HKEY key;
    LONG r = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                           L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Schedule\\TaskCache\\Tree\\" SCHED_TASK, 0,
                           KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);
    if (r != ERROR_SUCCESS) return false;
    RegCloseKey(key);
    return true;
}
