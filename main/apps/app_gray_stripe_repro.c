/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：16 灰阶竖条纹复现页。灰区与底部黑白内容在同一次 GC16 刷新里出图时，
 * 原生扫描行约第 493 行之后（竖屏 x < 约 190）的灰阶会变浅，形成竖条纹。
 * 六张图依次复现：现象、位置无关对照、固定分界、黑像素门槛、不驱动也触发、分刷对照。
 * 每张刷完再用 DU 单独补一条顶部标尺（分刷不会引起条纹），照片上可直接读 x。
 *
 * English: Repro page for the 16-gray vertical stripe. When gray and footer
 * black/white content are presented in one GC16 refresh, gray on native scan
 * rows after about 493 (portrait x < ~190) turns lighter. Six views: symptom,
 * position control, fixed boundary, black-pixel threshold, undriven trigger and
 * split-refresh control. A ruler is added by a separate DU update after each
 * view (split updates do not stripe) so x can be read off a photo.
 *
 * 冻结：只用默认波形与现有刷新出口，不改 VCOM、扫描时序或波形表头文件；第 5 张的测试表
 * 只存在于本页内存，刷完立即换回默认表。
 * Frozen: default waveform and existing present paths only; no VCOM, scan
 * timing or waveform table edits. View 5's test table lives in this page's RAM
 * only and the default table is restored right after.
 *
 * ---- 加入仓库 / Adding to the tree ----
 * 1. 复制到 main/apps/app_gray_stripe_repro.c
 *    Copy to main/apps/app_gray_stripe_repro.c
 * 2. main/CMakeLists.txt 的 SRCS 里，在 "apps/app_refresh.c" 下面加一行：
 *    In main/CMakeLists.txt SRCS, below "apps/app_refresh.c", add:
 *        "apps/app_gray_stripe_repro.c"
 * 3. main/app/app_registry.c 加声明与表项（放在 app_refresh 后面即可）：
 *    In main/app/app_registry.c add the declaration and table entry (after app_refresh):
 *        extern const app_desc_t app_gray_stripe_repro;
 *        &app_gray_stripe_repro,
 * 4. idf.py build flash，菜单进入「灰阶条纹复现」，点屏幕或 KEY1 翻页。
 *    idf.py build flash; open "灰阶条纹复现 Gray Stripe Repro" from the menu; tap or KEY1 for the next view.
 *
 * ---- 预期 / Expected ----
 * 坐标都是竖屏 x（标尺读数）。原生扫描行 = 683 − x。
 * All coordinates are portrait x (ruler). Native scan row = 683 − x.
 * 1 基准：黑块 x160..320，灰区只在 x160..190 变浅。/ Baseline: lighter gray only at x160..190.
 * 2 右移：黑块 x240..400 全在 x>190，无条纹。/ Shifted: block entirely at x>190, no stripe.
 * 3 宽块：黑块 x60..420，分界固定在 x≈190；x<70 变浅较弱。/ Wide: boundary at x≈190; weaker at x<70.
 * 4 黑量：每行 64/96/128/160 个黑像素，96 无、128 起满幅。/ Count: 96 none, full from 128.
 * 5 不驱动：黑块全程不驱动（保持白），仍变浅约六成。/ Undriven: block never driven, still ~60% of the effect.
 * 6 分刷：同第 1 张，但先刷黑块、再单独刷灰区，无条纹。/ Split: same as 1 in two updates, no stripe.
 */

#include <stdio.h>
#include <string.h>

#include "app.h"
#include "display.h"
#include "e0470_epaper_waveform.h"
#include "esp_log.h"
#include "ui_kit.h"
#include "ui_menu.h"

#define TAG "gray_stripe"

// 竖屏 x 对应原生扫描行 683−x。灰区 x 40..440；标签与菜单把手在 x≥470，不与灰区共用扫描行。
// 灰区 y 96..800 对齐原生列 32 像素，第 6 张的局部更新不会外扩到其它内容。
// Portrait x maps to native scan row 683−x. Gray spans x 40..440; label and
// menu handle stay at x≥470 so they share no scan row with the gray. Gray y
// 96..800 is aligned to 32 native columns so view 6's area update does not grow.
static const EpdRect k_gray = { .x = 40, .y = 96, .width = 400, .height = 704 };
static const EpdRect k_ruler = { .x = 0, .y = 32, .width = 684, .height = 64 };
#define GRAY_MID (7 * 17)
#define LABEL_X 470
#define LABEL_Y 24
#define FOOT_Y 880
#define FOOT_H 208

static void foot_block(uint8_t* fb, int x0, int x1, int h) {
    epd_fill_rect((EpdRect){ .x = x0, .y = FOOT_Y, .width = x1 - x0, .height = h }, UI_GRAY_BLACK, fb);
}

