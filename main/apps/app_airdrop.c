/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 隔空投送页：进页启动 AirDrop 接收，把收到的图书、图片和其它文件存到 TF 卡，
 * 传完后自动打开图书或图片。协议与无线由 esp32drop 组件负责。
 *
 * AirDrop page: starts the AirDrop receiver on entry, saves received books, images and
 * other files to the TF card and opens the book or image once the transfer ends. The
 * esp32drop component owns the protocol and the radio.
 *
 * 冻结：用户要求只存 TF 卡，不回退内置存储；离页停止 AirDrop 并释放无线，与传书页互斥。
 * 同名不覆盖，另起编号文件名；图书经 book_store 请求交给图书页打开，本页不解析图书。
 * 沿用全局锁屏/睡眠策略；卡失效时停止接收，重新进页再启动。render 只画快照。
 * Frozen: per the user, save only to the TF card with no flash fallback; leaving stops
 * AirDrop and frees the radio, exclusive with the transfer page. Never overwrite a file;
 * use a numbered name. Books are handed to the book page through a book_store request;
 * this page never parses books. Keep the global lock/sleep policy; media loss stops
 * reception until the page is entered again. Render only paints snapshots.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app.h"
#include "app_registry.h"
#include "book_image.h"
#include "book_store.h"
#include "display.h"
#include "e0470_epaper_waveform.h"
#include "esp32drop.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "read_pico_sd.h"
#include "ttf_font.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"

#define AIRDROP_NAME "小纸 Pico"
#define AIRDROP_BOOK_DIR "/sdcard/books"
#define AIRDROP_IMAGE_DIR "/sdcard/images"
#define AIRDROP_OTHER_DIR "/sdcard/airdrop"
#define AIRDROP_MAX_RECEIVE (6u * 1024u * 1024u)
#define AIRDROP_IMAGE_MAX_INPUT (8u * 1024u * 1024u)
#define AIRDROP_POLL_MS 1000

static const char* TAG = "airdrop_page";

typedef enum { AIRDROP_KIND_BOOK, AIRDROP_KIND_IMAGE, AIRDROP_KIND_OTHER } airdrop_kind_t;

static bool s_start_pending, s_running, s_media_lost, s_settle, s_image_open, s_saving;
static esp_err_t s_start_err;
static int64_t s_poll_ms;
static esp32drop_status_t s_status;
static unsigned s_saved, s_books_saved;
static char s_last_name[128], s_message[128];
// 本次传输里最后一本图书与最后一张图片，传完后据此自动打开。
// Last book and image of the current transfer, opened when it ends.
static char s_open_book[BOOK_STORE_PATH_MAX];
static uint8_t* s_image;
static uint16_t s_image_w, s_image_h;
static char s_image_name[128];
static int s_pressed = -1;
static EpdRect s_area;

static void copy_text(char* dst, size_t cap, const char* src) { snprintf(dst, cap, "%s", src ? src : ""); }

static void fit_text(char* text, int px, int width) {
    while (*text && ttf_text_width_px(px, text) > width) {
        size_t n = strlen(text) - 1;
        while (n && ((unsigned char)text[n] & 0xc0) == 0x80) --n;
        text[n] = 0;
    }
}

static void free_image(void) {
    free(s_image);
    s_image = NULL;
    s_image_w = s_image_h = 0;
    s_image_open = false;
}

/* ---- 文件保存 / Saving ---- */

static bool has_ext(const char* name, const char* ext) {
    size_t n = strlen(name), e = strlen(ext);
    return n > e && !strcasecmp(name + n - e, ext);
}

static airdrop_kind_t classify(const esp32drop_file_t* f) {
    if (has_ext(f->name, ".txt") || has_ext(f->name, ".epub")) return AIRDROP_KIND_BOOK;
    if (f->type == ESP32DROP_FILE_JPEG || f->type == ESP32DROP_FILE_PNG || f->type == ESP32DROP_FILE_GIF ||
        f->type == ESP32DROP_FILE_HEIC) return AIRDROP_KIND_IMAGE;
    return AIRDROP_KIND_OTHER;
}

