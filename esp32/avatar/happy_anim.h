// Copyright (c) Meta Platforms, Inc. and affiliates.

// Generated for the RLCD-4.2 Boeing port. Do not edit.
#pragma once

#include <stdint.h>
#include "sdkconfig.h"

#define HAPPY_ANIM_WIDTH     140
#define HAPPY_ANIM_HEIGHT    147
#define HAPPY_ANIM_FRAMES    9
#define HAPPY_ANIM_TALK_FRAMES 6
#define HAPPY_ANIM_IDLE_TICKS 58
#define HAPPY_ANIM_TALK_TICKS 24
#define HAPPY_ANIM_FRAME_MS  40

#if CONFIG_HOMEHUB_DISPLAY
extern const uint16_t happy_anim_palette[20];
extern const uint8_t happy_anim_frames[HAPPY_ANIM_FRAMES][HAPPY_ANIM_HEIGHT * HAPPY_ANIM_WIDTH];
extern const uint8_t happy_anim_talk_frames[HAPPY_ANIM_TALK_FRAMES][HAPPY_ANIM_HEIGHT * HAPPY_ANIM_WIDTH];
extern const uint8_t happy_anim_idle_map[HAPPY_ANIM_IDLE_TICKS];
extern const uint8_t happy_anim_talk_map[HAPPY_ANIM_TALK_TICKS];
#endif
