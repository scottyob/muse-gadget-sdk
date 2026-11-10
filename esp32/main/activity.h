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

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// One shared activity state for the whole gadget (v20). Pure idle - no
// source raised - is the only state in which the wake-word detector may
// be resident: it holds ~16 KB of internal RAM that a picture download,
// a TLS handshake or a call also needs. Any subsystem that starts doing
// work raises its source, and the transition out of idle is what makes
// the wake listener unload; when the last source clears, the gadget is
// idle again and the listener may re-arm (after the hysteresis in
// activity_busy_for_wake(), so bursts of short activities do not thrash
// the detector's load/unload cycle).
//
// Sources are reference-counted per source, so nested users of the same
// kind (a call inside a voice turn) compose.
typedef enum {
    ACTIVITY_VOICE = 0,  // a voice turn (note or call) owns the mic
    ACTIVITY_CALL,       // a realtime call is in progress (phone icon)
    ACTIVITY_IMAGE,      // a picture download/draw is in progress
    ACTIVITY_COUNT
} activity_t;

void activity_raise(activity_t source);
void activity_clear(activity_t source);
bool activity_any(void);
bool activity_source_active(activity_t source);

// What the wake-word gate consults: true while any activity is raised,
// and for a few seconds after the last one cleared (re-arm hysteresis).
bool activity_busy_for_wake(void);

#ifdef __cplusplus
}
#endif