// 去掉 FAT 不接受的字符和开头的点，保留 UTF-8 与扩展名。
// Drop characters FAT rejects and leading dots, keeping UTF-8 and the extension.
static void sanitize_name(const char* in, char* out, size_t cap) {
    size_t o = 0;
    for (const unsigned char* p = (const unsigned char*)in; *p && o + 1 < cap; ++p) {
        unsigned char c = *p;
        if (o == 0 && (c == '.' || c == ' ')) continue;
        if (c < 0x20 || strchr("/\\:*?\"<>|", c)) c = '_';
        out[o++] = (char)c;
    }
    while (o && (out[o - 1] == ' ' || out[o - 1] == '.')) --o;
    out[o] = 0;
    if (!o) copy_text(out, cap, "airdrop");
}

static bool path_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

// 同名已存在时追加 (2)、(3)…，绝不覆盖。/ Append (2), (3)… when the name exists; never overwrite.
static bool unique_path(const char* dir, const char* name, char* out, size_t cap) {
    if ((size_t)snprintf(out, cap, "%s/%s", dir, name) >= cap) return false;
    if (!path_exists(out)) return true;
    const char* dot = strrchr(name, '.');
    int base = dot && dot != name ? (int)(dot - name) : (int)strlen(name);
    const char* ext = dot && dot != name ? dot : "";
    for (int i = 2; i < 100; ++i) {
        if ((size_t)snprintf(out, cap, "%s/%.*s (%d)%s", dir, base, name, i, ext) >= cap) return false;
        if (!path_exists(out)) return true;
    }
    return false;
}

static esp_err_t ensure_dir(const char* dir) {
    struct stat st;
    if (stat(dir, &st) == 0) return S_ISDIR(st.st_mode) ? ESP_OK : ESP_ERR_INVALID_STATE;
    return mkdir(dir, 0775) == 0 || errno == EEXIST ? ESP_OK : ESP_FAIL;
}

// 先写临时文件再改名，中途失败不留下半个文件。/ Write a temp file then rename, so a failure leaves no partial file.
static esp_err_t write_file(const char* path, const uint8_t* data, uint32_t len) {
    char tmp[BOOK_STORE_PATH_MAX + 8];
    snprintf(tmp, sizeof(tmp), "%s.part", path);
    FILE* fp = fopen(tmp, "wb");
    if (!fp) return ESP_FAIL;
    uint32_t done = 0;
    while (done < len) {
        uint32_t n = len - done > 32768 ? 32768 : len - done;
        if (fwrite(data + done, 1, n, fp) != n) break;
        done += n;
    }
    bool ok = done == len;
    if (fclose(fp) != 0) ok = false;
    if (ok && rename(tmp, path) != 0) ok = false;
    if (!ok) unlink(tmp);
    return ok ? ESP_OK : ESP_FAIL;
}

// EXIF 方向（1–8），读不到返回 1。/ EXIF orientation 1–8, or 1 when absent.
static int jpeg_orientation(const uint8_t* d, uint32_t n) {
    if (n < 4 || d[0] != 0xff || d[1] != 0xd8) return 1;
    uint32_t at = 2;
    while (at + 4 <= n && d[at] == 0xff) {
        uint8_t marker = d[at + 1];
        uint32_t seg = (uint32_t)d[at + 2] << 8 | d[at + 3];
        if (marker == 0xda || seg < 2 || at + 2 + seg > n) break;
        const uint8_t* p = d + at + 4;
        uint32_t len = seg - 2;
        if (marker == 0xe1 && len >= 14 && !memcmp(p, "Exif\0\0", 6)) {
            const uint8_t* t = p + 6;
            uint32_t tl = len - 6;
            bool le = t[0] == 'I';
#define RD16(o) (le ? (uint32_t)t[o] | (uint32_t)t[(o) + 1] << 8 : (uint32_t)t[o] << 8 | t[(o) + 1])
#define RD32(o) (le ? RD16(o) | RD16((o) + 2) << 16 : RD16(o) << 16 | RD16((o) + 2))
            uint32_t ifd = RD32(4);
            if (ifd + 2 > tl) return 1;
            uint32_t count = RD16(ifd);
            for (uint32_t i = 0; i < count && ifd + 2 + i * 12 + 12 <= tl; ++i) {
                uint32_t e = ifd + 2 + i * 12;
                if (RD16(e) == 0x0112) {
                    uint32_t v = RD16(e + 8);
                    return v >= 1 && v <= 8 ? (int)v : 1;
                }
            }
#undef RD16
#undef RD32
            return 1;
        }
        at += 2 + seg;
    }
    return 1;
}

