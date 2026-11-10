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

#include "voice.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "activity.h"
#include "button.h"
#include "config_store.h"
#include "led_status.h"
#include "muse_chat.h"
#include "voice_board.h"
#include "voice_muse_chat.h"
#include "voice_player.h"
#include "wake_word.h"
#if CONFIG_MUSE_REALTIME_VOICE
#include "muse_voice_call.h"
#include "muse_voice_call_core.h"
#endif

static const char *TAG = "link.voice";

#define KEY_VOLUME          "voice_volume"
#define DEFAULT_VOLUME      60
// Volume change per detent of the dial.
#define VOLUME_STEP         5
// Store a dialled volume once the dial has rested this long.
#define VOLUME_SAVE_MS      2000
#define DIAL_POLL_MS        20

#define CAPTURE_MAX_MS      15000
#define CAPTURE_MIN_MS      300
#define CAPTURE_CHUNK       (VOICE_MIC_RATE / 50)      // 20 ms
#define CAPTURE_MAX         (VOICE_MIC_RATE * CAPTURE_MAX_MS / 1000)
// Keep listening briefly after release so the last word is not clipped.
#define RELEASE_TAIL_MS     250
#define REPLY_CHUNK         (VOICE_PLAYER_RATE / 50)   // 20 ms

// EVT_WAKE (v20): the wake word's own event, handled by the voice task
// directly as a call, instead of the synthetic press/release pair it
// used up to v19 (whose "tap" first opened and then cancelled a voice
// note turn). Only ever queued by wake_fire(), and only when a
// realtime call can actually start.
// EVT_WAKE_NOTE (v21): a wake phrase recorded a note - pre-roll
// streamed first, silence ending it, no button involved. No wake
// phrase queues it since v23 ("hey boeing" starts a call instead;
// notes are button-only), but the path stays compiled in, dormant.
// (Button behaviour is unchanged: tap = call, hold = note.)
typedef enum { EVT_PRESS, EVT_RELEASE, EVT_WAKE, EVT_WAKE_NOTE } voice_evt_t;

static QueueHandle_t s_events;
static atomic_bool s_ready;
static atomic_int s_volume;
#if CONFIG_SR_MN_EN_MULTINET6_QUANT
// Wake word (wake_word.c): the listener reads the mic only while the
// shared activity state (activity.h) is clear - this file raises
// ACTIVITY_VOICE around turns and calls - and no button press is on
// its way (s_press_hint_at, a few seconds' grace from the press).
// s_wake_detected_us carries the detection time of the wake event the
// voice task is about to handle, for its latency logs.
static atomic_llong s_press_hint_at;
static atomic_llong s_wake_detected_us;
#endif

static int load_volume(void) {
    char buf[8];
    if (!config_get_str(KEY_VOLUME, buf, sizeof(buf)) || !buf[0]) return DEFAULT_VOLUME;
    int v = atoi(buf);
    return v < 0 ? 0 : v > 100 ? 100 : v;
}

static bool store_volume(int volume) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", volume);
    return config_set_str(KEY_VOLUME, buf);
}

// Set the speaker volume and show it on the ring.
static void apply_volume(int volume) {
    atomic_store(&s_volume, volume);
    voice_board_set_volume(volume);
    led_status_show_volume(volume);
}

// Turns the volume with the dial. On an internal-RAM stack: storing the volume
// writes NVS.
static void dial_task(void *arg) {
    int64_t save_at = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(DIAL_POLL_MS));
        int steps = voice_board_dial_steps();
        if (steps) {
            int volume = atomic_load(&s_volume) + steps * VOLUME_STEP;
            apply_volume(volume < 0 ? 0 : volume > 100 ? 100 : volume);
            save_at = esp_timer_get_time() + VOLUME_SAVE_MS * 1000LL;
        } else if (save_at && esp_timer_get_time() >= save_at) {
            save_at = 0;
            int volume = atomic_load(&s_volume);
            if (store_volume(volume)) ESP_LOGI(TAG, "volume %d", volume);
            else ESP_LOGW(TAG, "volume could not be stored");
        }
    }
}

// Did a press arrive? Releases are dropped, and so is a wake event:
// it only ever means something to the voice task's main loop (below),
// and the detector cannot fire while a turn or call is running anyway.
static bool pressed_again(TickType_t wait) {
    voice_evt_t evt;
    while (xQueueReceive(s_events, &evt, wait) == pdTRUE) {
        if (evt == EVT_PRESS) return true;
        wait = 0;
    }
    return false;
}

