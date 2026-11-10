/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "activity.h"

#include <stdint.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

// After the last activity clears, keep reporting busy to the wake gate
// for this long. Re-loading the speech model takes seconds, so a quick
// idle gap between two activities (or a burst of short commands) must
// not start a load that the next activity immediately tears down.
#define ACTIVITY_REARM_US 3000000

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_counts[ACTIVITY_COUNT];
static int64_t s_last_clear_us;

void activity_raise(activity_t source) {
    if (source >= ACTIVITY_COUNT) return;
    portENTER_CRITICAL(&s_lock);
    if (s_counts[source] < 255) s_counts[source]++;
    portEXIT_CRITICAL(&s_lock);
}

void activity_clear(activity_t source) {
    if (source >= ACTIVITY_COUNT) return;
    portENTER_CRITICAL(&s_lock);
    if (s_counts[source] > 0) s_counts[source]--;
    bool any = false;
    for (int i = 0; i < ACTIVITY_COUNT; i++) any = any || s_counts[i] > 0;
    if (!any) s_last_clear_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_lock);
}

bool activity_any(void) {
    portENTER_CRITICAL(&s_lock);
    bool any = false;
    for (int i = 0; i < ACTIVITY_COUNT; i++) any = any || s_counts[i] > 0;
    portEXIT_CRITICAL(&s_lock);
    return any;
}

bool activity_source_active(activity_t source) {
    if (source >= ACTIVITY_COUNT) return false;
    portENTER_CRITICAL(&s_lock);
    bool active = s_counts[source] > 0;
    portEXIT_CRITICAL(&s_lock);
    return active;
}

bool activity_busy_for_wake(void) {
    if (activity_any()) return true;
    portENTER_CRITICAL(&s_lock);
    int64_t last = s_last_clear_us;
    portEXIT_CRITICAL(&s_lock);
    return last && esp_timer_get_time() - last < ACTIVITY_REARM_US;
}
