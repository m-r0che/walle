#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

typedef enum {
    FACE_EMOTION_NEUTRAL = 0,
    FACE_EMOTION_HAPPY,
    FACE_EMOTION_DOUBTFUL,
    FACE_EMOTION_SLEEPY,
    FACE_EMOTION_PEEK,
    FACE_EMOTION_SURPRISED,
    FACE_EMOTION_INTERESTED,
    FACE_EMOTION_DEVIOUS,
    FACE_EMOTION_ANGRY,
    FACE_EMOTION_FURIOUS,
    FACE_EMOTION_SAD,
    FACE_EMOTION_COUNT,
} face_emotion_t;

typedef enum {
    FACE_INTERACTION_IDLE = 0,
    FACE_INTERACTION_LISTENING,
    FACE_INTERACTION_THINKING,
    FACE_INTERACTION_SPEAKING,
    FACE_INTERACTION_CONFIRM,
    FACE_INTERACTION_SUCCESS,
    FACE_INTERACTION_ERROR,
    FACE_INTERACTION_OFFLINE,
    FACE_INTERACTION_SLEEPING,
    FACE_INTERACTION_COUNT,
} face_interaction_t;

typedef enum {
    FACE_INPUT_PTT_START = 0,
    FACE_INPUT_PTT_STOP,
    FACE_INPUT_MUTE_TOGGLE,
} face_input_event_t;

typedef struct face face_t;
typedef void (*face_input_callback_t)(face_input_event_t event, void *context);

face_t *face_create(lv_obj_t *parent);
void face_set_input_callback(face_t *face, face_input_callback_t callback,
                             void *context);
void face_set_emotion(face_t *face, face_emotion_t emotion);
face_emotion_t face_get_emotion(face_t *face);
void face_set_interaction(face_t *face, face_interaction_t interaction);
void face_set_mouth_level(face_t *face, float level);
void face_set_muted(face_t *face, bool muted);
void face_set_output_volume(face_t *face, uint8_t volume_percent);