// The voice task's main loop waits here: the next thing worth acting
// on, a button press or a wake-word detection. Releases are dropped.
static voice_evt_t wait_event(void) {
    voice_evt_t evt;
    for (;;) {
        if (xQueueReceive(s_events, &evt, portMAX_DELAY) == pdTRUE
            && (evt == EVT_PRESS || evt == EVT_WAKE || evt == EVT_WAKE_NOTE)) {
            return evt;
        }
    }
}

// Stream the microphone into the turn until release (plus a tail) or the
// limit. Returns the number of samples, 0 if the capture failed. Also
// reports how many samples were captured before the release, which is
// the press length the tap test must use: with the tail included, a tap
// would have to be under CAPTURE_MIN_MS - RELEASE_TAIL_MS (50 ms) to
// count as one, which no human press manages.
static size_t record(size_t *press_samples) {
    static int16_t chunk[CAPTURE_CHUNK];
    if (voice_board_mic_start() != ESP_OK) return 0;
    size_t samples = 0;
    int64_t stop_at = 0;
    *press_samples = 0;
    while (samples < CAPTURE_MAX) {
        voice_evt_t evt;
        if (!stop_at && xQueueReceive(s_events, &evt, 0) == pdTRUE && evt == EVT_RELEASE) {
            *press_samples = samples;
            stop_at = esp_timer_get_time() + RELEASE_TAIL_MS * 1000LL;
        }
        if (stop_at && esp_timer_get_time() >= stop_at) break;
        int peak = 0;
        size_t got = voice_board_mic_read(chunk, CAPTURE_CHUNK, &peak);
        if (!got) break;
        muse_hatch_turn_audio(chunk, got);
        samples += got;
        led_status_set_level(peak / 12000.0f);
    }
    voice_board_mic_stop();
    if (!stop_at) *press_samples = samples;   // never released: held throughout
    if (samples >= CAPTURE_MAX) ESP_LOGI(TAG, "capture limit reached");
    return samples;
}

static bool fail(const char *why) {
    ESP_LOGW(TAG, "turn failed: %s", why);
    led_status_set_voice(LED_VOICE_ERROR);
    return false;
}

// Play the reply as it arrives. Returns true if a new press interrupted it.
static bool reply(void) {
    static int16_t pcm[REPLY_CHUNK];
    char text[96];
    char incomplete[sizeof(text)] = "";
    bool done = false;
    size_t played = 0;
    int64_t t0 = esp_timer_get_time();
    voice_player_begin();
    for (;;) {
        if (pressed_again(0)) {
            muse_hatch_turn_cancel();
            voice_player_stop();
            return true;
        }
        muse_hatch_ev_t ev;
        while ((ev = muse_hatch_turn_event(text, sizeof(text))) != MUSE_HATCH_EV_NONE) {
            switch (ev) {
            case MUSE_HATCH_EV_HEARD:
                ESP_LOGI(TAG, "heard: %s", text);
                led_status_set_voice(LED_VOICE_THINKING);
                break;
            case MUSE_HATCH_EV_REPLY:
                if (!voice_player_started()) led_status_set_voice(LED_VOICE_BUFFERING);
                break;
            case MUSE_HATCH_EV_DONE:
                done = true;
                strlcpy(incomplete, text, sizeof(incomplete));
                break;
            case MUSE_HATCH_EV_ERROR:
                voice_player_stop();
                return fail(text);
            default:
                break;
            }
        }
        // After DONE the reply's audio is all decoded; drain what's left.
        size_t n = muse_hatch_turn_read(pcm, REPLY_CHUNK, done ? 0 : 20);
        if (n) {
            voice_player_write(pcm, n);
            played += n;
        } else if (done) {
            break;
        }
        if (voice_player_started()) led_status_set_voice(LED_VOICE_SPEAKING);
    }
    voice_player_end();
    while (!voice_player_wait(0)) {
        if (pressed_again(pdMS_TO_TICKS(40))) {
            voice_player_stop();
            return true;
        }
        if (voice_player_started()) led_status_set_voice(LED_VOICE_SPEAKING);
    }
    ESP_LOGI(TAG, "reply: %.1fs of speech, %.1fs total", (double)played / VOICE_PLAYER_RATE,
             (esp_timer_get_time() - t0) / 1e6);
    if (incomplete[0]) return fail(incomplete);
    led_status_set_voice(LED_VOICE_IDLE);
    return false;
}

