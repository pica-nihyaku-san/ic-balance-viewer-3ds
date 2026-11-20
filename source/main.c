// IC Balance Viewer v2 for New 3DS (Japanese model, built-in FeliCa reader)
// ---------------------------------------------------------------------------
// Read-only viewer for Japanese transit IC cards (Suica / PASMO / ICOCA /
// Kitaca / TOICA / manaca / SUGOCA / nimoca / Hayakaken ...).
// Shows the card balance and the last 20 usage-history entries with
// station names resolved from a bundled station database.
// No payment functionality, no writes to the card: it only uses FeliCa's
// unauthenticated "Read Without Encryption" command, the same access a
// station balance checker uses.
//
// v2 adds a graphical card-style UI (citro2d, design A), full Japanese
// text via the system font, and station-name lookup (stations.bin:
// RomFS-bundled, replaceable via sdmc:/ic-balance/stations.bin).
// The NFC read parameters are unchanged from v1 and were verified on
// real hardware by the PoC (2026-11-09):
//   - nfcInit(NFC_OpType_RawNFC), nfcStartOtherTagScanning(0, 0x04)
//   - TagInfo id[0..7] is the IDm; frames carry the leading length byte.
//   - Balance: service 0x008B block 0, LE u16 at bytes 11-12.
//   - History: service 0x090F, 20 blocks; balance LE u16 at bytes 10-11,
//     date packed in bytes 4-5, region code in byte 15.

#include <3ds.h>
#include <3ds/services/nfc.h>
#include <citro2d.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "stations.h"

#define NFC_TIMEOUT_US (200ULL * 1000000ULL)
#define FELICA_MASK 0x04

// ---------------------------------------------------------------------------
// Colours (design A: card style)
// ---------------------------------------------------------------------------
#define COL_BG      C2D_Color32(247, 245, 239, 255)
#define COL_BG_BOT  C2D_Color32(252, 251, 247, 255)
#define COL_HEADER  C2D_Color32(52, 110, 28, 255)
#define COL_CARD    C2D_Color32(128, 194, 65, 255)
#define COL_CARD_D  C2D_Color32(104, 168, 48, 255)
#define COL_CHIP    C2D_Color32(245, 205, 90, 255)
#define COL_INK     C2D_Color32(30, 34, 30, 255)
#define COL_GREY    C2D_Color32(120, 128, 120, 255)
#define COL_LINE    C2D_Color32(225, 222, 210, 255)
#define COL_WHITE   C2D_Color32(255, 255, 255, 255)
#define COL_NEG     C2D_Color32(192, 57, 43, 255)
#define COL_POS     C2D_Color32(30, 130, 60, 255)
#define COL_CARDLN  C2D_Color32(235, 246, 226, 255)

static C3D_RenderTarget *scr_top, *scr_bot;
static C2D_TextBuf tbuf;

// ---------------------------------------------------------------------------
// Text helpers (system font; UTF-8, Japanese OK)
// ---------------------------------------------------------------------------
static float text_w(const char *s, float scale)
{
    C2D_Text t;
    C2D_TextParse(&t, tbuf, s);
    float w, h;
    C2D_TextGetDimensions(&t, scale, scale, &w, &h);
    return w;
}

static void draw_text(const char *s, float x, float y, float scale, u32 color)
{
    C2D_Text t;
    C2D_TextParse(&t, tbuf, s);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, x, y, 0.0f, scale, scale, color);
}

static void draw_text_r(const char *s, float x_right, float y, float scale, u32 color)
{
    draw_text(s, x_right - text_w(s, scale), y, scale, color);
}

static void draw_text_c(const char *s, float cx, float y, float scale, u32 color)
{
    draw_text(s, cx - text_w(s, scale) / 2.0f, y, scale, color);
}

// Draw text shrinking the scale until it fits maxw.
static void draw_text_fit(const char *s, float x, float y, float scale,
                          float maxw, u32 color)
{
    float w = text_w(s, scale);
    if (w > maxw && w > 0) scale *= maxw / w;
    draw_text(s, x, y, scale, color);
}