static void draw_baseline(uint8_t* fb) { foot_block(fb, 160, 320, FOOT_H); }
static void draw_shifted(uint8_t* fb) { foot_block(fb, 240, 400, FOOT_H); }
static void draw_wide(uint8_t* fb) { foot_block(fb, 60, 420, FOOT_H); }
static void draw_undriven(uint8_t* fb) { foot_block(fb, 40, 185, FOOT_H); }

// 敏感区内四段，每行黑像素 64 / 96 / 128 / 160，段间 5px 白缝。/ Four segments in the sensitive zone, 64 / 96 / 128 / 160 black px per row, 5 px gaps.
static void draw_counts(uint8_t* fb) {
    static const int heights[4] = { 64, 96, 128, 160 };
    for (int i = 0; i < 4; i++) foot_block(fb, 40 + i * 37, 72 + i * 37, heights[i]);
}

typedef enum {
    PRESENT_SAME,      ///< 灰区与底部一次刷出 / Gray and footer in one refresh
    PRESENT_UNDRIVEN,  ///< 同上，但 15→0 全程不驱动 / Same, but 15→0 is never driven
    PRESENT_SPLIT,     ///< 先底部、后灰区两次刷 / Footer first, then gray alone
} present_kind_t;

typedef struct {
    const char* label;
    const char* detail;
    void (*draw)(uint8_t* fb);
    present_kind_t kind;
} repro_view_t;

static const repro_view_t k_views[] = {
    { "1/6 基准", "黑块 x160..320 同刷 Baseline", draw_baseline, PRESENT_SAME },
    { "2/6 右移", "黑块 x240..400 无条纹 Shifted", draw_shifted, PRESENT_SAME },
    { "3/6 宽块", "黑块 x60..420 分界 x≈190 Wide", draw_wide, PRESENT_SAME },
    { "4/6 黑量", "每行 64/96/128/160 Count", draw_counts, PRESENT_SAME },
    { "5/6 不驱动", "黑块全程不驱动 Undriven", draw_undriven, PRESENT_UNDRIVEN },
    { "6/6 分刷", "同 1，先黑块后灰区 Split", draw_baseline, PRESENT_SPLIT },
};
#define VIEW_COUNT ((int)(sizeof(k_views) / sizeof(k_views[0])))

static int s_view = -1;  // -1 = 说明页 / intro page

/* ---- 测试表 / Test table ---- */
// 默认 GC16 的 RAM 副本，只把 (from 15 → to 0) 清成全程保持：data[frame][to][from/4]，from 15 在最低两位。
// RAM copy of the default GC16 with only (from 15 → to 0) cleared to hold: data[frame][to][from/4]; from 15 is the lowest two bits.
static uint8_t s_lut[E0470_GC16_FRAMES][16][4];
static EpdWaveformPhases s_phases = { .phases = E0470_GC16_FRAMES, .luts = &s_lut[0][0][0] };
static const EpdWaveformPhases* s_ranges[] = { &s_phases };
static const EpdWaveformMode s_mode = { .type = 2, .temp_ranges = 1, .range_data = s_ranges };
static const EpdWaveformMode* s_modes[] = { &s_mode };
static const EpdWaveformTempInterval s_intervals[] = { { .min = 0, .max = 50 } };
static const EpdWaveform s_undriven_wave = {
    .num_modes = 1, .num_temp_ranges = 1, .mode_data = s_modes, .temp_intervals = s_intervals,
};

static void build_undriven_lut(void) {
    const EpdWaveformPhases* base = e0470_waveform_phases(&E0470_WAVEFORM, MODE_GC16);
    memcpy(s_lut, base->luts, sizeof(s_lut));
    for (int f = 0; f < E0470_GC16_FRAMES; f++) s_lut[f][0][3] &= (uint8_t)~3u;
}

/* ---- 绘制 / Drawing ---- */
static void draw_intro(uint8_t* fb) {
    ui_clear_page(fb);
    int y = ui_draw_header(fb, "灰阶条纹复现 Gray Stripe Repro", "灰区与底部同刷 Same Refresh");
    y = ui_draw_section(fb, y, "六张对照 Six Views");
    for (int i = 0; i < VIEW_COUNT; i++) y = ui_draw_row(fb, y, k_views[i].label, k_views[i].detail);
    y += 24;
    ui_text(
        fb, UI_MARGIN, y, UI_PX_CAPTION, "点按屏幕或 KEY1 下一张 Tap/KEY1 Next",
        EPD_DRAW_ALIGN_LEFT, false
    );
    ui_draw_menu_handle(fb, false);
}