// 按 EXIF 方向转正灰度图，失败保留原图。/ Apply EXIF orientation to the grayscale image; keep the original on failure.
static void apply_orientation(int orient) {
    if (orient <= 1 || !s_image) return;
    unsigned w = s_image_w, h = s_image_h;
    bool swap = orient >= 5;
    unsigned dw = swap ? h : w, dh = swap ? w : h;
    uint8_t* out = heap_caps_malloc((size_t)dw * dh, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) return;
    for (unsigned y = 0; y < dh; ++y) for (unsigned x = 0; x < dw; ++x) {
        unsigned sx, sy;
        switch (orient) {
        case 2: sx = w - 1 - x; sy = y; break;
        case 3: sx = w - 1 - x; sy = h - 1 - y; break;
        case 4: sx = x; sy = h - 1 - y; break;
        case 5: sx = y; sy = x; break;
        case 6: sx = y; sy = h - 1 - x; break;
        case 7: sx = w - 1 - y; sy = h - 1 - x; break;
        default: sx = w - 1 - y; sy = x; break;
        }
        out[(size_t)y * dw + x] = s_image[(size_t)sy * w + sx];
    }
    free(s_image);
    s_image = out;
    s_image_w = (uint16_t)dw;
    s_image_h = (uint16_t)dh;
}

static EpdRect image_area(void) { return (EpdRect){0, 0, UI_LOCK_WIDTH, UI_BAR_TOP - 12}; }

static void decode_image(const esp32drop_file_t* f) {
    free_image();
    if (f->type != ESP32DROP_FILE_JPEG && f->type != ESP32DROP_FILE_PNG) return;
    EpdRect area = image_area();
    int orient = f->type == ESP32DROP_FILE_JPEG ? jpeg_orientation(f->data, f->len) : 1;
    unsigned bw = orient >= 5 ? area.height : area.width, bh = orient >= 5 ? area.width : area.height;
    if (!book_image_decode_fit(f->data, f->len, AIRDROP_IMAGE_MAX_INPUT, bw, bh, (size_t)bw * bh,
                               &s_image, &s_image_w, &s_image_h)) {
        free_image();
        return;
    }
    apply_orientation(orient);
    copy_text(s_image_name, sizeof(s_image_name), f->name);
}

// 在 esp32drop_poll() 内、本页任务上运行；f->data 只在回调期间有效。
// Runs inside esp32drop_poll() on this page's task; f->data is valid only during the call.
static bool on_file(void* ctx, const esp32drop_file_t* f) {
    (void)ctx;
    if (f->type == ESP32DROP_FILE_APPLEDOUBLE || !strncmp(f->name, "._", 2)) return true;
    airdrop_kind_t kind = classify(f);
    const char* dir = kind == AIRDROP_KIND_BOOK ? AIRDROP_BOOK_DIR :
                      kind == AIRDROP_KIND_IMAGE ? AIRDROP_IMAGE_DIR : AIRDROP_OTHER_DIR;
    char name[128], path[BOOK_STORE_PATH_MAX];
    sanitize_name(f->name, name, sizeof(name));
    book_store_root_t root = {.path = "/sdcard", .is_flash = false};
    if (book_store_free_bytes(&root) < (uint64_t)f->len + 256u * 1024u) {
        copy_text(s_message, sizeof(s_message), "TF 卡空间不足，文件未保存");
        return false;
    }
    if (ensure_dir(dir) != ESP_OK || !unique_path(dir, name, path, sizeof(path)) ||
        write_file(path, f->data, f->len) != ESP_OK) {
        ESP_LOGW(TAG, "save failed: %s", f->name);
        snprintf(s_message, sizeof(s_message), "保存失败：%s", name);
        return false;
    }
    ESP_LOGI(TAG, "saved %s (%u B, type %d)", path, (unsigned)f->len, (int)f->type);
    ++s_saved;
    copy_text(s_last_name, sizeof(s_last_name), strrchr(path, '/') + 1);
    s_message[0] = 0;
    if (kind == AIRDROP_KIND_BOOK) {
        ++s_books_saved;
        copy_text(s_open_book, sizeof(s_open_book), path);
    } else if (kind == AIRDROP_KIND_IMAGE) {
        decode_image(f);
        if (!s_image) copy_text(s_message, sizeof(s_message),
                                f->type == ESP32DROP_FILE_HEIC ? "已保存；HEIC 暂不支持预览" :
                                f->type == ESP32DROP_FILE_GIF ? "已保存；GIF 暂不支持预览" : "已保存；图片过大或无法预览");
    }
    return true;
}

