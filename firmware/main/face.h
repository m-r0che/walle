#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

typedef enum {
    FACE_MOOD_CALM = 0,
    FACE_MOOD_WARM,
    FACE_MOOD_CURIOUS,
    FACE_MOOD_DELIGHTED,
    FACE_MOOD_UNCERTAIN,
    FACE_MOOD_CONCERNED,
    FACE_MOOD_SLEEPY,
    FACE_MOOD_COUNT,
} face_mood_t;

typedef enum {
    FACE_ACTIVITY_IDLE = 0,
    FACE_ACTIVITY_LISTENING,
    FACE_ACTIVITY_THINKING,
    FACE_ACTIVITY_SPEAKING,
    FACE_ACTIVITY_CONFIRM,
    FACE_ACTIVITY_SUCCESS,
    FACE_ACTIVITY_ERROR,
    FACE_ACTIVITY_OFFLINE,
    FACE_ACTIVITY_SLEEPING,
    FACE_ACTIVITY_COUNT,
} face_activity_t;

typedef enum {
    FACE_REACTION_FOCUS = 0,
    FACE_REACTION_REALISE,
    FACE_REACTION_STARTLE,
    FACE_REACTION_TOUCH_HAPPY,
    FACE_REACTION_GLANCE_LEFT,
    FACE_REACTION_GLANCE_RIGHT,
    FACE_REACTION_WINK,
    FACE_REACTION_COUNT,
} face_reaction_t;

typedef enum {
    FACE_INPUT_PTT_START = 0,
    FACE_INPUT_PTT_STOP,
} face_input_event_t;

typedef struct face face_t;
typedef void (*face_input_callback_t)(face_input_event_t event, void *context);

/** Create the local living-face engine and its autonomous animation timer. */
face_t *face_create(lv_obj_t *parent);

void face_set_input_callback(face_t *face, face_input_callback_t callback,
                             void *context);

/** Set the truthful device-owned conversational activity. */
void face_set_activity(face_t *face, face_activity_t activity);

/** Set the slow personality bias. Intensity is clamped to 0.0–1.0. */
void face_set_mood(face_t *face, face_mood_t mood, float intensity);

/** Apply a bounded semantic mood which returns to warm when its TTL expires. */
void face_set_mood_for(face_t *face, face_mood_t mood, float intensity,
                       uint32_t ttl_ms);
face_mood_t face_get_mood(face_t *face);

/** Start a bounded local reaction which eases back into the underlying face. */
void face_react(face_t *face, face_reaction_t reaction, float intensity);

/** Supply the envelope of PCM samples actually entering the codec. */
void face_set_playback_level(face_t *face, float level);
