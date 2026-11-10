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

// Always-on wake phrase ("Hey Boeing") on ESP-SR MultiNet6 (English).
// MultiNet6 takes its commands as plain English graphemes, so the
// phrases are registered as text and converted inside the model.
// Detection runs entirely on the chip from the "model" flash partition;
// the mic stream never leaves the board until a detection fires the
// fire callback. Since v20 voice.c handles that as a dedicated wake
// event and starts a call directly; up to v19 it queued a synthetic
// button tap, which cost a fake voice-note turn (open, record, cancel)
// before the call's own admission - two handshakes, ~2 s all told.
// Since v21 the matched phrase picks the action; since v23 the only
// action left on the wake word is the call, on the plain phrase:
// "hey boeing" starts the call. (v21-v22 had a note action too - see
// WAKE_PHRASES for the full history.)
//
// The microphone has one reader at a time by convention: turns and
// calls read it while the owner's gate is up, and this task reads it
// only while the gate is down, re-checking between the board's short
// reads so a button press takes the mic back within a few tens of
// milliseconds.
//
// The detector is a fair-weather resident: it loads only while the
// gadget is idle, and unloads the moment the gate goes up (a turn, a
// call, or its own detection firing). Resident, it holds ~16 KB of
// internal RAM (plus ~4 MB of PSRAM); a realtime call starting on top
// of that in v15 squeezed internal RAM to ~1 KB largest-block and the
// call never went live. Unloaded, the call gets the RAM it had before
// the wake word existed, and the detector re-arms a second or two
// after the call ends.
//
// Since v20 the gate is the shared activity state (activity.c): pure
// idle is the only state with the detector resident, and any activity
// at all - a picture download as much as a call - raises the gate,
// which is what finally lets pictures download again while the wake
// word exists.
//
// Earlier lessons baked in below:
// - v13 boot-looped because this task ran on a PSRAM stack while
//   esp_srmodel_init froze the cache to read flash (the SDK's own
//   esp_task_stack_is_sane_when_cache_frozen assert). Internal stack.
// - v14 never fired (MultiNet6's shipped detection threshold is 0.9;
//   see WAKE_THRESHOLD) and its boot-time model load starved the Link
//   control task, so the load waits for the boot to settle.

#include "wake_word.h"

#include <stdatomic.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "led_status.h"
#include "muse_chat.h"
#include "model_path.h"
#include "voice_board.h"

static const char *TAG = "link.wake";

// The wake phrases, as graphemes (command ids are 1-based, in this
// order). MultiNet6 pronounces them from the text, so near-spellings
// widen the net for how the name actually comes out. ("Hey bowing" is
// the spelling that has actually fired on hardware, at prob 0.78.)
// History of what a phrase means: v21 split them - the plain
// spellings (ids 1-3) recorded a voice note, "hey call ..." (ids 4-6)
// started a realtime call; v22 unregistered the note phrases, so
// only "hey call boeing" could start a call and notes went back to
// the button; v23 renames the call onto the plain phrase, as Scott
// expected once notes left the wake word: "hey boeing" starts a
// call, and no wake phrase records a note. The note machinery
// (EVT_WAKE_NOTE, the pre-roll below) stays compiled in, dormant;
// reviving v21's two-phrase scheme means re-adding "hey call ..." as
// ids 4-6 and giving ids 1-3 back to the note range below.
static const char *const WAKE_PHRASES[] = {
    "hey boeing", "hey boing", "hey bowing",
};
#define WAKE_PHRASE_COUNT (sizeof(WAKE_PHRASES) / sizeof(WAKE_PHRASES[0]))
#define WAKE_NOTE_ID_MAX  0   // ids <= this are notes; none are, since v23