static void rrect(float x, float y, float w, float h, float r, u32 color)
{
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    C2D_DrawRectSolid(x + r, y, 0.0f, w - 2 * r, h, color);
    C2D_DrawRectSolid(x, y + r, 0.0f, w, h - 2 * r, color);
    C2D_DrawCircleSolid(x + r, y + r, 0.0f, r, color);
    C2D_DrawCircleSolid(x + w - r, y + r, 0.0f, r, color);
    C2D_DrawCircleSolid(x + r, y + h - r, 0.0f, r, color);
    C2D_DrawCircleSolid(x + w - r, y + h - r, 0.0f, r, color);
}

// ---------------------------------------------------------------------------
// Formatting helpers
// ---------------------------------------------------------------------------
static void fmt_yen(char *out, size_t n, int v)
{
    char tmp[24];
    snprintf(tmp, sizeof(tmp), "%d", v);
    int len = strlen(tmp), neg = (tmp[0] == '-');
    char *p = out, *q = tmp + neg;
    if (neg) *p++ = '-';
    int rem = strlen(q);
    for (int i = 0; q[i]; i++) {
        *p++ = q[i];
        if (--rem > 0 && rem % 3 == 0 && (size_t)(p - out) < n - 2) *p++ = ',';
    }
    *p = 0;
    (void)len;
}

// ---------------------------------------------------------------------------
// FeliCa helpers (frames always carry the leading length byte) — from v1
// ---------------------------------------------------------------------------
static int build_rwe(u8 *out, const u8 *idm, u16 service,
                     const u8 *blocks, int nblocks)
{
    int n = 0;
    out[n++] = 0;
    out[n++] = 0x06;
    memcpy(out + n, idm, 8); n += 8;
    out[n++] = 0x01;
    out[n++] = (u8)(service & 0xFF);
    out[n++] = (u8)(service >> 8);
    out[n++] = (u8)nblocks;
    for (int i = 0; i < nblocks; i++) {
        out[n++] = 0x80;
        out[n++] = blocks[i];
    }
    out[0] = (u8)n;
    return n;
}

static int parse_rwe(const u8 *resp, int len, const u8 *idm,
                     u8 blocks_out[][16], int maxblocks)
{
    for (int i = 0; i + 8 <= len; i++) {
        if (memcmp(resp + i, idm, 8) != 0) continue;
        int p = i + 8;
        if (len - p < 3) return -1;
        if (resp[p] != 0 || resp[p + 1] != 0) return -2;
        int nb = resp[p + 2];
        int avail = (len - (p + 3)) / 16;
        int copy = nb;
        if (copy > avail) copy = avail;
        if (copy > maxblocks) copy = maxblocks;
        for (int b = 0; b < copy; b++)
            memcpy(blocks_out[b], resp + p + 3 + b * 16, 16);
        return copy;
    }
    return -1;
}

static int read_blocks(const u8 *idm, u16 service, int first_block,
                       int nblocks, u8 blocks_out[][16])
{
    u8 cmd[80];
    static u8 resp[0x800];
    u8 list[16];
    for (int i = 0; i < nblocks; i++) list[i] = (u8)(first_block + i);
    int clen = build_rwe(cmd, idm, service, list, nblocks);
    size_t actual = 0;
    Result ret = nfcSendTagCommand(cmd, clen, resp, sizeof(resp), &actual,
                                   NFC_TIMEOUT_US);
    if (R_FAILED(ret)) return -10;
    return parse_rwe(resp, (int)actual, idm, blocks_out, nblocks);
}

// ---------------------------------------------------------------------------
// History decoding
// ---------------------------------------------------------------------------
typedef struct {
    int year, mon, day;
    int proc, term;
    int entry_line, entry_sta, exit_line, exit_sta;
    int balance;
    int region;        // byte 15
    int amount;        // delta vs next older entry (valid if has_amount)
    int has_amount;
    char route[128];   // display route, station names when resolvable
} HistEntry;

static HistEntry hist[20];
static int hist_count = 0;
static int card_balance = 0;
static char card_idm[17] = "";
static int db_count = -1;

static void set_history(u8 raw[][16], int nhist);

static int block_is_empty(const u8 *b)
{
    for (int i = 0; i < 16; i++) if (b[i] != 0 && b[i] != 0xFF) return 0;
    return 1;
}