#if CONFIG_MUSE_REALTIME_VOICE
// How far the speaker runs behind the player's buffer (DMA), and how long the
// mic stays shut after Muse's voice stops when the call is half duplex.
#define CALL_SPEAKER_LAG_US 80000
#define CALL_TAIL_US        300000
#define CALL_END_WAIT_US    (20LL * 1000000)

// Full duplex only where the XMOS confirms echo cancellation; otherwise the
// mic is shut while Muse is audibly talking.
static bool call_full_duplex(void) {
#if CONFIG_MUSE_REALTIME_VOICE_AEC_DUPLEX
    return voice_board_echo_cancelled();
#else
    return false;
#endif
}

// A realtime call, from a tap until it ends; a press (or the mute switch)
// hangs up. The mic read paces the loop; the player's own task drains the
// reply to the speaker.
// Two-tone call chimes, generated as PCM and played through the voice
// player: rising (low-high) as a call starts, falling (high-low) when
// it ends — confirmation you can hear without looking at the screen.
// The start chime writes into the stream run_call has already begun
// and does not end it or wait: v17 played it at the READY event with
// its own begin/end/wait inside the call loop, which delayed it
// behind the connection, displaced (and likely clipped) the greeting
// queued behind it, and held the half-duplex mic gate shut longer.
#define CHIME_TONE_SAMPLES (VOICE_PLAYER_RATE * 120 / 1000)   // 120 ms a tone
#define CHIME_FADE_SAMPLES (VOICE_PLAYER_RATE * 10 / 1000)    // click-free edges
#define CHIME_PEAK         11000                              // ~1/3 full scale
#define CHIME_TWO_PI       6.283185307179586

static void call_chime(bool up, muse_vc_gate_t *gate, bool own_player) {
    static const int freqs[2][2] = {{660, 880}, {880, 660}};
    const int *f = freqs[up ? 0 : 1];
    int16_t buf[160];
    if (own_player) voice_player_begin();
    for (int tone = 0; tone < 2; tone++) {
        double phase = 0, step = CHIME_TWO_PI * f[tone] / VOICE_PLAYER_RATE;
        for (int i = 0; i < CHIME_TONE_SAMPLES; i++) {
            double env = 1.0;
            if (i < CHIME_FADE_SAMPLES) {
                env = 0.5 - 0.5 * cos(3.141592653589793 * i / CHIME_FADE_SAMPLES);
            } else if (i >= CHIME_TONE_SAMPLES - CHIME_FADE_SAMPLES) {
                env = 0.5 - 0.5 * cos(3.141592653589793 * (CHIME_TONE_SAMPLES - 1 - i) / CHIME_FADE_SAMPLES);
            }
            buf[i % 160] = (int16_t)(CHIME_PEAK * env * sin(phase));
            phase += step;
            if (i % 160 == 159 || i == CHIME_TONE_SAMPLES - 1) {
                size_t n = (size_t)(i % 160) + 1;
                if (gate) {
                    int64_t queued = (int64_t)voice_player_queued() * 1000000 / VOICE_PLAYER_RATE;
                    muse_vc_gate_playout(gate, buf, n, esp_timer_get_time(),
                                         queued + CALL_SPEAKER_LAG_US);
                }
                voice_player_write(buf, n);
            }
        }
    }
    if (own_player) {
        voice_player_end();
        int64_t deadline = esp_timer_get_time() + 700000;
        while (!voice_player_wait(10) && esp_timer_get_time() < deadline) {
        }
    }
}

