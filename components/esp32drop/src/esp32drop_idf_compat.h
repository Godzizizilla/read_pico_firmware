/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：替代上游 <Arduino.h>，只提供上游用到的 millis()。
 *
 * English: Stands in for upstream <Arduino.h>; provides only the millis() it uses.
 */
#pragma once

#include <stdint.h>
#include "esp_system.h"
#include "esp_timer.h"

static inline uint32_t millis(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