static const char *proc_label_jp(int proc)
{
    switch (proc) {
        case 0x01: return "乗車";
        case 0x02: return "チャージ";
        case 0x03: return "切符";
        case 0x04: return "精算";
        case 0x05: return "入場";
        case 0x06: return "出場";
        case 0x0D: case 0x0F: case 0x1F: return "バス";
        case 0x13: return "新幹線";
        case 0x46: case 0x49: return "買物";
        default:   return "その他";
    }
}

static u32 proc_color(int proc)
{
    switch (proc) {
        case 0x01: return C2D_Color32(52, 110, 28, 255);
        case 0x02: return C2D_Color32(214, 137, 16, 255);
        case 0x0D: case 0x0F: case 0x1F: return C2D_Color32(70, 110, 160, 255);
        case 0x46: case 0x49: return C2D_Color32(140, 90, 170, 255);
        case 0x05: case 0x06: return C2D_Color32(0, 140, 140, 255);
        case 0x13: return C2D_Color32(30, 90, 160, 255);
        case 0x04: return C2D_Color32(110, 110, 110, 255);
        default:   return C2D_Color32(130, 130, 130, 255);
    }
}

static int is_train_proc(int proc)
{
    return proc == 0x01 || proc == 0x04 || proc == 0x05 || proc == 0x06 ||
           proc == 0x13;
}

// Format one station: resolved name, else raw hex code.
static void fmt_station(char *out, size_t n, int line, int sta, int region)
{
    const char *ln, *sn;
    if (station_resolve(line, sta, region, &ln, &sn)) {
        snprintf(out, n, "%s", sn);
    } else {
        snprintf(out, n, "(%02X%02X)", line, sta);
    }
}

// Build the display route for an entry. with_line prefixes the line name
// (used on the roomier bottom screen). Zero station fields are omitted.
static void fmt_route(const HistEntry *e, char *out, size_t n, int with_line)
{
    char a[64] = "", b[64] = "";
    int has_a = (e->entry_line | e->entry_sta) != 0;
    int has_b = (e->exit_line | e->exit_sta) != 0;
    if (has_a) fmt_station(a, sizeof(a), e->entry_line, e->entry_sta, e->region);
    if (has_b) fmt_station(b, sizeof(b), e->exit_line, e->exit_sta, e->region);
    if (is_train_proc(e->proc)) {
        if (has_a && has_b) {
            const char *ln, *sn;
            if (with_line &&
                station_resolve(e->entry_line, e->entry_sta, e->region, &ln, &sn))
                snprintf(out, n, "%s %s → %s", ln, a, b);
            else
                snprintf(out, n, "%s → %s", a, b);
        } else if (has_a) {
            snprintf(out, n, "%s", a);
        } else if (has_b) {
            snprintf(out, n, "%s", b);
        } else {
            out[0] = 0;
        }
    } else if (e->proc == 0x02 || e->proc == 0x03) {
        if (has_a) snprintf(out, n, "%s にて", a);
        else out[0] = 0;
    } else {
        out[0] = 0;
    }
}

static void build_route(HistEntry *e)
{
    fmt_route(e, e->route, sizeof(e->route), 1);
}

// Read the card that is currently InRange. Returns 1 on success.
static int read_card(void)
{
    svcSleepThread(200000000LL);

    NFC_TagInfo ti;
    memset(&ti, 0, sizeof(ti));
    if (R_FAILED(nfcGetTagInfo(&ti))) return 0;
    const u8 *idm = ti.id;
    snprintf(card_idm, sizeof(card_idm), "%02X%02X%02X%02X%02X%02X%02X%02X",
             idm[0], idm[1], idm[2], idm[3], idm[4], idm[5], idm[6], idm[7]);

    static u8 attr[1][16];
    if (read_blocks(idm, 0x008B, 0, 1, attr) < 1) return 0;
    card_balance = attr[0][11] | (attr[0][12] << 8);

    static u8 raw[20][16];
    int nhist = 0;
    int n1 = read_blocks(idm, 0x090F, 0, 10, raw);
    if (n1 > 0) {
        nhist = n1;
        int n2 = read_blocks(idm, 0x090F, 10, 10, raw + 10);
        if (n2 > 0) nhist = 10 + n2;
    }
    set_history(raw, nhist);
    return 1;
}

