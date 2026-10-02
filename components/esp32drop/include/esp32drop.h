/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：ESP32Drop AirDrop 接收端的 C 接口：启停 AWDL 与 TLS 监听、交付收到的文件、读取状态。
 * 不碰文件系统和显示，保存位置由调用方决定。
 *
 * English: C surface over the ESP32Drop AirDrop receiver: start/stop AWDL and the TLS
 * listener, hand over received files and report status. No file system or display
 * access; the caller decides where files go.
 *
 * 冻结：独占无线，与普通 WiFi（传书页）互斥；只做“所有人”接收，不发送。
 * Frozen: takes the radio exclusively and cannot run beside ordinary WiFi (transfer
 * page); receive-only, "Everyone" mode.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/// 文件类型，按内容嗅探，不看扩展名。/ File type sniffed from content, never the name.
typedef enum {
    ESP32DROP_FILE_OTHER = 0, ///< 其它 / Anything else
    ESP32DROP_FILE_JPEG,      ///< JPEG
    ESP32DROP_FILE_PNG,       ///< PNG
    ESP32DROP_FILE_GIF,       ///< GIF
    ESP32DROP_FILE_HEIC,      ///< HEIC
    ESP32DROP_FILE_PDF,       ///< PDF
    ESP32DROP_FILE_ZIP,       ///< ZIP 容器（含 EPUB）/ ZIP container, EPUB included
    ESP32DROP_FILE_APPLEDOUBLE, ///< macOS 元数据旁文件 / macOS metadata sidecar
} esp32drop_file_type_t;

/// 一个收到的文件，只在回调期间有效。/ One received file, valid only during the callback.
typedef struct {
    /// 文件内容，位于 PSRAM。/ File bytes, in PSRAM.
    const uint8_t* data;
    uint32_t len;
    /// 去掉路径的 UTF-8 文件名。/ UTF-8 basename.
    const char* name;
    esp32drop_file_type_t type;
    /// 本次传输中的序号与总数。/ Position and count within this transfer.
    uint32_t index;
    uint32_t count;
} esp32drop_file_t;

/// 返回 false 表示没能使用该文件，计入 file_err。/ Return false if the file could not be used; counted in file_err.
typedef bool (*esp32drop_file_cb_t)(void* ctx, const esp32drop_file_t* file);

typedef struct {
    /// 已启动并在监听。/ Started and listening.
    bool running;
    /// 正在接收。/ A transfer is arriving.
    bool rx_active;
    /// 已收压缩字节。/ Compressed bytes received so far.
    uint32_t rx_bytes;
    /// 生效的接收上限，首次传输前为 0。/ Receive ceiling in force; 0 before the first transfer.
    uint32_t rx_max;
    /// 累计计数：被发现、被询问、上传完成、交付成功、交付失败。
    /// Cumulative: discovered, asked, uploaded, delivered, refused.
    uint32_t discover_ok, ask_ok, upload_ok, files_ok, file_err;
    /// AWDL 已锁定网格时序。/ AWDL is locked to the mesh timing.
    bool awdl_locked;
    /// 最近一次失败的说明。/ Description of the latest failure.
    char errmsg[40];
} esp32drop_status_t;

/// 启动 AWDL 与 AirDrop 监听；name 是对方共享面板上显示的 UTF-8 名称。
/// Start AWDL and the AirDrop listener; name is the UTF-8 label on the sender's share sheet.
/// max_receive 为解码后归档上限字节，0 取库默认 6 MiB。回调只在 esp32drop_poll() 的调用任务上运行。
/// max_receive bounds the decoded archive in bytes, 0 for the 6 MiB default. Callbacks run only on the esp32drop_poll() caller.
esp_err_t esp32drop_start(const char* name, uint32_t max_receive, esp32drop_file_cb_t cb, void* ctx);
/// 停止监听并释放无线；未启动时直接返回。必须与 start 同一任务调用。
/// Stop listening and release the radio; returns at once when not started. Call from the start task.
esp_err_t esp32drop_stop(void);
/// 交付已收完的文件并把库日志转到 ESP_LOG；没有工作时开销很小。
/// Deliver finished files and forward library logs to ESP_LOG; cheap when idle.
void esp32drop_poll(void);
/// 读取状态快照。/ Read a status snapshot.
void esp32drop_get_status(esp32drop_status_t* out);

#ifdef __cplusplus
}
#endif
