# esp32drop

AirDrop 接收端（AWDL 链路层 + TLS/HTTP 应用层），移植自 [ESP32Drop](https://github.com/s-iwaki-d/ESP32Drop)，上游许可 0BSD，见 `LICENSE`。与 Apple 无关，AirDrop 是 Apple Inc. 的商标。

AirDrop receiver (AWDL link layer plus the TLS/HTTP application layer), ported from [ESP32Drop](https://github.com/s-iwaki-d/ESP32Drop) under its 0BSD licence (see `LICENSE`). Not affiliated with Apple; AirDrop is a trademark of Apple Inc.

上游版本 / Upstream revision: `92bdc1221924ca522f3ccbebbd9353612ce5b2d7` (2026-09-07)

## 接口 / Interface

只用 `include/esp32drop.h`。`src/` 下是上游代码，其注释保持上游英文原样。

Use only `include/esp32drop.h`. `src/` holds upstream code with its original English comments.

## 只收录的部分 / What is included

只收接收端：`src/awdl/**`、`src/airdrop/core/**`（去掉发送用的 `ad_pack.h`、`ad_zlib.h`、`ad_uti.h`）、`src/airdrop/port/**`。没有发送端、BLE 和示例。

Receive path only: `src/awdl/**`, `src/airdrop/core/**` (without the send-only `ad_pack.h`, `ad_zlib.h`, `ad_uti.h`) and `src/airdrop/port/**`. No sender, BLE or examples.

## 本地改动 / Local changes

- 用 `src/esp32drop_idf_compat.h` 替代 `<Arduino.h>`，只提供 `millis()`。/ `<Arduino.h>` replaced by `src/esp32drop_idf_compat.h`, which provides only `millis()`.
- mbedTLS 4（IDF v6）：去掉 entropy/ctr_drbg 与 `mbedtls_ssl_conf_rng()`，随机数交给 PSA；`mbedtls_pk_parse_key()` 改五参数；`mbedtls_pk_get_name()` 换成按证书类型取名；最高版本固定 TLS 1.2。/ mbedTLS 4 (IDF v6): entropy/ctr_drbg and `mbedtls_ssl_conf_rng()` removed with PSA as the RNG; five-argument `mbedtls_pk_parse_key()`; `mbedtls_pk_get_name()` replaced by the certificate kind; max version pinned to TLS 1.2.
- 新增 `ad_end()`：监听任务按标志自行退出，`shutdown()` 唤醒阻塞读，关闭全部套接字，丢弃未交付归档，可再次 `ad_begin()`。/ Added `ad_end()`: the listener leaves on a flag, `shutdown()` wakes a blocked read, every socket is closed and an undelivered archive dropped, so `ad_begin()` works again.
- `ad_poll()` 文件名缓冲由 64/128 字节加大到 256/384，避免截断中文长文件名和扩展名。/ `ad_poll()` name buffers grown from 64/128 to 256/384 bytes so long CJK names keep their extension.
- `src/esp32drop.cpp` 是新增的 C 包装与启停顺序。/ `src/esp32drop.cpp` is the new C wrapper and start/stop order.
- `CMakeLists.txt` 关掉上游在 C++26 下会触发的警告（`-Wvolatile`、`-Wcomment` 等）。/ `CMakeLists.txt` silences warnings upstream triggers under C++26 (`-Wvolatile`, `-Wcomment`, …).

## 限制 / Limits

- 独占无线：AWDL 固定在 2.4 GHz 信道 6，不能与普通 WiFi 同时使用。/ Takes the radio: AWDL stays on 2.4 GHz channel 6 and cannot share it with ordinary WiFi.
- 只支持“所有人”模式；“仅限联系人”需要 Apple 签名身份，无法实现。/ Everyone mode only; Contacts Only needs Apple-signed identity.
- 解码后单次归档上限由调用方给出（本机 6 MiB），超限向发送方返回 413。/ The decoded archive ceiling is the caller's (6 MiB here); larger transfers get HTTP 413.
- 上游实测在 M5Stack StopWatch（Arduino core 3.3.8 / IDF 5.x）；本移植未经上游验证。/ Upstream was measured on an M5Stack StopWatch (Arduino core 3.3.8 / IDF 5.x); this port is not upstream-verified.
