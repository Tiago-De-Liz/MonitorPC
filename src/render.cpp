// Desenho do widget, todo em software num bitmap de 32 bits que vai para a janela via UpdateLayeredWindow.
// Sem Direct2D/GDI+: o processo não carrega nada de GPU e redesenhar 1x por segundo custa microssegundos.
//
// Técnica: desenhamos tudo opaco (formas com antialias próprio, texto ClearType do GDI) e, no fim, um único
// passe recorta os cantos arredondados gerando o canal alfa. A translucidez geral é aplicada pela janela.
#include "app.h"
#include <math.h>
#include <stdio.h>

Hit g_hits[MAX_HITS];
int g_hitCount;

namespace {

// Paleta (0xRRGGBB): preto e cinzas neutros; cor só para alertas.
const DWORD C_BG      = 0x000000;
const DWORD C_BORDER  = 0x262626;
const DWORD C_TEXT    = 0xF2F2F2;
const DWORD C_DIM     = 0x8A8A8A;
const DWORD C_TRACK   = 0x262626;
const DWORD C_ACCENT  = 0xD9D9D9;
const DWORD C_OK      = 0x3DDC84;
const DWORD C_SOON    = 0xF5B83D;
const DWORD C_DUE     = 0xFF5A52;
const DWORD C_BTN     = 0x1F1F1F;
const DWORD C_BTN_HOT = 0x333333;
const DWORD C_GO      = 0x1E5A3C;
const DWORD C_GO_HOT  = 0x27734D;
const DWORD C_ON_SOON = 0x2A1E00;   // texto sobre fundo amarelo
const DWORD C_ON_DUE  = 0xFFFFFF;   // texto sobre fundo vermelho

struct Canvas { HDC dc; HBITMAP bmp; HGDIOBJ old; DWORD* px; int w, h; };
Canvas cv;
HFONT  fText, fBold, fSmall, fSmallB;
int    fontDpi;

// Estado do passe atual
const View* V;
bool drawing;        // false = só medindo
int  dpi, clipW, clipH;

inline int P(int v) { return MulDiv(v, dpi, 96); }

bool EnsureCanvas(int w, int h) {
    if (cv.dc && cv.w >= w && cv.h >= h) return true;
    int nw = ((w > cv.w ? w : cv.w) + 63) & ~63;
    int nh = ((h > cv.h ? h : cv.h) + 63) & ~63;
    if (!cv.dc) {
        HDC screen = GetDC(NULL);
        cv.dc = CreateCompatibleDC(screen);
        ReleaseDC(NULL, screen);
        if (!cv.dc) return false;
        SetBkMode(cv.dc, TRANSPARENT);
        GdiSetBatchLimit(1);   // alternamos GDI e acesso direto aos pixels: nada pode ficar pendente em lote
    }
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = nw;
    bi.bmiHeader.biHeight = -nh;   // de cima para baixo
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    DWORD* px = NULL;
    HBITMAP bmp = CreateDIBSection(cv.dc, &bi, DIB_RGB_COLORS, (void**)&px, NULL, 0);
    if (!bmp || !px) return false;
    HGDIOBJ prev = SelectObject(cv.dc, bmp);
    if (cv.bmp) DeleteObject(cv.bmp); else cv.old = prev;
    cv.bmp = bmp; cv.px = px; cv.w = nw; cv.h = nh;
    return true;
}

HFONT MakeFont(int tenthsOfPt, int weight) {
    return CreateFontW(-MulDiv(tenthsOfPt, dpi, 720), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void FreeFonts() {
    if (!fText) return;
    if (cv.dc) SelectObject(cv.dc, GetStockObject(SYSTEM_FONT));
    DeleteObject(fText); DeleteObject(fBold); DeleteObject(fSmall); DeleteObject(fSmallB);
    fText = fBold = fSmall = fSmallB = NULL;
}

void EnsureFonts() {
    if (fText && fontDpi == dpi) return;
    FreeFonts();
    fText   = MakeFont(90, FW_NORMAL);
    fBold   = MakeFont(90, FW_SEMIBOLD);
    fSmall  = MakeFont(80, FW_NORMAL);
    fSmallB = MakeFont(80, FW_SEMIBOLD);
    fontDpi = dpi;
}

// ------------------------------------------------------------------------------------ primitivas

// Mistura b sobre a com peso t (0..255).
inline DWORD Mix(DWORD a, DWORD b, int t) {
    if (t >= 255) return b;
    if (t <= 0) return a;
    DWORD rb = (((a & 0xFF00FF) * (DWORD)(255 - t) + (b & 0xFF00FF) * (DWORD)t + 0x800080) >> 8) & 0xFF00FF;
    DWORD g  = (((a & 0x00FF00) * (DWORD)(255 - t) + (b & 0x00FF00) * (DWORD)t + 0x008000) >> 8) & 0x00FF00;
    return rb | g;
}

void Fill(int x, int y, int w, int h, DWORD c) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > clipW ? clipW : x + w, y1 = y + h > clipH ? clipH : y + h;
    for (int yy = y0; yy < y1; yy++) {
        DWORD* row = cv.px + (size_t)yy * cv.w;
        for (int xx = x0; xx < x1; xx++) row[xx] = c;
    }
}

// Cobertura (0..1) do pixel (px,py) num retângulo arredondado de raio r.
inline float Coverage(int px, int py, float x0, float y0, float x1, float y1, float r) {
    float fx = px + 0.5f, fy = py + 0.5f;
    float cx = fx < x0 + r ? x0 + r : (fx > x1 - r ? x1 - r : fx);
    float cy = fy < y0 + r ? y0 + r : (fy > y1 - r ? y1 - r : fy);
    float dx = fx - cx, dy = fy - cy;
    float cov = r + 0.5f - sqrtf(dx * dx + dy * dy);
    return cov < 0 ? 0 : (cov > 1 ? 1 : cov);
}

// Retângulo arredondado com bordas suavizadas, pintado sobre o que já existe.
void FillRound(int x, int y, int w, int h, float r, DWORD c) {
    if (w <= 0 || h <= 0) return;
    float maxr = (w < h ? w : h) * 0.5f;
    if (r > maxr) r = maxr;
    if (r < 0.5f) { Fill(x, y, w, h, c); return; }
    const int ir = (int)ceilf(r);
    const float x0 = (float)x, y0 = (float)y, x1 = (float)(x + w), y1 = (float)(y + h);
    for (int yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= clipH) continue;
        const bool cornerRow = yy < y + ir || yy >= y + h - ir;
        DWORD* row = cv.px + (size_t)yy * cv.w;
        for (int xx = x; xx < x + w; xx++) {
            if (xx < 0 || xx >= clipW) continue;
            if (cornerRow && (xx < x + ir || xx >= x + w - ir)) {
                float cov = Coverage(xx, yy, x0, y0, x1, y1, r);
                if (cov <= 0) continue;
                if (cov < 1) { row[xx] = Mix(row[xx] & 0xFFFFFF, c, (int)(cov * 255.0f)); continue; }
            }
            row[xx] = c;
        }
    }
}

// Passe final: recorta a forma externa e gera o alfa (pré-multiplicado, como o UpdateLayeredWindow exige).
void ApplyShape(int w, int h, float r) {
    const int ir = (int)ceilf(r);
    for (int y = 0; y < h; y++) {
        DWORD* row = cv.px + (size_t)y * cv.w;
        const bool cornerRow = y < ir || y >= h - ir;
        for (int x = 0; x < w; x++) {
            DWORD c = row[x] & 0x00FFFFFF;
            if (cornerRow && (x < ir || x >= w - ir)) {
                float cov = Coverage(x, y, 0, 0, (float)w, (float)h, r);
                if (cov <= 0) { row[x] = 0; continue; }
                if (cov < 1) {
                    DWORD a = (DWORD)(cov * 255.0f + 0.5f);
                    DWORD rb = (((c & 0xFF00FF) * a + 0x800080) >> 8) & 0xFF00FF;
                    DWORD g  = (((c & 0x00FF00) * a + 0x008000) >> 8) & 0x00FF00;
                    row[x] = (a << 24) | rb | g;
                    continue;
                }
            }
            row[x] = c | 0xFF000000;
        }
    }
}

int TextW(const wchar_t* s, HFONT f) {
    SelectObject(cv.dc, f);
    SIZE sz = {};
    GetTextExtentPoint32W(cv.dc, s, lstrlenW(s), &sz);
    return sz.cx;
}

void Text(const wchar_t* s, int x, int y, int w, int h, DWORD rgb, HFONT f, UINT fmt) {
    if (!drawing || w <= 0) return;
    SelectObject(cv.dc, f);
    SetTextColor(cv.dc, RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF));
    RECT rc = { x, y, x + w, y + h };
    DrawTextW(cv.dc, s, -1, &rc, fmt | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

void AddHit(int x, int y, int w, int h, int kind, int param) {
    if (!drawing || g_hitCount >= MAX_HITS) return;
    Hit& hit = g_hits[g_hitCount++];
    SetRect(&hit.rc, x, y, x + w, y + h);
    hit.kind = kind;
    hit.param = param;
}

inline bool IsHot(int kind, int param) { return V->hotKind == kind && V->hotParam == param; }

DWORD StatusColor(Status s) { return s == ST_DUE ? C_DUE : (s == ST_SOON ? C_SOON : C_OK); }

DWORD TempColor(int t, int warn, int crit) {
    if (t < 0) return C_DIM;
    if (t >= crit) return C_DUE;
    if (t >= warn) return C_SOON;
    return C_TEXT;
}

void Dot(int x, int cy, DWORD c) {
    if (!drawing) return;
    const int d = P(6);
    FillRound(x, cy - d / 2, d, d, d * 0.5f, c);
}

// Sinal de "-" ou "+" centralizado numa área, com traço de 1 px lógico.
void Glyph(int x, int y, int w, int h, bool plus, DWORD c) {
    if (!drawing) return;
    const int len = P(8) | 1, th = P(1) < 1 ? 1 : P(1);
    const int cx = x + w / 2, cy = y + h / 2;
    Fill(cx - len / 2, cy - th / 2, len, th, c);
    if (plus) Fill(cx - th / 2, cy - len / 2, th, len, c);
}

void Button(int x, int y, int w, int h, const wchar_t* label, int kind, int param, bool primary) {
    if (drawing) {
        const bool hot = IsHot(kind, param);
        FillRound(x, y, w, h, (float)P(4), primary ? (hot ? C_GO_HOT : C_GO) : (hot ? C_BTN_HOT : C_BTN));
        Text(label, x, y, w, h, primary ? 0xE4F8EC : C_TEXT, fText, DT_CENTER);
    }
    AddHit(x, y, w, h, kind, param);
}

// --------------------------------------------------------------------------------- modo expandido

SIZE PaintFull() {
    const Config& c = *V->cfg;
    const Readings& r = *V->r;
    const int W = P(208), PAD = P(10), ROW = P(19), BTN = P(21);
    const int today = Today();
    int notOk = 0;
    const Status worst = Tasks_Worst(c, today, &notOk);
    // Tudo em dia: nada de lembrete na tela, só uma faixa fina com o botão de minimizar.
    // Algo vencendo: aparece o cabeçalho colorido e só os lembretes que precisam de atenção.
    const bool alert = worst != ST_OK;
    const int HDR = alert ? P(21) : P(15);
    const DWORD hdrBg = alert ? StatusColor(worst) : C_BG;
    const DWORD hdrFg = worst == ST_SOON ? C_ON_SOON : C_ON_DUE;

    if (drawing) {
        const float R = (float)P(9);
        Fill(0, 0, W, clipH, alert ? StatusColor(worst) : C_BORDER);   // vira a borda de 1 px
        FillRound(1, 1, W - 2, clipH - 2, R - 1, hdrBg);
        if (alert) {
            Fill(1, HDR, W - 2, P(10), C_BG);
            FillRound(1, HDR, W - 2, clipH - 1 - HDR, R - 1, C_BG);
        }
    }

    if (alert) {
        wchar_t s[48];
        if (notOk == 1) lstrcpyW(s, worst == ST_DUE ? L"Manutenção vencida" : L"Manutenção próxima");
        else _snwprintf_s(s, _countof(s), _TRUNCATE,
                          worst == ST_DUE ? L"%d manutenções pendentes" : L"%d manutenções próximas", notOk);
        Text(s, PAD, 1, W - PAD - BTN - P(2), HDR - 1, hdrFg, fSmallB, DT_LEFT | DT_END_ELLIPSIS);
    }
    {
        const int bx = W - BTN - 1;
        const bool hot = IsHot(HIT_COLLAPSE, 0);
        if (drawing && hot) FillRound(bx + P(2), P(2), BTN - P(4), HDR - P(3), (float)P(4), Mix(hdrBg, 0xFFFFFF, 50));
        Glyph(bx, 1, BTN, HDR - 1, false, alert ? hdrFg : (hot ? C_TEXT : C_DIM));
        AddHit(bx, 0, BTN + 1, HDR, HIT_COLLAPSE, 0);
    }

    // Sensores
    int y = HDR + (alert ? P(4) : 0);
    wchar_t s[32];
    auto row = [&](const wchar_t* label, int pct, const wchar_t* right, DWORD rightCol, HFONT rightFont) {
        if (drawing) {
            Text(label, PAD, y, P(30), ROW, C_DIM, fText, DT_LEFT);
            const int bx = PAD + P(32), bw = P(60), bh = P(4), by = y + (ROW - bh) / 2 + 1;
            FillRound(bx, by, bw, bh, bh * 0.5f, C_TRACK);
            if (pct > 0) {
                int fw = bw * (pct > 100 ? 100 : pct) / 100;
                if (fw < bh) fw = bh;
                FillRound(bx, by, fw, bh, bh * 0.5f, C_ACCENT);
            }
            wchar_t v[12];
            if (pct >= 0) _snwprintf_s(v, _countof(v), _TRUNCATE, L"%d%%", pct);
            else lstrcpyW(v, L"–");
            Text(v, bx + bw + P(2), y, P(38), ROW, C_TEXT, fBold, DT_RIGHT);
            Text(right, W - PAD - P(56), y, P(56), ROW, rightCol, rightFont, DT_RIGHT);
        }
        y += ROW;
    };
    auto tempText = [&](int t) {
        if (t >= 0) _snwprintf_s(s, _countof(s), _TRUNCATE, L"%d°C", t);
        else lstrcpyW(s, L"–");
    };
    tempText(r.cpuTemp);
    row(L"CPU", r.cpuPct, s, TempColor(r.cpuTemp, c.cpuWarn, c.cpuCrit), fBold);
    tempText(r.gpuTemp);
    row(L"GPU", r.gpuPct, s, TempColor(r.gpuTemp, c.gpuWarn, c.gpuCrit), fBold);
    if (c.showRam) {
        _snwprintf_s(s, _countof(s), _TRUNCATE, L"%d,%d GB", r.ramUsedGb10 / 10, r.ramUsedGb10 % 10);
        row(L"RAM", r.ramPct, s, C_DIM, fText);
    }
    if (c.showFps && r.fps > 0) {
        if (drawing) {
            Text(L"FPS", PAD, y, P(30), ROW, C_DIM, fText, DT_LEFT);
            _snwprintf_s(s, _countof(s), _TRUNCATE, L"%d", r.fps);
            const int vx = PAD + P(32), vw = TextW(s, fBold) + 2;
            Text(s, vx, y, vw, ROW, C_TEXT, fBold, DT_LEFT);
            const int gx = vx + vw + P(10);
            Text(r.game, gx, y, W - PAD - gx, ROW, C_DIM, fText, DT_RIGHT | DT_END_ELLIPSIS);
        }
        y += ROW;
    }

    // Lembretes: só os que estão perto de vencer ou vencidos (os em dia/adiados ficam nas Configurações).
    if (alert) {
        y += P(3);
        if (drawing) Fill(PAD, y, W - 2 * PAD, 1, C_BORDER);
        y += P(5);
        for (int i = 0; i < c.taskCount; i++) {
            const Task& t = c.tasks[i];
            const Status st = Task_GetStatus(t, today, c.warnDays, NULL, NULL);
            if (st == ST_OK) continue;
            const int h = P(18);
            const DWORD col = StatusColor(st);
            Dot(PAD, y + h / 2 + 1, col);
            if (drawing) {
                wchar_t rem[32];
                Task_RemainingText(t, today, c.warnDays, rem, _countof(rem));
                const int rw = TextW(rem, fText) + 1;
                const int nx = PAD + P(12);
                Text(rem, W - PAD - rw, y, rw, h, col, fText, DT_RIGHT);
                Text(t.name, nx, y, W - PAD - rw - P(6) - nx, h, C_TEXT, fText, DT_LEFT | DT_END_ELLIPSIS);
            }
            AddHit(0, y, W, h, HIT_TASK, i);
            y += h;
            if (st != ST_OK) {
                const int bh = P(20), bw = P(66), bx = PAD + P(12);
                Button(bx, y + P(1), bw, bh, L"Feito", HIT_DONE, i, true);
                Button(bx + bw + P(6), y + P(1), bw, bh, L"Adiar", HIT_SNOOZE, i, false);
                y += bh + P(6);
            }
        }
    }
    y += P(6);
    SIZE sz = { W, y };
    return sz;
}

// -------------------------------------------------------------------------------------- modo linha

SIZE PaintLine() {
    const Config& c = *V->cfg;
    const Readings& r = *V->r;
    const int H = P(24), PAD = P(9), GAP = P(11);
    const Status worst = Tasks_Worst(c, Today(), NULL);

    if (drawing) {
        Fill(0, 0, clipW, H, worst == ST_OK ? C_BORDER : StatusColor(worst));
        FillRound(1, 1, clipW - 2, H - 2, (float)P(8) - 1, worst == ST_OK ? C_BG : Mix(C_BG, StatusColor(worst), 40));
    }

    int x = PAD;
    if (worst != ST_OK) {   // bolinha de aviso só quando há manutenção chegando
        Dot(x, H / 2, StatusColor(worst));
        x += P(6) + P(8);
    }

    // Larguras fixas (pelo maior valor possível) para a linha não "dançar" quando os números mudam.
    const int wPct = TextW(L"100%", fBold), wTemp = TextW(L"100°", fBold);
    wchar_t s[16];
    auto seg = [&](const wchar_t* label, int pct, bool hasTemp, int temp, DWORD tempCol) {
        const int wl = TextW(label, fText);
        Text(label, x, 0, wl, H, C_DIM, fText, DT_LEFT);
        x += wl + P(4);
        if (pct >= 0) _snwprintf_s(s, _countof(s), _TRUNCATE, L"%d%%", pct);
        else lstrcpyW(s, L"–");
        Text(s, x, 0, wPct, H, C_TEXT, fBold, DT_RIGHT);
        x += wPct;
        if (hasTemp) {
            x += P(5);
            if (temp >= 0) _snwprintf_s(s, _countof(s), _TRUNCATE, L"%d°", temp);
            else lstrcpyW(s, L"–");
            Text(s, x, 0, wTemp, H, tempCol, fBold, DT_RIGHT);
            x += wTemp;
        }
        x += GAP;
    };
    seg(L"CPU", r.cpuPct, true, r.cpuTemp, TempColor(r.cpuTemp, c.cpuWarn, c.cpuCrit));
    seg(L"GPU", r.gpuPct, true, r.gpuTemp, TempColor(r.gpuTemp, c.gpuWarn, c.gpuCrit));
    if (c.showRam) seg(L"RAM", r.ramPct, false, 0, 0);
    if (c.showFps && r.fps > 0) {
        const int wv = TextW(L"999", fBold), wl = TextW(L"FPS", fText);
        _snwprintf_s(s, _countof(s), _TRUNCATE, L"%d", r.fps);
        Text(s, x, 0, wv, H, C_TEXT, fBold, DT_RIGHT);
        x += wv + P(4);
        Text(L"FPS", x, 0, wl, H, C_DIM, fText, DT_LEFT);
        x += wl + GAP;
    }
    x -= GAP - P(4);

    // [+] expande, [-] encolhe para o modo mini
    const int bw = P(20);
    const int kinds[2] = { HIT_EXPAND, HIT_TOMINI };
    for (int i = 0; i < 2; i++) {
        if (drawing && IsHot(kinds[i], 0)) FillRound(x + P(1), P(3), bw - P(2), H - P(6), (float)P(4), C_BTN_HOT);
        Glyph(x, 0, bw, H, i == 0, C_TEXT);
        AddHit(x, 0, bw, H, kinds[i], 0);
        x += bw;
    }
    x += P(3);
    SIZE sz = { x, H };
    return sz;
}

// --------------------------------------------------------------------------------------- modo mini

SIZE PaintMini() {
    const Config& c = *V->cfg;
    const int S = P(24);
    if (drawing) {
        const Status worst = Tasks_Worst(c, Today(), NULL);
        DWORD bg = worst == ST_OK ? C_BG : StatusColor(worst);
        if (IsHot(HIT_FROMMINI, 0)) bg = Mix(bg, 0xFFFFFF, 34);
        Fill(0, 0, S, S, worst == ST_OK ? C_BORDER : StatusColor(worst));
        FillRound(1, 1, S - 2, S - 2, (float)P(7) - 1, bg);
        // Ícone: três barrinhas.
        const DWORD ic = worst == ST_OK ? C_TEXT : (worst == ST_SOON ? C_ON_SOON : C_ON_DUE);
        const int bw = P(3), gap = P(2), base = S / 2 + P(5);
        const int hs[3] = { P(5), P(10), P(7) };
        int x = (S - (3 * bw + 2 * gap)) / 2;
        for (int i = 0; i < 3; i++, x += bw + gap) Fill(x, base - hs[i], bw, hs[i], ic);
    }
    AddHit(0, 0, S, S, HIT_FROMMINI, 0);
    SIZE sz = { S, S };
    return sz;
}

SIZE Paint() {
    switch (V->cfg->mode) {
    case MODE_LINE: return PaintLine();
    case MODE_MINI: return PaintMini();
    default:        return PaintFull();
    }
}

}   // namespace

void Render_Measure(const View& v, SIZE* size) {
    V = &v;
    dpi = v.dpi;
    drawing = false;
    size->cx = size->cy = 1;
    if (!EnsureCanvas(8, 8)) return;
    EnsureFonts();
    *size = Paint();
}

HDC Render_Draw(const View& v, SIZE size) {
    V = &v;
    dpi = v.dpi;
    if (!EnsureCanvas(size.cx, size.cy)) return NULL;
    EnsureFonts();
    drawing = true;
    clipW = size.cx;
    clipH = size.cy;
    g_hitCount = 0;
    Paint();
    const int radius = v.cfg->mode == MODE_FULL ? 9 : (v.cfg->mode == MODE_LINE ? 8 : 7);
    ApplyShape(size.cx, size.cy, (float)P(radius));
    drawing = false;
    return cv.dc;
}

void Render_Shutdown() {
    FreeFonts();
    if (cv.dc) {
        if (cv.old) SelectObject(cv.dc, cv.old);
        if (cv.bmp) DeleteObject(cv.bmp);
        DeleteDC(cv.dc);
    }
    ZeroMemory(&cv, sizeof(cv));
}