// Decode raw history blocks into the view model (shared with the demo build).
static void set_history(u8 raw[][16], int nhist)
{
    hist_count = 0;
    for (int i = 0; i < nhist && hist_count < 20; i++) {
        if (block_is_empty(raw[i])) continue;
        const u8 *b = raw[i];
        unsigned date = (b[4] << 8) | b[5];
        HistEntry *e = &hist[hist_count];
        e->year = 2000 + ((date >> 9) & 0x7F);
        e->mon  = (date >> 5) & 0x0F;
        e->day  = date & 0x1F;
        if (e->mon < 1 || e->mon > 12) continue;
        e->term = b[0];
        e->proc = b[1];
        e->entry_line = b[6];  e->entry_sta = b[7];
        e->exit_line  = b[8];  e->exit_sta  = b[9];
        e->balance = b[10] | (b[11] << 8);
        e->region = b[15];
        e->has_amount = 0;
        e->amount = 0;
        hist_count++;
    }
    for (int i = 0; i < hist_count; i++) {
        if (i + 1 < hist_count) {
            hist[i].amount = hist[i].balance - hist[i + 1].balance;
            hist[i].has_amount = 1;
        }
        build_route(&hist[i]);
    }
}

// ---------------------------------------------------------------------------
// Drawing: shared header
// ---------------------------------------------------------------------------
static void draw_header(const char *status)
{
    C2D_DrawRectSolid(0, 0, 0.0f, 400, 38, COL_HEADER);
    draw_text("IC残高ビューア", 12, 7, 0.72f, COL_WHITE);
    draw_text_r(status, 388, 11, 0.45f, COL_CARDLN);
}

// One compact history row for the top screen (single line).
static void draw_mini_row(const HistEntry *e, float y)
{
    rrect(14, y, 372, 22, 5, COL_WHITE);
    char dbuf[16];
    snprintf(dbuf, sizeof(dbuf), "%02d/%02d", e->mon, e->day);
    draw_text(dbuf, 22, y + 3, 0.48f, COL_INK);
    // type chip
    const char *lab = proc_label_jp(e->proc);
    float cw = text_w(lab, 0.42f) + 12;
    rrect(66, y + 3, cw, 16, 4, proc_color(e->proc));
    draw_text_c(lab, 66 + cw / 2, y + 4, 0.42f, COL_WHITE);
    // route (station names, no line prefix on the compact top screen)
    char rbuf[96];
    fmt_route(e, rbuf, sizeof(rbuf), 0);
    draw_text_fit(rbuf, 66 + cw + 8, y + 3, 0.48f, 190, COL_INK);
    if (e->has_amount) {
        char abuf[24], ybuf[28];
        fmt_yen(abuf, sizeof(abuf), e->amount);
        snprintf(ybuf, sizeof(ybuf), "%s%s", e->amount > 0 ? "+" : "", abuf);
        draw_text_r(ybuf, 330, y + 3, 0.5f,
                    e->amount < 0 ? COL_NEG : (e->amount > 0 ? COL_POS : COL_GREY));
    }
    char bbuf[32], bb2[40];
    fmt_yen(bbuf, sizeof(bbuf), e->balance);
    snprintf(bb2, sizeof(bb2), "¥%s", bbuf);
    draw_text_r(bb2, 380, y + 4, 0.45f, COL_GREY);
}

