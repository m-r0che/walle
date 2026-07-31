#include "face.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define FACE_WIDTH 448
#define FACE_HEIGHT 368
#define FACE_CANVAS_WIDTH 400
#define FACE_CANVAS_HEIGHT 240
#define FACE_CANVAS_X ((FACE_WIDTH - FACE_CANVAS_WIDTH) / 2)
#define FACE_CANVAS_Y 52
#define FACE_IDLE_FRAME_PERIOD_MS 40
#define FACE_ACTIVE_FRAME_PERIOD_MS 33
#define FACE_SLEEPING_FRAME_PERIOD_MS 140
#define FACE_OFFLINE_FRAME_PERIOD_MS 100
#define FACE_DRIFT_PERIOD_MS 60000
#define EYE_POINT_COUNT 17
#define BROW_POINT_COUNT 9
#define MOUTH_POINT_COUNT 13
#define PI_F 3.14159265358979323846f
#define MUTE_CONTROL_X 35
#define MUTE_CONTROL_Y 250
#define MUTE_CONTROL_HIT_RADIUS 25

static const char *TAG = "face";

typedef struct {
    float left_eye_open;
    float right_eye_open;
    float left_eye_scale;
    float right_eye_scale;
    float gaze_x;
    float gaze_y;
    float pupil_scale;
    float left_brow_lift;
    float right_brow_lift;
    float left_brow_angle;
    float right_brow_angle;
    float smile;
    float mouth_open;
    float mouth_width;
    float tilt;
    float energy;
} face_pose_t;

typedef struct {
    lv_point_precise_t points[EYE_POINT_COUNT];
} eye_curve_t;

typedef struct {
    lv_point_precise_t points[BROW_POINT_COUNT];
} brow_curve_t;

typedef struct {
    lv_point_precise_t points[MOUTH_POINT_COUNT];
} mouth_curve_t;

typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} raster_color_t;

struct face {
    lv_obj_t *canvas;
    uint16_t *canvas_buffer;
    lv_timer_t *timer;
    uint32_t timer_period_ms;
    portMUX_TYPE state_lock;

    face_activity_t activity;
    face_mood_t mood;
    float mood_intensity;
    float playback_level;
    bool muted;
    uint8_t output_volume_percent;
    face_input_callback_t input_callback;
    void *input_context;
    bool ptt_pressed;
    bool mute_pressed;

    face_reaction_t requested_reaction;
    float requested_reaction_intensity;
    uint32_t reaction_generation;
    uint32_t rendered_reaction_generation;
    face_reaction_t active_reaction;
    float active_reaction_intensity;
    uint32_t reaction_started_ms;

    face_pose_t pose;
    uint32_t last_pose_ms;
    uint32_t blink_started_ms;
    uint32_t next_blink_ms;
    uint32_t next_saccade_ms;
    float autonomous_gaze_x;
    float autonomous_gaze_y;
    float autonomous_gaze_target_x;
    float autonomous_gaze_target_y;

    int8_t drift_x;
    int8_t drift_y;
    uint8_t drift_phase;
    uint32_t next_drift_ms;

    uint32_t report_started_ms;
    uint32_t report_frames;
    int64_t render_total_us;
    int64_t render_max_us;
};