/* ---- 绘制 / Drawing ---- */

static EpdRect status_rect(void) { return (EpdRect){UI_MARGIN, UI_CONTENT_TOP, ui_content_width(), 330}; }
static EpdRect back_rect(void) { return ui_bar_rect(0, 1); }

static void draw_status(uint8_t* fb) {
    EpdRect area = status_rect();
    ui_clear_rect_fast(fb, area);
    const char* state = "正在启动隔空投送…";
    if (s_media_lost) state = "TF 卡已移除，接收已停止";
    else if (s_start_err == ESP_ERR_NOT_FOUND) state = "请插入 TF 卡";
    else if (s_start_err != ESP_OK) state = "启动失败，请重新进入本页";
    else if (s_saving) state = "正在保存到 TF 卡…";
    else if (s_status.rx_active) state = "正在接收…";
    else if (s_running) state = "等待接收";
    ui_text(fb, area.x, area.y, UI_PX_BODY, state, EPD_DRAW_ALIGN_LEFT, false);
    char line[200];
    if (s_status.rx_active) snprintf(line, sizeof(line), "已收到 %.1f MB", s_status.rx_bytes / 1048576.0);
    else if (s_running) snprintf(line, sizeof(line), "共享面板里选择「%s」", AIRDROP_NAME);
    else line[0] = 0;
    ui_text(fb, area.x, area.y + 64, UI_PX_CAPTION, line, EPD_DRAW_ALIGN_LEFT, false);
    snprintf(line, sizeof(line), "本次已保存 %u 个文件", s_saved);
    ui_text(fb, area.x, area.y + 116, UI_PX_CAPTION, line, EPD_DRAW_ALIGN_LEFT, false);
    copy_text(line, sizeof(line), s_last_name);
    fit_text(line, UI_PX_CAPTION, area.width);
    ui_text(fb, area.x, area.y + 160, UI_PX_CAPTION, line, EPD_DRAW_ALIGN_LEFT, false);
    copy_text(line, sizeof(line), s_message[0] ? s_message : s_status.errmsg);
    fit_text(line, UI_PX_CAPTION, area.width);
    ui_text(fb, area.x, area.y + 212, UI_PX_CAPTION, line, EPD_DRAW_ALIGN_LEFT, false);
    if (s_status.rx_active && s_status.rx_max) {
        EpdRect track = {area.x, area.y + 280, area.width, 12};
        epd_fill_rect(track, UI_GRAY_LIGHT, fb);
        uint64_t w = (uint64_t)s_status.rx_bytes * track.width / s_status.rx_max;
        track.width = w > (uint64_t)track.width ? track.width : (int)w;
        epd_fill_rect(track, UI_GRAY_BLACK, fb);
    }
}

static void draw_image(uint8_t* fb) {
    ui_clear_page(fb);
    EpdRect area = image_area();
    int width = s_image_w, height = s_image_h;
    if (width > area.width) { height = height * area.width / width; width = area.width; }
    if (height > area.height) { width = width * area.height / height; height = area.height; }
    if (width < 1) width = 1;
    if (height < 1) height = 1;
    int left = area.x + (area.width - width) / 2, top = area.y + (area.height - height) / 2;
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        size_t src = (size_t)((int64_t)y * s_image_h / height) * s_image_w + (size_t)((int64_t)x * s_image_w / width);
        epd_draw_pixel(left + x, top + y, s_image[src], fb);
    }
    if (s_pressed == 0) ui_draw_pressed_round_rect(fb, back_rect(), UI_BTN_RADIUS);
    ui_draw_button(fb, back_rect(), "继续接收", false);
    ui_draw_menu_handle(fb, false);
}