// ---------------------------------------------------------------------------
// Drawing: screens
// ---------------------------------------------------------------------------
static void draw_top_wait(int frame)
{
    C2D_SceneBegin(scr_top);
    C2D_TargetClear(scr_top, COL_BG);
    draw_header("待機中");
    // card outline with NFC waves
    float cx = 200, cy = 108;
    rrect(cx - 62, cy - 40, 124, 80, 10, COL_CARD);
    rrect(cx - 50, cy - 12, 26, 18, 3, COL_CHIP);
    draw_text_c("IC", cx, cy - 34, 0.6f, COL_WHITE);
    for (int i = 2; i >= 0; i--) {
        int ph = (frame * 2 + i * 20) % 60;
        float r = 8 + ph * 0.55f;
        u8 alpha = (u8)(110 - ph);
        C2D_DrawCircleSolid(cx + 52, cy - 18, 0.0f, r, C2D_Color32(104, 168, 48, alpha));
    }
    C2D_DrawCircleSolid(cx + 52, cy - 18, 0.0f, 4, COL_CARD_D);
    draw_text_c("カードを下画面にかざしてください", 200, 168, 0.66f, COL_INK);
    draw_text_c("交通系ICカード（Suica / Kitaca / PASMO など）", 200, 196, 0.44f, COL_GREY);
    draw_text_c("START：終了", 200, 218, 0.42f, COL_GREY);
}

static void draw_bot_wait(void)
{
    C2D_SceneBegin(scr_bot);
    C2D_TargetClear(scr_bot, COL_BG_BOT);
    draw_text("使い方", 14, 14, 0.6f, COL_INK);
    draw_text("New 3DS の NFC で交通系ICの", 14, 52, 0.5f, COL_INK);
    draw_text("残高と利用履歴を表示します。", 14, 74, 0.5f, COL_INK);
    draw_text("読み取り専用です。決済はしません。", 14, 106, 0.5f, COL_INK);
    char buf[64];
    if (db_count > 0) {
        char nb[24];
        fmt_yen(nb, sizeof(nb), db_count);
        snprintf(buf, sizeof(buf), "駅名DB：%s 駅を読み込み済み", nb);
    } else {
        snprintf(buf, sizeof(buf), "駅名DB：未読み込み（コード表示）");
    }
    draw_text(buf, 14, 150, 0.46f, COL_GREY);
    draw_text("DB差し替え：sdmc:/ic-balance/stations.bin", 14, 172, 0.4f, COL_GREY);
}

static void draw_top_card(void)
{
    C2D_SceneBegin(scr_top);
    C2D_TargetClear(scr_top, COL_BG);
    draw_header("読取完了");
    // card graphic
    rrect(16, 52, 180, 92, 10, COL_CARD);
    draw_text("交通系IC", 28, 58, 0.5f, COL_WHITE);
    draw_text_r("IC", 184, 55, 0.62f, COL_WHITE);
    rrect(30, 90, 30, 22, 3, COL_CHIP);
    char idb[40];
    snprintf(idb, sizeof(idb), "IDm %s", card_idm);
    draw_text(idb, 28, 120, 0.4f, COL_CARDLN);
    // balance block
    draw_text("残高", 214, 56, 0.55f, COL_GREY);
    char yb[32], yb2[40];
    fmt_yen(yb, sizeof(yb), card_balance);
    snprintf(yb2, sizeof(yb2), "¥%s", yb);
    draw_text_r(yb2, 388, 76, 1.35f, COL_INK);
    char hb[48];
    snprintf(hb, sizeof(hb), "履歴 %d 件", hist_count);
    draw_text_r(hb, 388, 122, 0.45f, COL_GREY);
    // divider + latest entries
    C2D_DrawRectSolid(14, 152, 0.0f, 372, 1, COL_LINE);
    draw_text("最近の履歴", 14, 158, 0.5f, COL_GREY);
    if (hist_count > 0) draw_mini_row(&hist[0], 182);
    if (hist_count > 1) draw_mini_row(&hist[1], 208);
    if (hist_count == 0) draw_text("履歴はありません", 14, 186, 0.5f, COL_GREY);
}

#define ROW_H 34.0f
#define ROWS_VISIBLE 5