// The note path's pre-roll (v21): while the listener runs, every mic
// read is also copied into a rolling PSRAM ring; at a note detection
// the ring is linearized into a snapshot the voice task streams ahead
// of the live mic, so words spoken during the detect/unload gap -
// including words that overlap the wake phrase itself - are not
// clipped off the front of the note. The wake phrase is NOT sliced
// out: detection timing varies too much for a fixed cut, and a note
// transcript that starts "Hey Boeing, ..." is the acceptable price.
// 2.5 s at 16 kHz = 40,000 samples = 80 KB, twice over (ring +
// snapshot) - nothing next to the model's ~4 MB of PSRAM.
#define WAKE_PREROLL_SAMPLES (16000 * 5 / 2)
static int16_t *s_preroll_ring;
static int16_t *s_preroll_snap;
static size_t s_preroll_head;      // next write index in the ring
static size_t s_preroll_len;       // valid samples in the ring
static size_t s_preroll_snap_len;

static void preroll_push(const int16_t *pcm, size_t n) {
    if (!s_preroll_ring) return;
    for (size_t i = 0; i < n; i++) {
        s_preroll_ring[s_preroll_head] = pcm[i];
        s_preroll_head = (s_preroll_head + 1) % WAKE_PREROLL_SAMPLES;
        if (s_preroll_len < WAKE_PREROLL_SAMPLES) s_preroll_len++;
    }
}

// Linearize the ring (oldest first) into the snapshot buffer.
static void preroll_snapshot(void) {
    s_preroll_snap_len = 0;
    if (!s_preroll_ring || !s_preroll_snap) return;
    size_t start = (s_preroll_head + WAKE_PREROLL_SAMPLES - s_preroll_len)
                   % WAKE_PREROLL_SAMPLES;
    for (size_t i = 0; i < s_preroll_len; i++) {
        s_preroll_snap[i] = s_preroll_ring[(start + i) % WAKE_PREROLL_SAMPLES];
    }
    s_preroll_snap_len = s_preroll_len;
}

const int16_t *wake_word_preroll(size_t *samples) {
    *samples = s_preroll_snap_len;
    return s_preroll_snap;
}
// The model ships a 0.9 operating point; a custom phrase in a real
// room rarely scores that high. 0.5 with a cheap blast radius (a call
// opens, hears quiet, hangs itself up) is the right trade.
#define WAKE_THRESHOLD    0.5f
// How long MultiNet listens before reporting a timeout (and the
// detection window restarts). The stock example value.
#define WAKE_WINDOW_MS    6000
// The listener task's stack. It must be internal RAM (the v13 lesson
// below), so its size is internal RAM spent for as long as the task
// lives. It was 12 KB up to v19, but the window report's high-water
// mark showed only ~2 KB ever used (init peaks similar), so v20 runs
// 8 KB and keeps logging the high-water mark to prove the margin.
#define WAKE_STACK_BYTES  8192

static wake_word_gate_fn s_gate;
static wake_word_fire_fn s_fire;
// True while the detector is resident; read by other tasks
// (wake_word_armed) that wait for an unload to complete.
static atomic_bool s_armed;

// Detector state, live only while the gadget is idle (see the header).
static srmodel_list_t *s_models;
static const esp_mn_iface_t *s_multinet;
static model_iface_data_t *s_model_data;
static int16_t *s_chunk_buf;
static int s_chunksize;