static void render(app_ctx_t* ctx, uint8_t* fb) {
    (void)ctx;
    if (s_image_open && s_image) { draw_image(fb); return; }
    ui_clear_page(fb);
    ui_draw_header(fb, "隔空投送 AirDrop", "iPhone / Mac 直接发送到 TF 卡");
    draw_status(fb);
    int y = UI_CONTENT_TOP + 350;
    ui_hairline(fb, y, UI_MARGIN, ui_content_width(), UI_GRAY_BLACK);
    y += 30;
    static const char* const tips[] = {
        "1. 对方打开隔空投送，接收方设为「所有人」",
        "2. 共享图书 TXT / EPUB 或图片",
        "3. 收完自动打开：图书进阅读，图片全屏看",
        "图书存 /books，图片存 /images",
        "其它文件存 /airdrop；同名自动编号",
        "离开本页即关闭隔空投送",
    };
    for (size_t i = 0; i < sizeof(tips) / sizeof(tips[0]); ++i) {
        ui_text(fb, UI_MARGIN, y, UI_PX_CAPTION, tips[i], EPD_DRAW_ALIGN_LEFT, false);
        y += 48;
    }
    if (s_pressed == 0) ui_draw_pressed_round_rect(fb, back_rect(), UI_BTN_RADIUS);
    ui_draw_button(fb, back_rect(), "停止并返回", false);
    ui_draw_menu_handle(fb, false);
}

/* ---- 生命周期 / Lifecycle ---- */

static void stop_session(void) {
    if (!s_running) return;
    esp_err_t err = esp32drop_stop();
    if (err != ESP_OK) ESP_LOGE(TAG, "stop: %s", esp_err_to_name(err));
    s_running = err != ESP_OK;
    memset(&s_status, 0, sizeof(s_status));
}

static void on_enter(app_ctx_t* ctx) {
    (void)ctx;
    display_set_bulk_io(true);
    s_start_pending = true;
    s_start_err = ESP_OK;
    s_media_lost = s_settle = s_saving = false;
    s_saved = s_books_saved = 0;
    s_last_name[0] = s_message[0] = s_open_book[0] = 0;
    free_image();
    s_pressed = -1;
    s_poll_ms = 0;
    memset(&s_status, 0, sizeof(s_status));
    read_pico_sd_start_probe();
}

static void airdrop_on_exit(app_ctx_t* ctx) {
    s_start_pending = false;
    stop_session();
    free_image();
    display_set_bulk_io(false);
    guard_draw_result(ctx->hl, update_display_white(ctx->hl));
    if (s_books_saved) book_store_notify_changed();
}

static void airdrop_on_media_lost(app_ctx_t* ctx) {
    (void)ctx;
    stop_session();
    s_start_pending = false;
    s_media_lost = true;
    s_open_book[0] = 0;
    s_settle = false;
}