static void draw_bot_history(int scroll)
{
    C2D_SceneBegin(scr_bot);
    C2D_TargetClear(scr_bot, COL_BG_BOT);
    C2D_DrawRectSolid(0, 0, 0.0f, 320, 30, COL_HEADER);
    draw_text("利用履歴（新しい順）", 10, 6, 0.55f, COL_WHITE);
    draw_text_r("↑↓ スクロール", 310, 8, 0.42f, COL_CARDLN);
    float y = 34;
    for (int i = scroll; i < hist_count && i < scroll + ROWS_VISIBLE; i++, y += ROW_H) {
        const HistEntry *e = &hist[i];
        if (i > scroll) C2D_DrawRectSolid(10, y - 3, 0.0f, 300, 1, COL_LINE);
        char dbuf[24];
        snprintf(dbuf, sizeof(dbuf), "%02d/%02d", e->mon, e->day);
        draw_text(dbuf, 10, y, 0.5f, COL_INK);
        const char *lab = proc_label_jp(e->proc);
        float cw = text_w(lab, 0.42f) + 12;
        rrect(56, y, cw, 17, 4, proc_color(e->proc));
        draw_text_c(lab, 56 + cw / 2, y + 1, 0.42f, COL_WHITE);
        if (e->has_amount) {
            char abuf[24], ybuf[28];
            fmt_yen(abuf, sizeof(abuf), e->amount);
            snprintf(ybuf, sizeof(ybuf), "%s%s", e->amount > 0 ? "+" : "", abuf);
            draw_text_r(ybuf, 312, y - 1, 0.56f,
                        e->amount < 0 ? COL_NEG : (e->amount > 0 ? COL_POS : COL_GREY));
        }
        if (e->route[0])
            draw_text_fit(e->route, 56, y + 18, 0.5f, 254, COL_INK);
        char bbuf[32], bb2[44];
        fmt_yen(bbuf, sizeof(bbuf), e->balance);
        snprintf(bb2, sizeof(bb2), "残 ¥%s", bbuf);
        draw_text_r(bb2, 312, y + 18, 0.44f, COL_GREY);
    }
    if (hist_count > ROWS_VISIBLE) {
        // scrollbar
        float track_y = 34, track_h = 240 - 38;
        float thumb_h = track_h * ROWS_VISIBLE / hist_count;
        float maxoff = hist_count - ROWS_VISIBLE;
        float thumb_y = track_y + (track_h - thumb_h) * (maxoff ? scroll / maxoff : 0);
        C2D_DrawRectSolid(315, track_y, 0.0f, 3, track_h, COL_LINE);
        C2D_DrawRectSolid(315, thumb_y, 0.0f, 3, thumb_h, COL_GREY);
    }
    draw_text("別のカードをかざすと切替", 10, 222, 0.38f, COL_GREY);
    draw_text_r("B：もどる／START：終了", 312, 222, 0.38f, COL_GREY);
}

static void draw_top_error(void)
{
    C2D_SceneBegin(scr_top);
    C2D_TargetClear(scr_top, COL_BG);
    draw_header("読取失敗");
    draw_text_c("読み取りに失敗しました", 200, 92, 0.7f, COL_NEG);
    draw_text_c("カードを下画面に平らに当てて", 200, 132, 0.5f, COL_INK);
    draw_text_c("動かさずに置いてください", 200, 156, 0.5f, COL_INK);
    draw_text_c("別のカードをかざすと再読み取り ／ B：もどる", 200, 196, 0.44f, COL_GREY);
}

static void draw_bot_blank(const char *msg)
{
    C2D_SceneBegin(scr_bot);
    C2D_TargetClear(scr_bot, COL_BG_BOT);
    if (msg) draw_text_c(msg, 160, 110, 0.5f, COL_GREY);
}

// ---------------------------------------------------------------------------
// Tag state polling helpers
// ---------------------------------------------------------------------------
static int tag_in_range(void)
{
    NFC_TagState st;
    return R_SUCCEEDED(nfcGetTagState(&st)) && st == NFC_TagState_InRange;
}
static int tag_out_of_range(void)
{
    NFC_TagState st;
    return R_SUCCEEDED(nfcGetTagState(&st)) && st == NFC_TagState_OutOfRange;
}

// Snapshot of the view model, so a failed card-swap read during the show
// phase can restore the previous card's data instead of corrupting it.
static HistEntry hist_snap[20];
static int hist_count_snap = 0;
static int card_balance_snap = 0;
static char card_idm_snap[17] = "";
static void view_snapshot(void)
{
    memcpy(hist_snap, hist, sizeof(hist));
    hist_count_snap = hist_count;
    card_balance_snap = card_balance;
    snprintf(card_idm_snap, sizeof(card_idm_snap), "%s", card_idm);
}
static void view_restore(void)
{
    memcpy(hist, hist_snap, sizeof(hist));
    hist_count = hist_count_snap;
    card_balance = card_balance_snap;
    snprintf(card_idm, sizeof(card_idm), "%s", card_idm_snap);
}