static bool detector_start(void) {
    s_models = esp_srmodel_init("model");
    if (!s_models) {
        ESP_LOGE(TAG, "no model partition found; wake word is off");
        return false;
    }
    char *mn_name = esp_srmodel_filter(s_models, ESP_MN_PREFIX, ESP_MN_ENGLISH);
    s_multinet = mn_name ? esp_mn_handle_from_name(mn_name) : NULL;
    s_model_data = s_multinet ? s_multinet->create(mn_name, WAKE_WINDOW_MS) : NULL;
    if (!s_model_data) {
        ESP_LOGE(TAG, "no usable English MultiNet model; wake word is off");
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        return false;
    }
    s_multinet->set_det_threshold(s_model_data, WAKE_THRESHOLD);
    bool ok = esp_mn_commands_alloc(s_multinet, s_model_data) == ESP_OK;
    for (size_t i = 0; ok && i < WAKE_PHRASE_COUNT; i++) {
        ok = esp_mn_commands_add((int)i + 1, WAKE_PHRASES[i]) == ESP_OK;
    }
    esp_mn_error_t *err = ok ? esp_mn_commands_update() : NULL;
    if (err && err->num) {
        for (int i = 0; i < err->num; i++) {
            ESP_LOGE(TAG, "phrase rejected: %s", err->phrases[i]->string);
        }
        ok = false;
    }
    s_chunksize = s_multinet->get_samp_chunksize(s_model_data);
    if (ok && !s_chunk_buf) {
        s_chunk_buf = heap_caps_malloc(s_chunksize * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        ok = s_chunk_buf != NULL;
    }
    // The note pre-roll buffers: allocated once, kept across unload
    // cycles. A failure only costs the pre-roll, not the listener.
    if (!s_preroll_ring) {
        s_preroll_ring = heap_caps_malloc(WAKE_PREROLL_SAMPLES * sizeof(int16_t),
                                          MALLOC_CAP_SPIRAM);
        s_preroll_snap = heap_caps_malloc(WAKE_PREROLL_SAMPLES * sizeof(int16_t),
                                          MALLOC_CAP_SPIRAM);
        if (!s_preroll_ring || !s_preroll_snap) {
            ESP_LOGW(TAG, "no PSRAM for the note pre-roll; notes start at detection");
            heap_caps_free(s_preroll_ring);
            heap_caps_free(s_preroll_snap);
            s_preroll_ring = s_preroll_snap = NULL;
        }
    }
    // A fresh listening period starts a fresh pre-roll: whatever the
    // ring held belongs to before the activity that unloaded us.
    s_preroll_head = 0;
    s_preroll_len = 0;
    s_preroll_snap_len = 0;
    if (!ok) {
        ESP_LOGE(TAG, "detector would not start; wake word is off");
        s_multinet->destroy(s_model_data);
        s_model_data = NULL;
        esp_srmodel_deinit(s_models);
        s_models = NULL;
        return false;
    }
    ESP_LOGI(TAG, "listening: 'hey boeing' -> call (wake notes off) (%s, %d Hz, %d-sample chunks, threshold %.2f)",
             mn_name, s_multinet->get_samp_rate(s_model_data), s_chunksize, WAKE_THRESHOLD);
    atomic_store(&s_armed, true);
    led_status_set_wake_icon(true);
    return true;
}

static void detector_stop(void) {
    if (!s_model_data) return;
    s_multinet->destroy(s_model_data);
    s_model_data = NULL;
    esp_srmodel_deinit(s_models);
    s_models = NULL;
    atomic_store(&s_armed, false);
    led_status_set_wake_icon(false);
    ESP_LOGI(TAG, "detector unloaded; internal free %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

bool wake_word_armed(void) {
    return atomic_load(&s_armed);
}

bool wake_word_wait_unloaded(uint32_t timeout_ms) {
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (atomic_load(&s_armed)) {
        if (esp_timer_get_time() >= deadline) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

static void wake_task(void *arg) {
    // Arm once the voice session is up, plus 2 s - not on a fixed
    // clock (v24; the flat 25 s here up to v23 was Scott's "dramatic
    // long wait"). The delay exists for one reason: loading the model
    // during the boot crunch starved the Link control task of
    // internal RAM in v14 and the session never came up (the status
    // bars blinked "disconnected"). The session being ready IS the
    // condition that crunch is over - it is typically up in ~7 s, so
    // the listener now arms at ~9 s instead of 25. If the session
    // never comes up the listener never arms, which costs nothing: a
    // fire would be dropped anyway (wake_fire checks readiness). The
    // wait is logged every ~10 s so a stuck boot stays diagnosable.
    ESP_LOGI(TAG, "wake word arms 2 s after the voice session is ready");
    for (int waited = 0; !muse_hatch_ready(); waited++) {
        vTaskDelay(pdMS_TO_TICKS(250));
        if (waited > 0 && waited % 40 == 0) {
            ESP_LOGI(TAG, "still waiting for the voice session (%d s)", waited / 4);
        }
    }
    vTaskDelay(pdMS_TO_TICKS(2000));

    bool listening = false;
    uint32_t chunks_window = 0;
    int peak_window = 0;
    for (;;) {
        if (s_gate && s_gate()) {
            if (listening) {
                voice_board_mic_stop();
                listening = false;
            }
            detector_stop();   // hand the RAM to the turn/call
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (!s_model_data) {
            if (!detector_start()) {
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
        }
        if (!listening) {
            if (voice_board_mic_start() != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            listening = true;
        }
        // Fill one detector chunk from the board's short reads, checking
        // the gate between reads so a press reclaims the mic promptly,
        // and remember how loud the room was for the window report.
        size_t filled = 0;
        while (filled < (size_t)s_chunksize) {
            if (s_gate && s_gate()) break;
            int peak = 0;
            size_t want = (size_t)s_chunksize - filled;
            if (want > 320) want = 320;   // voice_board_mic_read's cap
            size_t got = voice_board_mic_read(s_chunk_buf + filled, want, &peak);
            if (!got) break;
            if (peak > peak_window) peak_window = peak;
            preroll_push(s_chunk_buf + filled, got);
            filled += got;
        }
        if (filled < (size_t)s_chunksize) continue;   // gated or mic hiccup
        chunks_window++;
        esp_mn_state_t state = s_multinet->detect(s_model_data, s_chunk_buf);
        if (state == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *res = s_multinet->get_results(s_model_data);
            int id = res && res->num > 0 ? res->command_id[0] : -1;
            float prob = res && res->num > 0 ? res->prob[0] : 0.0f;
            bool valid = id >= 1 && id <= (int)WAKE_PHRASE_COUNT;
            wake_action_t action = valid && id > WAKE_NOTE_ID_MAX
                                       ? WAKE_ACTION_CALL
                                       : WAKE_ACTION_NOTE;
            // The action is on the line so a live test can settle
            // whether the phrases cross-fire (see WAKE_PHRASES).
            ESP_LOGI(TAG, "detected id %d '%s' (prob %.2f)%s", id,
                     res ? res->string : "?", prob,
                     valid ? (action == WAKE_ACTION_CALL ? " -> CALL" : " -> NOTE")
                           : "");
            s_multinet->clean(s_model_data);
            chunks_window = 0;
            peak_window = 0;
            if (valid) {
                int64_t detected_us = esp_timer_get_time();
                ESP_LOGI(TAG, "wake phrase heard at %lld ms; starting a %s",
                         detected_us / 1000,
                         action == WAKE_ACTION_CALL ? "call" : "note");
                voice_board_mic_stop();
                listening = false;
                if (action == WAKE_ACTION_NOTE) preroll_snapshot();
                if (s_fire) s_fire(detected_us, action, id);
                // Free the detector before the note/call allocates:
                // RAM headroom is the difference between a live call
                // and one stuck in "Thinking" (the v15 failure).
                detector_stop();
            }
        } else if (state == ESP_MN_STATE_TIMEOUT) {
            s_multinet->clean(s_model_data);
            // The liveness report: proves the loop is fed, and shows
            // the loudest thing the detector heard this window. A full
            // 6 s window at 32 ms a chunk is ~187 chunks; speech peaks
            // in the thousands, a silent or starved feed near zero.
            ESP_LOGI(TAG, "window: %lu chunks, peak %d, stack free %u",
                     (unsigned long)chunks_window, peak_window,
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
            chunks_window = 0;
            peak_window = 0;
        }
    }
}

void wake_word_start(wake_word_gate_fn gate, wake_word_fire_fn fire) {
    s_gate = gate;
    s_fire = fire;
    // Internal stack, not PSRAM like the voice task's: loading the
    // model reads and mmaps flash, which freezes the cache, and a task
    // whose stack sits in external memory asserts there (v13).
    if (xTaskCreate(wake_task, "wake", WAKE_STACK_BYTES, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "no memory for the wake word listener");
    }
}