// wake_detected_us is the esp_timer time the wake phrase was heard
// for a wake-started call (v20's fast path), 0 for a tap-started one.
// A wake call is entered within milliseconds of the detection, so the
// chime below - at call entry, as v19 put it - sounds at the detection
// instant for practical purposes, while admission completes behind
// it; a tap call keeps v19's behaviour exactly.
static void run_call_impl(int64_t wake_detected_us) {
    static int16_t chunk[CAPTURE_CHUNK];
    static int16_t pcm[REPLY_CHUNK];
    char text[96] = "";
    muse_vc_gate_t gate;
    bool duplex = call_full_duplex();
    muse_vc_gate_init(&gate, duplex, CALL_TAIL_US);
    bool live = false, hung_up = false, ended = false;
    int64_t give_up = 0;
    activity_raise(ACTIVITY_CALL);
    led_status_set_call_icon(true);
    voice_player_stop();
    led_status_set_level(0);
    led_status_set_voice(LED_VOICE_THINKING);
    if (voice_board_mic_start() != ESP_OK) {
        fail("microphone unavailable");
        led_status_set_call_icon(false);
        activity_clear(ACTIVITY_CALL);
        return;
    }
    voice_player_begin();
    muse_voice_call_start();
    if (wake_detected_us) {
        ESP_LOGI(TAG, "wake latency: admission started %lld ms after detection",
                 (esp_timer_get_time() - wake_detected_us) / 1000);
    }
    ESP_LOGI(TAG, "call: started (%s duplex)", duplex ? "full" : "half");
    call_chime(true, &gate, false);
    while (!ended) {
        int peak = 0;
        size_t got = voice_board_mic_read(chunk, CAPTURE_CHUNK, &peak);
        int64_t now = esp_timer_get_time();
        // The mute switch is read after the capture and before anything is
        // handed on: a chunk captured while the switch went on is dropped.
        bool muted = voice_board_muted();
        if (got && !muted && live && !hung_up && muse_vc_gate_mic_open(&gate, now)) {
            muse_voice_call_audio(chunk, got);
            led_status_set_level(peak / 12000.0f);
        }
        muse_voice_call_ev_t ev;
        while (!ended && (ev = muse_voice_call_event(text, sizeof(text))) != MUSE_VOICE_CALL_EV_NONE) {
            switch (ev) {
            case MUSE_VOICE_CALL_EV_READY:
                live = true;
                if (wake_detected_us) {
                    ESP_LOGI(TAG, "wake latency: live %lld ms after detection",
                             (esp_timer_get_time() - wake_detected_us) / 1000);
                    wake_detected_us = 0;   // log once
                }
                led_status_set_voice(LED_VOICE_LISTENING);
                break;
            case MUSE_VOICE_CALL_EV_ENDED:
                ESP_LOGI(TAG, "call: %s", text);
                ended = true;
                break;
            default:
                break;
            }
        }
        if (ended) break;
        bool press = false;
        voice_evt_t evt;
        while (xQueueReceive(s_events, &evt, 0) == pdTRUE) press |= evt == EVT_PRESS;
        if (!hung_up && (press || muted || !got)) {
            ESP_LOGI(TAG, "call: hanging up (%s)", press ? "press" : muted ? "muted" : "microphone failed");
            hung_up = true;
            give_up = now + CALL_END_WAIT_US;
            muse_voice_call_end();
            voice_player_stop();
            call_chime(false, NULL, true);
            led_status_set_voice(LED_VOICE_THINKING);
        }
        if (hung_up) {
            if (now > give_up) {
                ESP_LOGW(TAG, "call: no word that it ended");
                break;
            }
            if (!got) vTaskDelay(pdMS_TO_TICKS(20));   // no mic to pace the loop
            continue;
        }
        // Move what has arrived to the player (up to 200 ms per pass).
        for (int i = 0; i < 10; i++) {
            bool flushed = false;
            size_t n = muse_voice_call_read(pcm, REPLY_CHUNK, &flushed);
            if (flushed) {
                // The caller barged in: drop what the player still holds.
                voice_player_begin();
                muse_vc_gate_flush(&gate, now);
            }
            if (!n) break;
            int64_t queued = (int64_t)voice_player_queued() * 1000000 / VOICE_PLAYER_RATE;
            muse_vc_gate_playout(&gate, pcm, n, now, queued + CALL_SPEAKER_LAG_US);
            voice_player_write(pcm, n);
        }
        if (live) {
            led_status_set_voice(muse_vc_gate_mic_open(&gate, now) ? LED_VOICE_LISTENING : LED_VOICE_SPEAKING);
        }
    }
    voice_board_mic_stop();
    led_status_set_level(0);
    if (ended && !hung_up) {
        // Muse ended it (a goodbye, say): let what it already said finish.
        bool flushed;
        size_t n;
        while ((n = muse_voice_call_read(pcm, REPLY_CHUNK, &flushed)) > 0) voice_player_write(pcm, n);
        voice_player_end();
        while (!voice_player_wait(0)) {
            if (pressed_again(pdMS_TO_TICKS(40))) break;
        }
    }
    voice_player_stop();
    // Muse ended it (or it never connected): the falling chime once
    // the last of the speech has played out. A press hangup already
    // chimed at the tap.
    if (!hung_up) call_chime(false, NULL, true);
    led_status_set_voice(LED_VOICE_IDLE);
    led_status_set_call_icon(false);
    activity_clear(ACTIVITY_CALL);
}