static float clampf(float value, float minimum, float maximum)
{
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

static float lerpf(float from, float to, float amount)
{
    return from + (to - from) * amount;
}

static float smoothstep(float value)
{
    const float t = clampf(value, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static bool time_reached(uint32_t now, uint32_t target)
{
    return (int32_t)(now - target) >= 0;
}

static uint32_t random_range(uint32_t minimum, uint32_t maximum)
{
    return minimum + (esp_random() % (maximum - minimum + 1));
}

static face_pose_t neutral_pose(void)
{
    return (face_pose_t) {
        .left_eye_open = 0.96f,
        .right_eye_open = 0.96f,
        .left_eye_scale = 1.0f,
        .right_eye_scale = 1.0f,
        .gaze_x = 0.0f,
        .gaze_y = 0.0f,
        .pupil_scale = 1.0f,
        .left_brow_lift = 0.05f,
        .right_brow_lift = 0.05f,
        .left_brow_angle = 0.0f,
        .right_brow_angle = 0.0f,
        .smile = 0.18f,
        .mouth_open = 0.0f,
        .mouth_width = 1.0f,
        .tilt = 0.0f,
        .energy = 0.45f,
    };
}

static face_pose_t activity_pose(face_activity_t activity)
{
    face_pose_t pose = neutral_pose();
    switch (activity) {
    case FACE_ACTIVITY_LISTENING:
        pose.left_eye_open = 1.04f;
        pose.right_eye_open = 1.04f;
        pose.pupil_scale = 1.06f;
        pose.left_brow_lift = 0.30f;
        pose.right_brow_lift = 0.30f;
        pose.smile = 0.18f;
        pose.energy = 0.76f;
        break;
    case FACE_ACTIVITY_THINKING:
        pose.left_eye_open = 0.76f;
        pose.right_eye_open = 0.90f;
        pose.gaze_x = 0.28f;
        pose.gaze_y = -0.32f;
        pose.left_brow_lift = 0.02f;
        pose.right_brow_lift = 0.36f;
        pose.left_brow_angle = -0.16f;
        pose.right_brow_angle = 0.10f;
        pose.smile = -0.04f;
        pose.tilt = -0.018f;
        pose.energy = 0.34f;
        break;
    case FACE_ACTIVITY_SPEAKING:
        pose.left_eye_open = 0.93f;
        pose.right_eye_open = 0.98f;
        pose.smile = 0.32f;
        pose.mouth_width = 0.92f;
        pose.left_brow_lift = 0.16f;
        pose.right_brow_lift = 0.20f;
        pose.energy = 0.88f;
        break;
    case FACE_ACTIVITY_CONFIRM:
        pose.left_eye_open = 1.02f;
        pose.right_eye_open = 1.02f;
        pose.pupil_scale = 0.94f;
        pose.left_brow_lift = 0.42f;
        pose.right_brow_lift = 0.42f;
        pose.smile = 0.02f;
        pose.energy = 0.58f;
        break;
    case FACE_ACTIVITY_SUCCESS:
        pose.left_eye_open = 0.66f;
        pose.right_eye_open = 0.66f;
        pose.left_brow_lift = 0.35f;
        pose.right_brow_lift = 0.35f;
        pose.smile = 0.92f;
        pose.mouth_width = 1.13f;
        pose.energy = 1.0f;
        break;
    case FACE_ACTIVITY_ERROR:
        pose.left_eye_open = 0.68f;
        pose.right_eye_open = 0.72f;
        pose.pupil_scale = 0.84f;
        pose.left_brow_lift = 0.18f;
        pose.right_brow_lift = 0.18f;
        pose.left_brow_angle = 0.34f;
        pose.right_brow_angle = -0.34f;
        pose.smile = -0.48f;
        pose.energy = 0.28f;
        break;
    case FACE_ACTIVITY_OFFLINE:
        pose.left_eye_open = 0.72f;
        pose.right_eye_open = 0.82f;
        pose.pupil_scale = 0.82f;
        pose.gaze_x = -0.15f;
        pose.left_brow_lift = 0.10f;
        pose.right_brow_lift = 0.24f;
        pose.left_brow_angle = 0.22f;
        pose.right_brow_angle = -0.15f;
        pose.smile = -0.18f;
        pose.energy = 0.18f;
        break;
    case FACE_ACTIVITY_SLEEPING:
        pose.left_eye_open = 0.04f;
        pose.right_eye_open = 0.04f;
        pose.pupil_scale = 0.0f;
        pose.left_brow_lift = -0.12f;
        pose.right_brow_lift = -0.12f;
        pose.smile = 0.30f;
        pose.mouth_width = 0.78f;
        pose.energy = 0.08f;
        break;
    case FACE_ACTIVITY_IDLE:
    default:
        break;
    }
    return pose;
}

static void apply_mood(face_pose_t *pose, face_mood_t mood, float amount)
{
    switch (mood) {
    case FACE_MOOD_WARM:
        pose->smile += 0.44f * amount;
        pose->left_brow_lift += 0.10f * amount;
        pose->right_brow_lift += 0.10f * amount;
        pose->pupil_scale += 0.05f * amount;
        break;
    case FACE_MOOD_CURIOUS:
        pose->left_eye_open += 0.07f * amount;
        pose->right_eye_open -= 0.08f * amount;
        pose->left_eye_scale += 0.03f * amount;
        pose->right_brow_lift += 0.34f * amount;
        pose->left_brow_lift += 0.08f * amount;
        pose->gaze_x += 0.18f * amount;
        pose->gaze_y -= 0.12f * amount;
        pose->smile += 0.16f * amount;
        pose->tilt -= 0.025f * amount;
        break;
    case FACE_MOOD_DELIGHTED:
        pose->left_eye_open -= 0.22f * amount;
        pose->right_eye_open -= 0.22f * amount;
        pose->left_brow_lift += 0.30f * amount;
        pose->right_brow_lift += 0.30f * amount;
        pose->smile += 0.72f * amount;
        pose->mouth_width += 0.12f * amount;
        pose->energy += 0.30f * amount;
        break;
    case FACE_MOOD_UNCERTAIN:
        pose->left_eye_open -= 0.18f * amount;
        pose->right_eye_open += 0.04f * amount;
        pose->left_brow_angle += 0.28f * amount;
        pose->right_brow_angle -= 0.20f * amount;
        pose->right_brow_lift += 0.18f * amount;
        pose->smile -= 0.28f * amount;
        pose->tilt += 0.018f * amount;
        break;
    case FACE_MOOD_CONCERNED:
        pose->left_eye_open -= 0.12f * amount;
        pose->right_eye_open -= 0.12f * amount;
        pose->left_brow_angle += 0.40f * amount;
        pose->right_brow_angle -= 0.40f * amount;
        pose->smile -= 0.42f * amount;
        pose->pupil_scale -= 0.10f * amount;
        break;
    case FACE_MOOD_SLEEPY:
        pose->left_eye_open -= 0.58f * amount;
        pose->right_eye_open -= 0.55f * amount;
        pose->left_brow_lift -= 0.18f * amount;
        pose->right_brow_lift -= 0.18f * amount;
        pose->smile += 0.12f * amount;
        pose->energy -= 0.30f * amount;
        break;
    case FACE_MOOD_CALM:
    default:
        break;
    }
}

static uint32_t reaction_duration_ms(face_reaction_t reaction)
{
    switch (reaction) {
    case FACE_REACTION_FOCUS:
        return 520;
    case FACE_REACTION_REALISE:
        return 900;
    case FACE_REACTION_STARTLE:
        return 620;
    case FACE_REACTION_TOUCH_HAPPY:
        return 1050;
    case FACE_REACTION_GLANCE_LEFT:
    case FACE_REACTION_GLANCE_RIGHT:
        return 1250;
    case FACE_REACTION_WINK:
        return 720;
    default:
        return 700;
    }
}

static float reaction_envelope(uint32_t elapsed, uint32_t duration)
{
    if (elapsed >= duration) {
        return 0.0f;
    }
    const float progress = (float)elapsed / (float)duration;
    if (progress < 0.18f) {
        return smoothstep(progress / 0.18f);
    }
    return 1.0f - smoothstep((progress - 0.55f) / 0.45f);
}

static void apply_reaction(face_pose_t *pose, face_reaction_t reaction,
                           float amount)
{
    switch (reaction) {
    case FACE_REACTION_FOCUS:
        pose->left_eye_open += 0.14f * amount;
        pose->right_eye_open += 0.14f * amount;
        pose->pupil_scale += 0.08f * amount;
        pose->gaze_x *= 1.0f - amount;
        pose->gaze_y *= 1.0f - amount;
        pose->left_brow_lift += 0.28f * amount;
        pose->right_brow_lift += 0.28f * amount;
        break;
    case FACE_REACTION_REALISE:
        pose->left_eye_open += 0.30f * amount;
        pose->right_eye_open += 0.30f * amount;
        pose->pupil_scale -= 0.20f * amount;
        pose->left_brow_lift += 0.50f * amount;
        pose->right_brow_lift += 0.50f * amount;
        pose->smile += 0.54f * amount;
        pose->mouth_open += 0.24f * amount;
        pose->energy += 0.25f * amount;
        break;
    case FACE_REACTION_STARTLE:
        pose->left_eye_open += 0.42f * amount;
        pose->right_eye_open += 0.42f * amount;
        pose->left_eye_scale += 0.06f * amount;
        pose->right_eye_scale += 0.06f * amount;
        pose->pupil_scale -= 0.32f * amount;
        pose->left_brow_lift += 0.64f * amount;
        pose->right_brow_lift += 0.64f * amount;
        pose->smile *= 1.0f - amount;
        pose->mouth_open += 0.62f * amount;
        break;
    case FACE_REACTION_TOUCH_HAPPY:
        pose->left_eye_open -= 0.48f * amount;
        pose->right_eye_open -= 0.48f * amount;
        pose->left_brow_lift += 0.16f * amount;
        pose->right_brow_lift += 0.16f * amount;
        pose->smile += 0.72f * amount;
        pose->tilt -= 0.035f * amount;
        break;
    case FACE_REACTION_GLANCE_LEFT:
        pose->gaze_x = lerpf(pose->gaze_x, -1.0f, amount);
        pose->left_eye_open += 0.06f * amount;
        pose->right_brow_lift += 0.18f * amount;
        break;
    case FACE_REACTION_GLANCE_RIGHT:
        pose->gaze_x = lerpf(pose->gaze_x, 1.0f, amount);
        pose->right_eye_open += 0.06f * amount;
        pose->left_brow_lift += 0.18f * amount;
        break;
    case FACE_REACTION_WINK:
        pose->left_eye_open *= 1.0f - amount;
        pose->right_brow_lift += 0.20f * amount;
        pose->smile += 0.45f * amount;
        pose->tilt += 0.022f * amount;
        break;
    default:
        break;
    }
}

static void clamp_pose(face_pose_t *pose)
{
    pose->left_eye_open = clampf(pose->left_eye_open, 0.02f, 1.25f);
    pose->right_eye_open = clampf(pose->right_eye_open, 0.02f, 1.25f);
    pose->left_eye_scale = clampf(pose->left_eye_scale, 0.86f, 1.10f);
    pose->right_eye_scale = clampf(pose->right_eye_scale, 0.86f, 1.10f);
    pose->gaze_x = clampf(pose->gaze_x, -1.0f, 1.0f);
    pose->gaze_y = clampf(pose->gaze_y, -1.0f, 1.0f);
    pose->pupil_scale = clampf(pose->pupil_scale, 0.42f, 1.18f);
    pose->left_brow_lift = clampf(pose->left_brow_lift, -0.25f, 0.90f);
    pose->right_brow_lift = clampf(pose->right_brow_lift, -0.25f, 0.90f);
    pose->left_brow_angle = clampf(pose->left_brow_angle, -0.65f, 0.65f);
    pose->right_brow_angle = clampf(pose->right_brow_angle, -0.65f, 0.65f);
    pose->smile = clampf(pose->smile, -1.0f, 1.0f);
    pose->mouth_open = clampf(pose->mouth_open, 0.0f, 1.0f);
    pose->mouth_width = clampf(pose->mouth_width, 0.70f, 1.18f);
    pose->tilt = clampf(pose->tilt, -0.06f, 0.06f);
    pose->energy = clampf(pose->energy, 0.05f, 1.0f);
}

static void update_autonomous_gaze(face_t *face, uint32_t now,
                                   face_activity_t activity, float energy)
{
    if (activity == FACE_ACTIVITY_SLEEPING) {
        face->autonomous_gaze_target_x = 0.0f;
        face->autonomous_gaze_target_y = 0.0f;
    } else if (time_reached(now, face->next_saccade_ms)) {
        const float spread = 0.10f + energy * 0.22f;
        face->autonomous_gaze_target_x =
            ((float)((int32_t)random_range(0, 200) - 100) / 100.0f) * spread;
        face->autonomous_gaze_target_y =
            ((float)((int32_t)random_range(0, 160) - 80) / 100.0f) * spread;
        face->next_saccade_ms = now + random_range(650, 2600);
    }

    face->autonomous_gaze_x +=
        (face->autonomous_gaze_target_x - face->autonomous_gaze_x) * 0.34f;
    face->autonomous_gaze_y +=
        (face->autonomous_gaze_target_y - face->autonomous_gaze_y) * 0.28f;
}

static void blink_openness(face_t *face, uint32_t now, float energy,
                           float *left, float *right)
{
    *left = 1.0f;
    *right = 1.0f;
    if (face->blink_started_ms == 0) {
        if (!time_reached(now, face->next_blink_ms)) {
            return;
        }
        face->blink_started_ms = now;
    }

    const uint32_t elapsed = now - face->blink_started_ms;
    const uint32_t right_delay = 24;
    const uint32_t left_elapsed = elapsed;
    const uint32_t right_elapsed = elapsed > right_delay
        ? elapsed - right_delay : 0;

    if (left_elapsed < 85) {
        *left = 1.0f - smoothstep((float)left_elapsed / 85.0f);
    } else if (left_elapsed < 245) {
        *left = smoothstep((float)(left_elapsed - 85) / 160.0f);
    }
    if (elapsed > right_delay && right_elapsed < 85) {
        *right = 1.0f - smoothstep((float)right_elapsed / 85.0f);
    } else if (elapsed > right_delay && right_elapsed < 245) {
        *right = smoothstep((float)(right_elapsed - 85) / 160.0f);
    }

    if (elapsed >= 285) {
        face->blink_started_ms = 0;
        const uint32_t minimum = (uint32_t)lerpf(5200.0f, 2800.0f, energy);
        const uint32_t maximum = (uint32_t)lerpf(7600.0f, 4800.0f, energy);
        face->next_blink_ms = now + random_range(minimum, maximum);
        *left = 1.0f;
        *right = 1.0f;
    }
}

static face_pose_t compose_target(face_t *face, uint32_t now,
                                  face_activity_t activity,
                                  face_mood_t mood, float mood_intensity,
                                  float playback_level,
                                  face_reaction_t requested_reaction,
                                  float requested_reaction_intensity,
                                  uint32_t reaction_generation,
                                  float *reaction_amount_out)
{
    face_pose_t target = activity_pose(activity);
    float mood_weight = mood_intensity;
    switch (activity) {
    case FACE_ACTIVITY_CONFIRM:
    case FACE_ACTIVITY_SUCCESS:
    case FACE_ACTIVITY_ERROR:
    case FACE_ACTIVITY_OFFLINE:
    case FACE_ACTIVITY_SLEEPING:
        mood_weight = 0.0f;
        break;
    case FACE_ACTIVITY_LISTENING:
    case FACE_ACTIVITY_THINKING:
        mood_weight *= 0.45f;
        break;
    case FACE_ACTIVITY_SPEAKING:
        mood_weight *= 0.72f;
        break;
    case FACE_ACTIVITY_IDLE:
    default:
        break;
    }
    apply_mood(&target, mood, mood_weight);

    update_autonomous_gaze(face, now, activity, target.energy);
    if (activity == FACE_ACTIVITY_IDLE || activity == FACE_ACTIVITY_SPEAKING) {
        target.gaze_x += face->autonomous_gaze_x;
        target.gaze_y += face->autonomous_gaze_y;
    }

    if (face->rendered_reaction_generation != reaction_generation) {
        face->rendered_reaction_generation = reaction_generation;
        face->active_reaction = requested_reaction;
        face->active_reaction_intensity = requested_reaction_intensity;
        face->reaction_started_ms = now;
    }

    float reaction_amount = 0.0f;
    if (face->reaction_started_ms != 0) {
        const uint32_t duration = reaction_duration_ms(face->active_reaction);
        const uint32_t elapsed = now - face->reaction_started_ms;
        reaction_amount = reaction_envelope(elapsed, duration)
            * face->active_reaction_intensity;
        if (elapsed >= duration) {
            face->reaction_started_ms = 0;
        } else {
            apply_reaction(&target, face->active_reaction, reaction_amount);
        }
    }

    float left_blink = 1.0f;
    float right_blink = 1.0f;
    const bool reaction_owns_eyes = reaction_amount > 0.05f
        && (face->active_reaction == FACE_REACTION_STARTLE
            || face->active_reaction == FACE_REACTION_REALISE
            || face->active_reaction == FACE_REACTION_WINK);
    if (activity != FACE_ACTIVITY_SLEEPING && !reaction_owns_eyes) {
        blink_openness(face, now, target.energy, &left_blink, &right_blink);
    }
    target.left_eye_open *= left_blink;
    target.right_eye_open *= right_blink;

    if (activity == FACE_ACTIVITY_SPEAKING) {
        const float eased_level = smoothstep(playback_level);
        target.mouth_open = 0.06f + eased_level * 0.94f;
        target.mouth_width = 0.86f + eased_level * 0.22f;
    }

    clamp_pose(&target);
    *reaction_amount_out = reaction_amount;
    return target;
}

static void update_pose(face_t *face, const face_pose_t *target, uint32_t now)
{
    float elapsed = face->last_pose_ms == 0
        ? 0.07f : (float)(now - face->last_pose_ms) / 1000.0f;
    elapsed = clampf(elapsed, 0.001f, 0.25f);
    face->last_pose_ms = now;

    const float expression_alpha = 1.0f - expf(-elapsed / 0.085f);
    const float gaze_alpha = 1.0f - expf(-elapsed / 0.065f);
    const float mouth_tau = target->mouth_open > face->pose.mouth_open
        ? 0.035f : 0.075f;
    const float mouth_alpha = 1.0f - expf(-elapsed / mouth_tau);

#define EASE_FIELD(field) \
    face->pose.field = lerpf(face->pose.field, target->field, expression_alpha)
    EASE_FIELD(left_eye_open);
    EASE_FIELD(right_eye_open);
    EASE_FIELD(left_eye_scale);
    EASE_FIELD(right_eye_scale);
    EASE_FIELD(pupil_scale);
    EASE_FIELD(left_brow_lift);
    EASE_FIELD(right_brow_lift);
    EASE_FIELD(left_brow_angle);
    EASE_FIELD(right_brow_angle);
    EASE_FIELD(smile);
    EASE_FIELD(mouth_width);
    EASE_FIELD(tilt);
    EASE_FIELD(energy);
#undef EASE_FIELD
    face->pose.gaze_x = lerpf(face->pose.gaze_x, target->gaze_x, gaze_alpha);
    face->pose.gaze_y = lerpf(face->pose.gaze_y, target->gaze_y, gaze_alpha);
    face->pose.mouth_open = lerpf(face->pose.mouth_open,
                                  target->mouth_open, mouth_alpha);
}

static void update_position_drift(face_t *face, uint32_t now)
{
    static const int8_t positions[][2] = {
        {0, 0}, {2, -2}, {-2, -1}, {-2, 2}, {2, 2}, {1, -1}, {-1, 1},
    };
    if (!time_reached(now, face->next_drift_ms)) {
        return;
    }
    face->drift_phase = (face->drift_phase + 1)
        % (sizeof(positions) / sizeof(positions[0]));
    face->drift_x = positions[face->drift_phase][0];
    face->drift_y = positions[face->drift_phase][1];
    face->next_drift_ms = now + FACE_DRIFT_PERIOD_MS;
    lv_obj_set_pos(face->canvas, FACE_CANVAS_X + face->drift_x,
                   FACE_CANVAS_Y + face->drift_y);
}

static uint16_t pack_dimmed_color(raster_color_t color, uint8_t brightness)
{
    const uint16_t red = ((color.red >> 3) * brightness + 127) / 255;
    const uint16_t green = ((color.green >> 2) * brightness + 127) / 255;
    const uint16_t blue = ((color.blue >> 3) * brightness + 127) / 255;
    return (red << 11) | (green << 5) | blue;
}

static uint16_t *canvas_pixel_at(face_t *face, int32_t x, int32_t y)
{
    x -= FACE_CANVAS_X;
    y -= FACE_CANVAS_Y;
    if ((uint32_t)x >= FACE_CANVAS_WIDTH || (uint32_t)y >= FACE_CANVAS_HEIGHT) {
        return NULL;
    }
    return &face->canvas_buffer[y * FACE_CANVAS_WIDTH + x];
}

static void brighten_pixel(face_t *face, int32_t x, int32_t y,
                           uint16_t source)
{
    uint16_t *pixel = canvas_pixel_at(face, x, y);
    if (pixel == NULL) {
        return;
    }
    const uint16_t destination = *pixel;
    const uint16_t red = LV_MAX((source >> 11) & 0x1f,
                                (destination >> 11) & 0x1f);
    const uint16_t green = LV_MAX((source >> 5) & 0x3f,
                                  (destination >> 5) & 0x3f);
    const uint16_t blue = LV_MAX(source & 0x1f, destination & 0x1f);
    *pixel = (red << 11) | (green << 5) | blue;
}

static void draw_disc(face_t *face, int32_t center_x, int32_t center_y,
                      int32_t radius, uint16_t color)
{
    const int32_t radius_squared = radius * radius;
    for (int32_t y = -radius; y <= radius; y++) {
        for (int32_t x = -radius; x <= radius; x++) {
            if (x * x + y * y <= radius_squared) {
                brighten_pixel(face, center_x + x, center_y + y, color);
            }
        }
    }
}

static void clear_disc(face_t *face, int32_t center_x, int32_t center_y,
                       int32_t radius)
{
    const int32_t radius_squared = radius * radius;
    for (int32_t y = -radius; y <= radius; y++) {
        for (int32_t x = -radius; x <= radius; x++) {
            if (x * x + y * y <= radius_squared) {
                uint16_t *pixel = canvas_pixel_at(face, center_x + x,
                                                   center_y + y);
                if (pixel != NULL) {
                    *pixel = 0;
                }
            }
        }
    }
}

static void draw_raster_segment(face_t *face,
                                const lv_point_precise_t *start,
                                const lv_point_precise_t *end,
                                int32_t radius, uint16_t color)
{
    const int32_t x0 = start->x;
    const int32_t y0 = start->y;
    const int32_t dx = end->x - x0;
    const int32_t dy = end->y - y0;
    const int32_t steps = LV_MAX(LV_ABS(dx), LV_ABS(dy));
    const int32_t stride = LV_MAX(1, radius / 2);
    if (steps == 0) {
        draw_disc(face, x0, y0, radius, color);
        return;
    }
    for (int32_t step = 0; step <= steps; step += stride) {
        draw_disc(face, x0 + dx * step / steps, y0 + dy * step / steps,
                  radius, color);
    }
    draw_disc(face, end->x, end->y, radius, color);
}

static void draw_glow_curve(face_t *face,
                            const lv_point_precise_t *points,
                            size_t point_count, bool strong)
{
    const int32_t radii[] = {strong ? 10 : 7, strong ? 6 : 4,
                             strong ? 3 : 2};
    const uint8_t brightness[] = {34, 102, 255};
    const raster_color_t deep_cyan = {0x00, 0x67, 0x75};
    const raster_color_t ice_cyan = {0xb9, 0xff, 0xff};
    for (size_t pass = 0; pass < 3; pass++) {
        const raster_color_t source = pass == 2 ? ice_cyan : deep_cyan;
        const uint16_t color = pack_dimmed_color(source, brightness[pass]);
        for (size_t index = 1; index < point_count; index++) {
            draw_raster_segment(face, &points[index - 1], &points[index],
                                radii[pass], color);
        }
    }
}

static void transform_point(lv_point_precise_t *point, float tilt,
                            float vertical_offset)
{
    const float center_x = FACE_WIDTH / 2.0f;
    const float center_y = 170.0f;
    const float x = point->x - center_x;
    const float y = point->y + vertical_offset - center_y;
    const float sine = sinf(tilt);
    const float cosine = cosf(tilt);
    point->x = (lv_value_precise_t)(center_x + x * cosine - y * sine);
    point->y = (lv_value_precise_t)(center_y + x * sine + y * cosine);
}

static void transform_points(lv_point_precise_t *points, size_t count,
                             float tilt, float vertical_offset)
{
    for (size_t index = 0; index < count; index++) {
        transform_point(&points[index], tilt, vertical_offset);
    }
}

static void build_eye_curves(eye_curve_t *top, eye_curve_t *bottom,
                             float center_x, float center_y,
                             float radius, float openness)
{
    const float half_width = radius;
    const float half_height = radius * openness;
    for (size_t index = 0; index < EYE_POINT_COUNT; index++) {
        const float t = (float)index / (EYE_POINT_COUNT - 1);
        const float upper_angle = PI_F - t * PI_F;
        const float lower_angle = PI_F + t * PI_F;
        top->points[index].x = (lv_value_precise_t)(
            center_x + cosf(upper_angle) * half_width);
        top->points[index].y = (lv_value_precise_t)(
            center_y - sinf(upper_angle) * half_height);
        bottom->points[index].x = (lv_value_precise_t)(
            center_x + cosf(lower_angle) * half_width);
        bottom->points[index].y = (lv_value_precise_t)(
            center_y - sinf(lower_angle) * half_height);
    }
}

static void draw_eye(face_t *face, float center_x, float center_y,
                     float openness, float scale, float gaze_x,
                     float gaze_y, float pupil_scale, float tilt,
                     float vertical_offset)
{
    eye_curve_t top;
    eye_curve_t bottom;
    const float radius = 49.0f * scale;
    build_eye_curves(&top, &bottom, center_x, center_y, radius,
                     LV_MAX(0.025f, openness));
    transform_points(top.points, EYE_POINT_COUNT, tilt, vertical_offset);
    transform_points(bottom.points, EYE_POINT_COUNT, tilt, vertical_offset);
    draw_glow_curve(face, top.points, EYE_POINT_COUNT, true);
    draw_glow_curve(face, bottom.points, EYE_POINT_COUNT, true);

    if (openness < 0.16f || pupil_scale <= 0.0f) {
        return;
    }
    lv_point_precise_t pupil_center = {
        .x = (lv_value_precise_t)(center_x + gaze_x * 24.0f),
        .y = (lv_value_precise_t)(center_y
                                  + gaze_y * 16.0f
                                  * clampf(openness, 0.3f, 1.0f)),
    };
    transform_point(&pupil_center, tilt, vertical_offset);
    const int32_t outer_radius = LV_MAX(7, (int32_t)(17.0f * pupil_scale));
    const int32_t iris_radius = LV_MAX(5, (int32_t)(13.0f * pupil_scale));
    const int32_t pupil_radius = LV_MAX(3, (int32_t)(7.0f * pupil_scale));
    draw_disc(face, pupil_center.x, pupil_center.y, outer_radius,
              pack_dimmed_color((raster_color_t){0x00, 0x67, 0x75}, 170));
    draw_disc(face, pupil_center.x, pupil_center.y, iris_radius,
              pack_dimmed_color((raster_color_t){0xb9, 0xff, 0xff}, 195));
    clear_disc(face, pupil_center.x, pupil_center.y, pupil_radius);
    if (pupil_scale > 0.62f) {
        draw_disc(face, pupil_center.x - 4, pupil_center.y - 5, 2,
                  pack_dimmed_color((raster_color_t){0xff, 0xff, 0xff}, 255));
    }
}

static void build_brow(brow_curve_t *brow, float center_x, float lift,
                       float angle)
{
    for (size_t index = 0; index < BROW_POINT_COUNT; index++) {
        const float t = (float)index / (BROW_POINT_COUNT - 1);
        const float x_normal = t * 2.0f - 1.0f;
        const float arch = sinf(t * PI_F);
        brow->points[index].x = (lv_value_precise_t)(center_x
                                                     + x_normal * 42.0f);
        brow->points[index].y = (lv_value_precise_t)(
            86.0f - lift * 18.0f + angle * x_normal * 17.0f
            - arch * 5.0f);
    }
}

static void draw_brow(face_t *face, float center_x, float lift, float angle,
                      float tilt, float vertical_offset)
{
    brow_curve_t brow;
    build_brow(&brow, center_x, lift, angle);
    transform_points(brow.points, BROW_POINT_COUNT, tilt, vertical_offset);
    draw_glow_curve(face, brow.points, BROW_POINT_COUNT, false);
}

static void build_mouth_curve(mouth_curve_t *mouth, float center_y,
                              float smile, float width)
{
    for (size_t index = 0; index < MOUTH_POINT_COUNT; index++) {
        const float t = (float)index / (MOUTH_POINT_COUNT - 1);
        const float x_normal = t * 2.0f - 1.0f;
        mouth->points[index].x = (lv_value_precise_t)(
            FACE_WIDTH / 2.0f + x_normal * 52.0f * width);
        mouth->points[index].y = (lv_value_precise_t)(
            center_y + sinf(t * PI_F) * 32.0f * smile);
    }
}

static void draw_mouth(face_t *face, const face_pose_t *pose,
                       float vertical_offset)
{
    mouth_curve_t center;
    build_mouth_curve(&center, 260.0f, pose->smile, pose->mouth_width);
    if (pose->mouth_open < 0.045f) {
        transform_points(center.points, MOUTH_POINT_COUNT, pose->tilt,
                         vertical_offset);
        draw_glow_curve(face, center.points, MOUTH_POINT_COUNT, true);
        return;
    }

    mouth_curve_t upper = center;
    mouth_curve_t lower = center;
    const float opening = 3.0f + smoothstep(pose->mouth_open) * 18.0f;
    for (size_t index = 0; index < MOUTH_POINT_COUNT; index++) {
        const float t = (float)index / (MOUTH_POINT_COUNT - 1);
        const float arch = sinf(t * PI_F);
        upper.points[index].y -= (lv_value_precise_t)(arch * opening * 0.65f);
        lower.points[index].y += (lv_value_precise_t)(arch * opening);
    }
    transform_points(upper.points, MOUTH_POINT_COUNT, pose->tilt,
                     vertical_offset);
    transform_points(lower.points, MOUTH_POINT_COUNT, pose->tilt,
                     vertical_offset);
    draw_glow_curve(face, upper.points, MOUTH_POINT_COUNT, true);
    draw_glow_curve(face, lower.points, MOUTH_POINT_COUNT, true);
}

static void draw_realisation_spark(face_t *face, float amount)
{
    if (amount < 0.52f) {
        return;
    }
    const float scale = smoothstep((amount - 0.52f) / 0.48f);
    const int32_t radius = 5 + (int32_t)(scale * 7.0f);
    const lv_point_precise_t horizontal[] = {
        {397 - radius, 90}, {397 + radius, 90},
    };
    const lv_point_precise_t vertical[] = {
        {397, 90 - radius}, {397, 90 + radius},
    };
    draw_glow_curve(face, horizontal, 2, false);
    draw_glow_curve(face, vertical, 2, false);
}

static void draw_mute_control(face_t *face, bool muted)
{
    lv_point_precise_t ring[13];
    for (size_t index = 0; index < 13; index++) {
        const float angle = (float)index * 2.0f * PI_F / 12.0f;
        ring[index].x = MUTE_CONTROL_X
            + (lv_value_precise_t)(cosf(angle) * 7.0f);
        ring[index].y = MUTE_CONTROL_Y
            + (lv_value_precise_t)(sinf(angle) * 7.0f);
    }
    if (!muted) {
        draw_glow_curve(face, ring, 13, false);
        return;
    }

    const raster_color_t coral = {0xff, 0x72, 0x62};
    const uint16_t color = pack_dimmed_color(coral, 255);
    for (size_t index = 1; index < 13; index++) {
        draw_raster_segment(face, &ring[index - 1], &ring[index], 2, color);
    }
    const lv_point_precise_t slash[] = {
        {MUTE_CONTROL_X - 7, MUTE_CONTROL_Y - 7},
        {MUTE_CONTROL_X + 7, MUTE_CONTROL_Y + 7},
    };
    draw_raster_segment(face, &slash[0], &slash[1], 2, color);
}

static void render_face(face_t *face, uint32_t now)
{
    const int64_t started_us = esp_timer_get_time();
    face_activity_t activity;
    face_mood_t mood;
    float mood_intensity;
    float playback_level;
    bool muted;
    face_reaction_t requested_reaction;
    float requested_reaction_intensity;
    uint32_t reaction_generation;
    portENTER_CRITICAL(&face->state_lock);
    activity = face->activity;
    mood = face->mood;
    mood_intensity = face->mood_intensity;
    playback_level = face->playback_level;
    muted = face->muted;
    requested_reaction = face->requested_reaction;
    requested_reaction_intensity = face->requested_reaction_intensity;
    reaction_generation = face->reaction_generation;
    portEXIT_CRITICAL(&face->state_lock);

    float reaction_amount = 0.0f;
    const face_pose_t target = compose_target(
        face, now, activity, mood, mood_intensity, playback_level,
        requested_reaction, requested_reaction_intensity,
        reaction_generation, &reaction_amount);
    update_pose(face, &target, now);

    memset(face->canvas_buffer, 0,
           FACE_CANVAS_WIDTH * FACE_CANVAS_HEIGHT
           * sizeof(*face->canvas_buffer));

    const float seconds = (float)now / 1000.0f;
    const float breath = sinf(seconds * (2.0f * PI_F / 4.2f))
        * (0.8f + face->pose.energy * 1.8f);
    draw_brow(face, 132.0f, face->pose.left_brow_lift,
              face->pose.left_brow_angle, face->pose.tilt, breath);
    draw_brow(face, 316.0f, face->pose.right_brow_lift,
              face->pose.right_brow_angle, face->pose.tilt, breath);
    draw_eye(face, 132.0f, 154.0f, face->pose.left_eye_open,
             face->pose.left_eye_scale, face->pose.gaze_x,
             face->pose.gaze_y, face->pose.pupil_scale,
             face->pose.tilt, breath);
    draw_eye(face, 316.0f, 154.0f, face->pose.right_eye_open,
             face->pose.right_eye_scale, face->pose.gaze_x,
             face->pose.gaze_y, face->pose.pupil_scale,
             face->pose.tilt, breath);
    draw_mouth(face, &face->pose, breath * 0.65f);
    if (face->active_reaction == FACE_REACTION_REALISE) {
        draw_realisation_spark(face, reaction_amount);
    }
    draw_mute_control(face, muted);

    lv_draw_buf_flush_cache(lv_canvas_get_draw_buf(face->canvas), NULL);
    lv_obj_invalidate(face->canvas);

    const int64_t render_us = esp_timer_get_time() - started_us;
    face->report_frames++;
    face->render_total_us += render_us;
    if (render_us > face->render_max_us) {
        face->render_max_us = render_us;
    }
    const uint32_t report_elapsed = now - face->report_started_ms;
    if (report_elapsed >= 5000) {
        const float fps = (float)face->report_frames * 1000.0f
            / report_elapsed;
        const int64_t average_us = face->report_frames == 0
            ? 0 : face->render_total_us / face->report_frames;
        ESP_LOGI(TAG,
                 "fps=%.1f render_avg=%lldus render_max=%lldus mood=%d activity=%d reaction=%d internal=%u psram=%u",
                 fps, (long long)average_us,
                 (long long)face->render_max_us, mood, activity,
                 face->reaction_started_ms == 0 ? -1 : face->active_reaction,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        face->report_started_ms = now;
        face->report_frames = 0;
        face->render_total_us = 0;
        face->render_max_us = 0;
    }
}

static uint32_t frame_period_for_activity(face_activity_t activity)
{
    switch (activity) {
    case FACE_ACTIVITY_SLEEPING:
        return FACE_SLEEPING_FRAME_PERIOD_MS;
    case FACE_ACTIVITY_OFFLINE:
        return FACE_OFFLINE_FRAME_PERIOD_MS;
    case FACE_ACTIVITY_LISTENING:
    case FACE_ACTIVITY_THINKING:
    case FACE_ACTIVITY_SPEAKING:
    case FACE_ACTIVITY_CONFIRM:
    case FACE_ACTIVITY_SUCCESS:
    case FACE_ACTIVITY_ERROR:
        return FACE_ACTIVE_FRAME_PERIOD_MS;
    case FACE_ACTIVITY_IDLE:
    default:
        return FACE_IDLE_FRAME_PERIOD_MS;
    }
}

static void animation_timer_cb(lv_timer_t *timer)
{
    face_t *face = lv_timer_get_user_data(timer);
    const uint32_t now = lv_tick_get();
    update_position_drift(face, now);
    render_face(face, now);

    portENTER_CRITICAL(&face->state_lock);
    const face_activity_t activity = face->activity;
    portEXIT_CRITICAL(&face->state_lock);
    const uint32_t period = frame_period_for_activity(activity);
    if (face->timer_period_ms != period) {
        face->timer_period_ms = period;
        lv_timer_set_period(timer, period);
    }
}

static void notify_input(face_t *face, face_input_event_t input_event)
{
    face_input_callback_t callback;
    void *context;
    portENTER_CRITICAL(&face->state_lock);
    callback = face->input_callback;
    context = face->input_context;
    portEXIT_CRITICAL(&face->state_lock);
    if (callback != NULL) {
        callback(input_event, context);
    }
}

static bool point_hits_mute_control(const face_t *face,
                                    const lv_point_t *point)
{
    const int32_t dx = point->x - (MUTE_CONTROL_X + face->drift_x);
    const int32_t dy = point->y - (MUTE_CONTROL_Y + face->drift_y);
    return dx * dx + dy * dy
        <= MUTE_CONTROL_HIT_RADIUS * MUTE_CONTROL_HIT_RADIUS;
}

static void touch_event_cb(lv_event_t *event)
{
    face_t *face = lv_event_get_user_data(event);
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        lv_point_t point;
        lv_indev_get_point(lv_indev_active(), &point);
        face->mute_pressed = point_hits_mute_control(face, &point);
        face->ptt_pressed = !face->mute_pressed;
        if (face->ptt_pressed) {
            face_react(face, FACE_REACTION_FOCUS, 0.85f);
        }
        notify_input(face, face->mute_pressed
                     ? FACE_INPUT_MUTE_TOGGLE : FACE_INPUT_PTT_START);
        return;
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (face->ptt_pressed) {
            face->ptt_pressed = false;
            notify_input(face, FACE_INPUT_PTT_STOP);
        }
        face->mute_pressed = false;
    }
}

face_t *face_create(lv_obj_t *parent)
{
    face_t *face = calloc(1, sizeof(*face));
    if (face == NULL) {
        return NULL;
    }
    portMUX_INITIALIZE(&face->state_lock);

    const size_t buffer_size = FACE_CANVAS_WIDTH * FACE_CANVAS_HEIGHT
        * sizeof(lv_color16_t);
    face->canvas_buffer = heap_caps_aligned_alloc(
        64, buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (face->canvas_buffer == NULL) {
        free(face);
        return NULL;
    }

    face->canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(face->canvas, face->canvas_buffer,
                         FACE_CANVAS_WIDTH, FACE_CANVAS_HEIGHT,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(face->canvas, FACE_CANVAS_X, FACE_CANVAS_Y);
    lv_obj_add_flag(face->canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(face->canvas, touch_event_cb, LV_EVENT_PRESSED, face);
    lv_obj_add_event_cb(face->canvas, touch_event_cb, LV_EVENT_RELEASED, face);
    lv_obj_add_event_cb(face->canvas, touch_event_cb, LV_EVENT_PRESS_LOST, face);

    const uint32_t now = lv_tick_get();
    face->activity = FACE_ACTIVITY_IDLE;
    face->mood = FACE_MOOD_WARM;
    face->mood_intensity = 0.72f;
    face->output_volume_percent = 20;
    face->pose = neutral_pose();
    face->next_drift_ms = now + FACE_DRIFT_PERIOD_MS;
    face->next_blink_ms = now + 1400;
    face->next_saccade_ms = now + 500;
    face->report_started_ms = now;
    render_face(face, now);
    face->timer_period_ms = FACE_IDLE_FRAME_PERIOD_MS;
    face->timer = lv_timer_create(animation_timer_cb,
                                  face->timer_period_ms, face);
    return face;
}

void face_set_input_callback(face_t *face, face_input_callback_t callback,
                             void *context)
{
    if (face == NULL) {
        return;
    }
    portENTER_CRITICAL(&face->state_lock);
    face->input_callback = callback;
    face->input_context = context;
    portEXIT_CRITICAL(&face->state_lock);
}

void face_set_activity(face_t *face, face_activity_t activity)
{
    if (face == NULL || activity < 0 || activity >= FACE_ACTIVITY_COUNT) {
        return;
    }
    portENTER_CRITICAL(&face->state_lock);
    face->activity = activity;
    portEXIT_CRITICAL(&face->state_lock);
}

void face_set_mood(face_t *face, face_mood_t mood, float intensity)
{
    if (face == NULL || mood < 0 || mood >= FACE_MOOD_COUNT) {
        return;
    }
    portENTER_CRITICAL(&face->state_lock);
    face->mood = mood;
    face->mood_intensity = clampf(intensity, 0.0f, 1.0f);
    portEXIT_CRITICAL(&face->state_lock);
}

face_mood_t face_get_mood(face_t *face)
{
    if (face == NULL) {
        return FACE_MOOD_CALM;
    }
    portENTER_CRITICAL(&face->state_lock);
    const face_mood_t mood = face->mood;
    portEXIT_CRITICAL(&face->state_lock);
    return mood;
}

void face_react(face_t *face, face_reaction_t reaction, float intensity)
{
    if (face == NULL || reaction < 0 || reaction >= FACE_REACTION_COUNT) {
        return;
    }
    portENTER_CRITICAL(&face->state_lock);
    face->requested_reaction = reaction;
    face->requested_reaction_intensity = clampf(intensity, 0.0f, 1.0f);
    face->reaction_generation++;
    portEXIT_CRITICAL(&face->state_lock);
}

void face_set_playback_level(face_t *face, float level)
{
    if (face == NULL) {
        return;
    }
    portENTER_CRITICAL(&face->state_lock);
    face->playback_level = clampf(level, 0.0f, 1.0f);
    portEXIT_CRITICAL(&face->state_lock);
}

void face_set_muted(face_t *face, bool muted)
{
    if (face == NULL) {
        return;
    }
    portENTER_CRITICAL(&face->state_lock);
    face->muted = muted;
    portEXIT_CRITICAL(&face->state_lock);
}

void face_set_output_volume(face_t *face, uint8_t volume_percent)
{
    if (face == NULL || volume_percent < 10 || volume_percent > 100) {
        return;
    }
    portENTER_CRITICAL(&face->state_lock);
    face->output_volume_percent = volume_percent;
    portEXIT_CRITICAL(&face->state_lock);
}
