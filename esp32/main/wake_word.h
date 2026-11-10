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
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Gate: true while the microphone belongs to someone else - since v20,
// while the shared activity state (activity.h) is raised at all, or a
// button press is on its way to a turn. The listener reads the mic only
// while the gate is down.
// Fire: called on the listener task when a wake phrase is heard, with
// the esp_timer time of the detection (so the handler can log the
// detection-to-start latency), the action the matched phrase selects
// and the command id that matched. Since v20 voice.c handles these as
// dedicated wake events, not synthetic button taps; since v21 there
// are two actions (below).
typedef bool (*wake_word_gate_fn)(void);

// What a matched wake phrase asks for: since v23 the only registered
// phrases ("hey boeing" and its boing/bowing spellings) start a
// realtime call. WAKE_ACTION_NOTE is kept from v21's two-phrase
// scheme; no phrase maps to it since v23 (notes are button-only),
// but the note path remains compiled in, dormant.
typedef enum {
    WAKE_ACTION_NOTE = 0,
    WAKE_ACTION_CALL,
} wake_action_t;

typedef void (*wake_word_fire_fn)(int64_t detected_us, wake_action_t action,
                                  int command_id);

// Start the always-on "Hey Boeing" listener (ESP-SR MultiNet, on-device:
// no audio leaves the gadget while it listens). Silently stays off when
// the model partition or the speech model is missing.
void wake_word_start(wake_word_gate_fn gate, wake_word_fire_fn fire);

// The note path's pre-roll: a linear snapshot of the last ~2.5 s of
// microphone audio the listener heard up to the detection that fired
// (wake phrase included - see wake_word.c). Taken on the listener task
// at fire time; valid until the listener next arms, and the note path
// consumes it before that can happen (it runs under ACTIVITY_VOICE,
// which keeps the listener unloaded). NULL/0 when no snapshot exists.
const int16_t *wake_word_preroll(size_t *samples);

// True while the detector is resident (loaded and listening). Other
// subsystems poll this to wait for the unload the gate triggers, and
// the status screen shows its ear icon from it.
bool wake_word_armed(void);

// Wait until the detector has unloaded (the gate the caller raised made
// the listener hand its RAM and the microphone back), or the timeout
// passes. Returns true when the detector is out. The listener notices
// the gate within a chunk or two, so a second is generous; callers that
// cannot afford to wait that long should not call this on a hot path.
bool wake_word_wait_unloaded(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