// A tap-started call: no wake detection time. This is the entry point
// run_turn's tap path (and the host harness) calls.
static void run_call(void) {
    run_call_impl(0);
}
#endif

// Returns true if a new press interrupted the turn.
static bool run_turn(void) {
    voice_player_stop();
    led_status_set_level(0);
    led_status_set_voice(LED_VOICE_LISTENING);
    muse_hatch_turn_begin();
    size_t press_samples = 0;
    size_t samples = record(&press_samples);
    if (press_samples < VOICE_MIC_RATE * CAPTURE_MIN_MS / 1000) {
        muse_hatch_turn_cancel();
        if (!samples) return fail("microphone unavailable");
#if CONFIG_MUSE_REALTIME_VOICE
        // A tap: a call, beside holding for a note.
        if (muse_voice_call_ready()) {
            run_call();
            return false;
        }
#endif
        ESP_LOGI(TAG, "press too short");
        led_status_set_voice(LED_VOICE_IDLE);
        return false;
    }
    ESP_LOGI(TAG, "recorded %.1fs", (double)samples / VOICE_MIC_RATE);
    led_status_set_voice(LED_VOICE_TRANSCRIBING);
    muse_hatch_turn_end();
    return reply();
}

// Runs on the button task. Claims the press only when a turn can run, so the
// button keeps its setup role otherwise.
static bool on_press(bool pressed) {
    if (pressed) {
        if (!atomic_load(&s_ready) || voice_board_muted()) return false;
        voice_hatch_refresh();
        if (!muse_hatch_ready()) return false;
    }
    voice_evt_t evt = pressed ? EVT_PRESS : EVT_RELEASE;
#if CONFIG_SR_MN_EN_MULTINET6_QUANT
    // Tell the wake listener a press is coming, before the voice task
    // can wake on it, so the listener stops reading the mic first.
    if (pressed) atomic_store(&s_press_hint_at, esp_timer_get_time());
#endif
    xQueueSend(s_events, &evt, 0);
    return true;
}

#if CONFIG_SR_MN_EN_MULTINET6_QUANT
// The wake listener's view of the microphone (v20): gated by the
// shared activity state - any activity at all, plus the few seconds'
// re-arm hysteresis activity.c adds - and briefly after a button
// press, whose turn is about to raise ACTIVITY_VOICE itself.
static bool wake_gate(void) {
    if (activity_busy_for_wake()) return true;
    int64_t at = atomic_load(&s_press_hint_at);
    return at && esp_timer_get_time() - at < 3000000;
}

// A wake phrase was heard. Up to v19 this queued a synthetic tap and
// let run_turn() sort it out - which opened a hatch turn, recorded
// the "press", found it too short for a note, cancelled the turn and
// only then admitted the call. Since v20 a call phrase queues the
// dedicated EVT_WAKE and the voice task goes straight to run_call();
// since v21 a note phrase queues EVT_WAKE_NOTE instead and the voice
// task records a note (run_wake_note). The listener unloads the
// detector right after this returns (it owns the mic until then), so
// what starts allocates with the detector's RAM already handed back,
// as v16 arranged. The guards mirror the old tap path's: ready,
// unmuted, no voice activity, session ready.
static void wake_fire(int64_t detected_us, wake_action_t action, int command_id) {
    (void)command_id;   // wake_word.c's detection line already logged it
    if (!atomic_load(&s_ready) || voice_board_muted()
        || activity_source_active(ACTIVITY_VOICE)) return;
    voice_hatch_refresh();
    if (!muse_hatch_ready()) {
        ESP_LOGI(TAG, "wake word heard, but the voice session is not ready");
        return;
    }
    atomic_store(&s_press_hint_at, esp_timer_get_time());
    atomic_store(&s_wake_detected_us, detected_us);
    if (action == WAKE_ACTION_CALL) {
        ESP_LOGI(TAG, "wake word: starting a call (fast path)");
#if CONFIG_MUSE_REALTIME_VOICE
        voice_evt_t evt = EVT_WAKE;
        xQueueSend(s_events, &evt, 0);
#else
        ESP_LOGI(TAG, "wake word: call phrase, but realtime calls are off in this build");
#endif
    } else {
        ESP_LOGI(TAG, "wake word: starting a voice note");
        voice_evt_t evt = EVT_WAKE_NOTE;
        xQueueSend(s_events, &evt, 0);
    }
}