// Stop the current scan session and start a fresh one. Acquiring a new
// tag reliably needs this cycle on hardware.
static int scan_restart(void)
{
    nfcStopScanning();
    svcSleepThread(100000000LL);
    return R_SUCCEEDED(nfcStartOtherTagScanning(NFC_STARTSCAN_DEFAULTINPUT, FELICA_MASK));
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    scr_top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    scr_bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    tbuf = C2D_TextBufNew(32768);

    romfsInit();
    db_count = station_db_load();

#ifdef DEMO_DATA
    // Screenshot/demo build (Azahar): sample history, no NFC involved.
    {
        static u8 demo_raw[10][16] = {
            {0x16,0x01,0x00,0x02,0x35,0x68,0x0D,0x53,0x0D,0x4E,0xFA,0x01,0x00,0x02,0xF8,0x00},
            {0x16,0x01,0x00,0x02,0x35,0x68,0x0D,0x4E,0x0D,0x53,0x30,0x03,0x00,0x02,0xF6,0x00},
            {0x15,0x02,0x00,0x00,0x35,0x68,0x0D,0x4E,0x00,0x00,0x66,0x04,0x00,0x02,0xF4,0x00},
            {0x05,0x0F,0x00,0x0F,0x35,0x54,0x09,0x05,0x00,0x00,0x7E,0x00,0x00,0x02,0xF3,0xC0},
            {0x05,0x0F,0x00,0x0F,0x35,0x50,0x09,0x05,0x00,0x00,0x78,0x01,0x00,0x02,0xF2,0xC0},
            {0x05,0x0F,0x00,0x0F,0x35,0x4D,0x09,0x05,0x00,0x00,0x72,0x02,0x00,0x02,0xF1,0xC0},
            {0x16,0x01,0x00,0x02,0x35,0x47,0x0D,0x52,0x0D,0x4E,0x6C,0x03,0x00,0x02,0xF0,0x00},
            {0x15,0x02,0x00,0x00,0x35,0x47,0x0D,0x52,0x00,0x00,0x7A,0x04,0x00,0x02,0xEE,0x00},
            {0x05,0x0F,0x00,0x0F,0x35,0x46,0x09,0x05,0x00,0x00,0x92,0x00,0x00,0x02,0xED,0xC0},
            {0x05,0x0F,0x00,0x0F,0x35,0x41,0x09,0x05,0x00,0x00,0x8C,0x01,0x00,0x02,0xEC,0xC0},
        };
        card_balance = 506;
        snprintf(card_idm, sizeof(card_idm), "011401145716DB06");
        set_history(demo_raw, 10);
        int scroll = 0, repeat = 0;
        while (aptMainLoop()) {
            hidScanInput();
            u32 down = hidKeysDown();
            u32 held = hidKeysHeld();
            if (down & KEY_START) break;
            int maxscroll = hist_count > ROWS_VISIBLE ? hist_count - ROWS_VISIBLE : 0;
            if (down & KEY_DDOWN) { if (scroll < maxscroll) scroll++; repeat = 0; }
            if (down & KEY_DUP)   { if (scroll > 0) scroll--; repeat = 0; }
            if ((held & KEY_DDOWN) || (held & KEY_DUP)) {
                if (++repeat > 25 && (repeat % 4) == 0) {
                    if ((held & KEY_DDOWN) && scroll < maxscroll) scroll++;
                    if ((held & KEY_DUP) && scroll > 0) scroll--;
                }
            } else repeat = 0;
            C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
            C2D_TextBufClear(tbuf);
            draw_top_card();
            draw_bot_history(scroll);
            C3D_FrameEnd(0);
        }
        goto done;
    }
#endif

    Result ret = nfcInit(NFC_OpType_RawNFC);
    if (R_FAILED(ret)) {
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_START) break;
            C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
            C2D_TextBufClear(tbuf);
            C2D_SceneBegin(scr_top);
            C2D_TargetClear(scr_top, COL_BG);
            draw_text_c("NFCの初期化に失敗しました", 200, 90, 0.6f, COL_NEG);
            draw_text_c("New 3DS（システム10.0以上）が必要です", 200, 124, 0.45f, COL_INK);
            draw_text_c("START：終了", 200, 170, 0.45f, COL_GREY);
            C3D_FrameEnd(0);
        }
        goto done;
    }

    {
        int frame = 0;
        int running = 1;
        while (running) {
            // ---- scan phase ----
            ret = nfcStartOtherTagScanning(NFC_STARTSCAN_DEFAULTINPUT, FELICA_MASK);
            if (R_FAILED(ret)) break;
            int got = 0, quit = 0;
            while (aptMainLoop()) {
                hidScanInput();
                if (hidKeysDown() & KEY_START) { quit = 1; break; }
                C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
                C2D_TextBufClear(tbuf);
                draw_top_wait(frame);
                draw_bot_wait();
                C3D_FrameEnd(0);
                frame++;
                if ((frame % 4) == 0 && tag_in_range()) { got = 1; break; }
            }
            if (quit || !aptMainLoop()) { nfcStopScanning(); running = 0; break; }
            if (!got) { nfcStopScanning(); continue; }

            // ---- read phase ----
            int ok = read_card();

            // ---- show phase ----
            // The result stays on screen until B (back to scanning) or
            // START (quit); removing the card no longer leaves this phase.
            // Card swap: once the shown card leaves, the scan session is
            // restarted (stop + start) while the old data stays on
            // screen. A fresh session is what reliably acquires a new
            // tag on hardware — the same stop/start cycle the scan phase
            // uses; re-detecting within the old session did not work
            // (v4 hardware finding). When the new tag is detected it is
            // read and the display switches; a failed read keeps the
            // previous display and keeps watching.
            int scroll = 0, repeat = 0;
            int watching = 0;     // fresh scan session running, awaiting a tag
            int restart_cool = 0; // frames to wait before retrying a failed restart
            while (aptMainLoop()) {
                hidScanInput();
                u32 down = hidKeysDown();
                u32 held = hidKeysHeld();
                if (down & KEY_START) { quit = 1; break; }
                if (down & KEY_B) break;
                int maxscroll = hist_count > ROWS_VISIBLE ? hist_count - ROWS_VISIBLE : 0;
                if (down & KEY_DDOWN) { if (scroll < maxscroll) scroll++; repeat = 0; }
                if (down & KEY_DUP)   { if (scroll > 0) scroll--; repeat = 0; }
                if ((held & KEY_DDOWN) || (held & KEY_DUP)) {
                    if (++repeat > 25 && (repeat % 4) == 0) {
                        if ((held & KEY_DDOWN) && scroll < maxscroll) scroll++;
                        if ((held & KEY_DUP) && scroll > 0) scroll--;
                    }
                } else repeat = 0;

                C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
                C2D_TextBufClear(tbuf);
                if (ok) {
                    draw_top_card();
                    draw_bot_history(scroll);
                } else {
                    draw_top_error();
                    draw_bot_blank("読み取りに失敗しました");
                }
                C3D_FrameEnd(0);
                frame++;
                if (restart_cool > 0) restart_cool--;
                if ((frame % 4) == 0) {
                    if (!watching) {
                        if (restart_cool == 0 && tag_out_of_range()) {
                            if (scan_restart()) watching = 1;
                            else restart_cool = 90;
                        }
                    } else if (tag_in_range()) {
                        view_snapshot();
                        if (read_card()) {
                            ok = 1; scroll = 0; repeat = 0;
                            watching = 0;
                        } else {
                            view_restore();
                            if (scan_restart()) watching = 1;
                            else { watching = 0; restart_cool = 90; }
                        }
                    }
                }
            }
            nfcStopScanning();
            if (quit) running = 0;
            svcSleepThread(300000000LL);
        }
    }

    nfcExit();
done:
    C2D_TextBufDelete(tbuf);
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    return 0;
}