static bool start_session(void) {
    read_pico_sd_info_t sd = {0};
    if (read_pico_sd_get_info(&sd) == ESP_ERR_NOT_FINISHED) return false;
    s_start_pending = false;
    if (!sd.present || !sd.mounted) { s_start_err = ESP_ERR_NOT_FOUND; return true; }
    size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_start_err = esp32drop_start(AIRDROP_NAME, AIRDROP_MAX_RECEIVE, on_file, NULL);
    s_running = s_start_err == ESP_OK;
    // 库计数跨会话累计，以启动时为基线。/ Library counters accumulate across sessions; baseline them at start.
    esp32drop_get_status(&s_status);
    ESP_LOGI(TAG, "start %s, internal %u -> %u B, largest %u B", esp_err_to_name(s_start_err), (unsigned)internal,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (s_start_err != ESP_OK) snprintf(s_message, sizeof(s_message), "错误：%s", esp_err_to_name(s_start_err));
    return true;
}

// 收完一批后的去向：图书交给图书页，图片在本页全屏显示。
// After a batch: hand books to the book page, show images full screen here.
static app_redraw_t finish_batch(app_ctx_t* ctx) {
    if (s_open_book[0]) {
        book_store_notify_changed();
        s_books_saved = 0;
        book_store_request_open(s_open_book);
        s_open_book[0] = 0;
        ctx->request_app = app_book_page();
        return APP_REDRAW_NONE;
    }
    if (s_image) {
        s_image_open = true;
        s_settle = true;
        return APP_REDRAW_FULL;
    }
    s_settle = true;
    return APP_REDRAW_FULL;
}

static app_redraw_t on_tick(app_ctx_t* ctx) {
    if (s_start_pending) {
        if (ctx->now_ms - s_poll_ms < 300) return APP_REDRAW_NONE;
        s_poll_ms = ctx->now_ms;
        return start_session() ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
    }
    if (!s_running) {
        // 未插卡时定期重探，插卡后自动启动。/ Re-probe while the card is absent; start once it appears.
        if (s_start_err == ESP_ERR_NOT_FOUND && !s_media_lost && ctx->now_ms - s_poll_ms >= 2000) {
            s_poll_ms = ctx->now_ms;
            read_pico_sd_start_probe();
            s_start_pending = true;
        }
        return APP_REDRAW_NONE;
    }
    esp32drop_status_t next;
    esp32drop_get_status(&next);
    if (next.upload_ok != s_status.upload_ok && !s_image_open && !s_saving) {
        // 写卡可能要数秒，先把“正在保存”推上屏。/ Saving can take seconds; show it first.
        s_saving = true;
        draw_status(ctx->fb);
        guard_draw_result(ctx->hl, update_display_area_with(ctx->hl, &E0470_FOLLOW_WAVEFORM, MODE_DU, status_rect()));
    }
    esp32drop_poll();
    esp32drop_get_status(&next);
    if (next.files_ok != s_status.files_ok || next.file_err != s_status.file_err) {
        s_saving = false;
        s_status = next;
        return finish_batch(ctx);
    }
    if (ctx->consumed || s_image_open || ctx->now_ms - s_poll_ms < AIRDROP_POLL_MS) return APP_REDRAW_NONE;
    s_poll_ms = ctx->now_ms;
    if (next.rx_active == s_status.rx_active && next.rx_bytes == s_status.rx_bytes &&
        next.file_err == s_status.file_err && !strcmp(next.errmsg, s_status.errmsg)) return APP_REDRAW_NONE;
    s_status = next;
    draw_status(ctx->fb);
    s_area = status_rect();
    return APP_REDRAW_AREA;
}

/* ---- 触摸 / Touch ---- */

static app_redraw_t on_gesture(app_ctx_t* ctx, const ui_gesture_event_t* ev) {
    int old = s_pressed;
    int origin = ui_rect_hit(back_rect(), ev->x0, ev->y0) ? 0 : -1;
    s_pressed = ev->type == UI_GESTURE_PRESS ? origin : -1;
    if (ev->type == UI_GESTURE_TAP && old == 0 && origin == 0 && ui_rect_hit(back_rect(), ev->x, ev->y)) {
        if (s_image_open) {
            free_image();
            s_settle = true;
            return APP_REDRAW_FULL;
        }
        s_start_pending = false;
        stop_session();
        ctx->request_return = true;
        return APP_REDRAW_NONE;
    }
    if (s_pressed == old) return APP_REDRAW_NONE;
    render(ctx, ctx->fb);
    s_settle = false;
    s_area = back_rect();
    return APP_REDRAW_AREA;
}

static bool present(app_ctx_t* ctx, app_redraw_t redraw) {
    if (redraw == APP_REDRAW_FULL && s_settle) {
        render(ctx, ctx->fb);
        guard_draw_result(ctx->hl, update_display_full(ctx->hl));
        s_settle = false;
        return true;
    }
    if (redraw != APP_REDRAW_AREA) return false;
    guard_draw_result(ctx->hl, update_display_area_with(ctx->hl, &E0470_FOLLOW_WAVEFORM, MODE_DU, s_area));
    return true;
}

const app_desc_t app_airdrop = {
    .title = "隔空投送 AirDrop", .detail = "iPhone / Mac 传书或图片", .enter_full = true,
    .on_enter = on_enter, .on_exit = airdrop_on_exit, .on_media_lost = airdrop_on_media_lost,
    .render = render, .present = present, .on_tick = on_tick, .on_gesture = on_gesture,
};