#if CONFIG_MUSE_REALTIME_VOICE
// Handle EVT_WAKE: straight to a call, with none of run_turn()'s
// note machinery. The guards were checked in wake_fire() a moment
// ago; call readiness is re-checked here because a button press may
// have started (and finished) a turn in between.
static void run_wake_call(void) {
    int64_t detected_us = atomic_load(&s_wake_detected_us);
    ESP_LOGI(TAG, "wake latency: call entered %lld ms after detection",
             (esp_timer_get_time() - detected_us) / 1000);
    if (!muse_voice_call_ready()) {
        ESP_LOGI(TAG, "wake word: call not ready, dropping the wake event");
        led_status_set_voice(LED_VOICE_IDLE);
        return;
    }
    run_call_impl(detected_us);
}
#endif

// The wake-note endpoint (v21), per Scott: the note ends after
// 1-2 s of silence following speech - 1.5 s here, one constant to
// tune. If nothing is said at all within the no-speech window the
// note cancels quietly instead of sending dead air. The speech
// threshold sits between the quiet-room peaks the listener logs
// (~350-400) and real speech at the board's gain (thousands).
#define WAKE_NOTE_SILENCE_MS   1500
#define WAKE_NOTE_NO_SPEECH_MS 6000
#define WAKE_NOTE_SPEECH_PEAK  1200

// record() for a wake note: the detector's pre-roll first, then the
// live mic until the endpoint above, the capture limit, or a button
// press (which ends the note early and sends it - the press itself is
// consumed here, its release is dropped by wait_event). Returns true
// when speech was heard; the pre-roll alone does not count, so a bare
// "hey boeing" with nothing after it still cancels.
static bool record_wake_note(size_t *out_samples) {
    static int16_t chunk[CAPTURE_CHUNK];
    size_t samples = 0;
    size_t pre_n = 0;
    const int16_t *pre = wake_word_preroll(&pre_n);
    for (size_t off = 0; off < pre_n;) {
        size_t n = pre_n - off < (size_t)CAPTURE_CHUNK
                       ? pre_n - off
                       : (size_t)CAPTURE_CHUNK;
        muse_hatch_turn_audio(pre + off, n);
        off += n;
        samples += n;
    }
    if (pre_n) {
        ESP_LOGI(TAG, "wake note: %.1fs of pre-roll", (double)pre_n / VOICE_MIC_RATE);
    }
    if (voice_board_mic_start() != ESP_OK) {
        *out_samples = samples;
        return false;
    }
    bool speech = false;
    int64_t start_us = esp_timer_get_time();
    int64_t last_speech_us = start_us;
    while (samples < CAPTURE_MAX) {
        voice_evt_t evt;
        if (xQueueReceive(s_events, &evt, 0) == pdTRUE && evt == EVT_PRESS) {
            ESP_LOGI(TAG, "wake note: ended early by a button press");
            break;
        }
        int peak = 0;
        size_t got = voice_board_mic_read(chunk, CAPTURE_CHUNK, &peak);
        if (!got) break;
        muse_hatch_turn_audio(chunk, got);
        samples += got;
        led_status_set_level(peak / 12000.0f);
        int64_t now = esp_timer_get_time();
        if (peak >= WAKE_NOTE_SPEECH_PEAK) {
            speech = true;
            last_speech_us = now;
        }
        if (speech && now - last_speech_us >= WAKE_NOTE_SILENCE_MS * 1000LL) break;
        if (!speech && now - start_us >= WAKE_NOTE_NO_SPEECH_MS * 1000LL) break;
    }
    voice_board_mic_stop();
    led_status_set_level(0);
    if (samples >= CAPTURE_MAX) ESP_LOGI(TAG, "wake note: capture limit reached");
    *out_samples = samples;
    return speech;
}