// x 40..440 每 10px 短刻度，每 50px 长刻度加数字。/ Short tick every 10 px over x 40..440, long tick and number every 50 px.
static void draw_ruler(uint8_t* fb) {
    char num[4];
    for (int x = 40; x <= 440; x += 10) {
        const bool major = x % 50 == 0;
        const int h = major ? 22 : 10;
        epd_fill_rect((EpdRect){ .x = x, .y = 94 - h, .width = 1, .height = h }, UI_GRAY_BLACK, fb);
        if (major) {
            snprintf(num, sizeof(num), "%d", x);
            ui_text(fb, x, 36, 20, num, EPD_DRAW_ALIGN_CENTER, false);
        }
    }
}

// 每张：GC16 铺白，按 kind 出图，再 DU 单独补标尺。
// Each view: GC16 to white, present by kind, then add the ruler with a separate DU update.
static enum EpdDrawError show_view(app_ctx_t* ctx, int view) {
    const repro_view_t* v = &k_views[view];
    display_hold_white_exit(false);
    enum EpdDrawError err = update_display_white(ctx->hl);
    if (err != EPD_DRAW_SUCCESS) return err;
    display_hold_white_exit(true);

    ui_clear_page(ctx->fb);
    ui_text(ctx->fb, LABEL_X, LABEL_Y, UI_PX_CAPTION, v->label, EPD_DRAW_ALIGN_LEFT, false);
    v->draw(ctx->fb);
    ui_draw_menu_handle(ctx->fb, false);

    switch (v->kind) {
        case PRESENT_SAME:
            epd_fill_rect(k_gray, GRAY_MID, ctx->fb);
            err = update_display_from_white(ctx->hl);
            break;
        case PRESENT_UNDRIVEN:
            epd_fill_rect(k_gray, GRAY_MID, ctx->fb);
            build_undriven_lut();
            err = update_display_from_white_with(ctx->hl, &s_undriven_wave, MODE_GC16);
            break;
        case PRESENT_SPLIT:
            err = update_display_from_white(ctx->hl);
            if (err != EPD_DRAW_SUCCESS) return err;
            epd_fill_rect(k_gray, GRAY_MID, ctx->fb);
            err = update_display_area_with(ctx->hl, &E0470_WAVEFORM, MODE_GC16, k_gray);
            break;
    }
    if (err != EPD_DRAW_SUCCESS) return err;

    draw_ruler(ctx->fb);
    return update_display_area_with(ctx->hl, &E0470_WAVEFORM, MODE_DU, k_ruler);
}

static app_redraw_t next_view(app_ctx_t* ctx) {
    s_view = (s_view + 1) % VIEW_COUNT;
    enum EpdDrawError err = show_view(ctx, s_view);
    ESP_LOGI(TAG, "view %d/%d result 0x%x", s_view + 1, VIEW_COUNT, (unsigned)err);
    guard_draw_result(ctx->hl, err);
    return APP_REDRAW_DONE;
}

/* ---- 页面回调 / Page callbacks ---- */
static void repro_render(app_ctx_t* ctx, uint8_t* fb) {
    (void)ctx;
    s_view = -1;
    draw_intro(fb);
}

static bool repro_present(app_ctx_t* ctx, app_redraw_t redraw) {
    if (redraw == APP_REDRAW_DONE) return true;
    if (redraw != APP_REDRAW_PAGE && redraw != APP_REDRAW_FULL) return false;
    if (!display_take_white_exit()) return false;
    // 灰图还在屏上：先铺白，再从白底出说明页。/ Gray still on panel: wipe white, then present the intro from white.
    enum EpdDrawError err = update_display_white(ctx->hl);
    if (err == EPD_DRAW_SUCCESS) {
        draw_intro(ctx->fb);
        err = update_display_from_white(ctx->hl);
    }
    guard_draw_result(ctx->hl, err);
    return true;
}

static app_redraw_t repro_touch(app_ctx_t* ctx, const cst836u_touch_t* touch) {
    (void)touch;
    return next_view(ctx);
}

static app_redraw_t repro_key(app_ctx_t* ctx, int key) {
    (void)key;
    return next_view(ctx);
}

static void repro_on_exit(app_ctx_t* ctx) {
    s_view = -1;
    if (display_take_white_exit()) guard_draw_result(ctx->hl, update_display_white(ctx->hl));
}

const app_desc_t app_gray_stripe_repro = {
    .title = "灰阶条纹复现 Gray Stripe Repro",
    .detail = "灰区与底部同刷 Gray + Footer",
    .enter_full = true,
    .render = repro_render,
    .present = repro_present,
    .on_touch = repro_touch,
    .on_key = repro_key,
    .on_exit = repro_on_exit,
};