// Handle EVT_WAKE_NOTE: run_turn()'s note half with record_wake_note
// in place of the button-driven record(). A press that interrupts the
// spoken reply just cuts it short (the press was already consumed by
// reply()'s pressed_again check).
static void run_wake_note(void) {
    int64_t detected_us = atomic_load(&s_wake_detected_us);
    ESP_LOGI(TAG, "wake latency: note entered %lld ms after detection",
             (esp_timer_get_time() - detected_us) / 1000);
    voice_player_stop();
    led_status_set_level(0);
    led_status_set_voice(LED_VOICE_LISTENING);
    muse_hatch_turn_begin();
    size_t samples = 0;
    if (!record_wake_note(&samples)) {
        muse_hatch_turn_cancel();
        ESP_LOGI(TAG, "wake note: nothing said after the wake phrase; cancelled");
        led_status_set_voice(LED_VOICE_IDLE);
        return;
    }
    ESP_LOGI(TAG, "wake note: recorded %.1fs", (double)samples / VOICE_MIC_RATE);
    led_status_set_voice(LED_VOICE_TRANSCRIBING);
    muse_hatch_turn_end();
    reply();
}
#endif

static void voice_task(void *arg) {
    if (voice_board_init() != ESP_OK || voice_player_init() != ESP_OK) {
        ESP_LOGE(TAG, "audio hardware unavailable; voice chat disabled");
        vTaskSuspend(NULL);
        return;
    }
    voice_board_set_volume(atomic_load(&s_volume));
    atomic_store(&s_ready, true);
    if (xTaskCreate(dial_task, "dial", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "no memory for the dial");
    }
#if CONFIG_SR_MN_EN_MULTINET6_QUANT
    wake_word_start(wake_gate, wake_fire);
#endif
    ESP_LOGI(TAG, "ready");

    for (;;) {
        voice_evt_t evt = wait_event();
#if CONFIG_SR_MN_EN_MULTINET6_QUANT
        // Raised for the whole handling, turns and calls alike: this
        // is what keeps the wake listener unloaded until pure idle.
        activity_raise(ACTIVITY_VOICE);
        if (evt == EVT_WAKE) {
#if CONFIG_MUSE_REALTIME_VOICE
            run_wake_call();
#else
            while (run_turn()) {
            }
#endif
        } else if (evt == EVT_WAKE_NOTE) {
            run_wake_note();
        } else
#endif
        {
            while (run_turn()) {
            }
        }
#if CONFIG_SR_MN_EN_MULTINET6_QUANT
        activity_clear(ACTIVITY_VOICE);
#endif
    }
}

void voice_init(void) {
    s_events = xQueueCreate(8, sizeof(voice_evt_t));
    if (!s_events) {
        ESP_LOGE(TAG, "no memory for voice chat");
        return;
    }
    atomic_store(&s_volume, load_volume());
    voice_hatch_refresh();
    muse_hatch_start();
    // The stack is in PSRAM, so the task must not touch flash (NVS): pairing
    // needs an 8 KB internal block for its TLS task.
    if (xTaskCreateWithCaps(voice_task, "voice", 4096, NULL, 4, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "failed to start voice chat");
        return;
    }
    button_set_press_cb(on_press);
}

cJSON *voice_configure_command(cJSON *params) {
    cJSON *volume = cJSON_GetObjectItem(params, "volume");
    const char *why = NULL;
    if (volume) {
        if (!cJSON_IsNumber(volume) || volume->valueint < 0 || volume->valueint > 100) {
            why = "volume must be 0-100";
        } else {
            if (!store_volume(volume->valueint)) why = "volume could not be stored";
            else if (atomic_load(&s_ready)) apply_volume(volume->valueint);
            else atomic_store(&s_volume, volume->valueint);
        }
    }

    cJSON *result = cJSON_CreateObject();
    if (why) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "code", "invalid_params");
        cJSON_AddStringToObject(error, "message", why);
        cJSON_AddItemToObject(result, "error", error);
        return result;
    }
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddNumberToObject(result, "volume", atomic_load(&s_volume));
    return result;
}
