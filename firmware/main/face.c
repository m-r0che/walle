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

#define FACE_WIDTH 368
#define FACE_HEIGHT 448
#define FACE_CANVAS_WIDTH 320
#define FACE_CANVAS_HEIGHT 220
#define FACE_CANVAS_X ((FACE_WIDTH - FACE_CANVAS_WIDTH) / 2)
#define FACE_CANVAS_Y ((FACE_HEIGHT - FACE_CANVAS_HEIGHT) / 2)
#define FACE_IDLE_FRAME_PERIOD_MS 40
#define FACE_ACTIVE_FRAME_PERIOD_MS 33
#define FACE_THINKING_FRAME_PERIOD_MS 50
#define FACE_OFFLINE_FRAME_PERIOD_MS 100
#define FACE_SLEEPING_FRAME_PERIOD_MS 200
#define EYE_POINT_COUNT 13
#define MOUTH_POINT_COUNT 9
#define PI_F 3.14159265358979323846f
#define MUTE_CONTROL_X 48
#define MUTE_CONTROL_Y 313
#define MUTE_CONTROL_HIT_RADIUS 24
#define FACE_DRIFT_PERIOD_MS 60000

static const char *TAG = "face";

typedef struct {
    lv_point_precise_t points[EYE_POINT_COUNT];
} eye_curve_t;

typedef struct {
    lv_point_precise_t points[MOUTH_POINT_COUNT];
} mouth_curve_t;

struct face {
    lv_obj_t *canvas;
    uint16_t *canvas_buffer;
    lv_timer_t *timer;
    uint32_t timer_period_ms;
    portMUX_TYPE state_lock;
    face_emotion_t emotion;
    face_interaction_t interaction;
    float mouth_level;
    bool muted;
    uint8_t output_volume_percent;
    face_input_callback_t input_callback;
    void *input_context;
    bool ptt_pressed;
    bool mute_pressed;
    int8_t drift_x;
    int8_t drift_y;
    uint8_t drift_phase;
    uint32_t next_drift_ms;
    uint32_t blink_started_ms;
    uint32_t next_blink_ms;
    uint32_t next_saccade_ms;
    float gaze_x;
    float gaze_y;
    float gaze_target_x;
    float gaze_target_y;
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

static void update_position_drift(face_t *face, uint32_t now)
{
    static const int8_t positions[][2] = {
        {0, 0}, {3, -2}, {-2, -3}, {-3, 2}, {2, 3}, {1, -1}, {-1, 1},
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

static float blink_openness(face_t *face, uint32_t now)
{
    if (face->blink_started_ms == 0) {
        if (time_reached(now, face->next_blink_ms)) {
            face->blink_started_ms = now;
        } else {
            return 1.0f;
        }
    }

    const uint32_t elapsed = now - face->blink_started_ms;
    if (elapsed < 90) {
        return 1.0f - smoothstep((float)elapsed / 90.0f);
    }
    if (elapsed < 280) {
        return smoothstep((float)(elapsed - 90) / 190.0f);
    }

    face->blink_started_ms = 0;
    face->next_blink_ms = now + random_range(2500, 4500);
    return 1.0f;
}

static void update_gaze(face_t *face, uint32_t now)
{
    if (time_reached(now, face->next_saccade_ms)) {
        face->gaze_target_x = (float)((int32_t)random_range(0, 160) - 80) / 10.0f;
        face->gaze_target_y = (float)((int32_t)random_range(0, 80) - 40) / 10.0f;
        face->next_saccade_ms = now + random_range(500, 2500);
    }

    face->gaze_x += (face->gaze_target_x - face->gaze_x) * 0.22f;
    face->gaze_y += (face->gaze_target_y - face->gaze_y) * 0.18f;
}

typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} raster_color_t;

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

static void brighten_pixel(face_t *face, int32_t x, int32_t y, uint16_t source)
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
                uint16_t *pixel = canvas_pixel_at(face, center_x + x, center_y + y);
                if (pixel != NULL) {
                    *pixel = 0;
                }
            }
        }
    }
}

static void draw_raster_segment(face_t *face, const lv_point_precise_t *start,
                                const lv_point_precise_t *end, int32_t radius,
                                uint16_t color)
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

static void draw_glow_curve(face_t *face, const lv_point_precise_t *points,
                            size_t point_count, raster_color_t glow_color,
                            raster_color_t line_color)
{
    static const int32_t radii[] = {10, 6, 3};
    static const uint8_t opacities[] = {38, 105, 255};
    const raster_color_t colors[] = {glow_color, glow_color, line_color};

    for (size_t pass = 0; pass < 3; pass++) {
        const uint16_t color = pack_dimmed_color(colors[pass], opacities[pass]);
        for (size_t index = 1; index < point_count; index++) {
            draw_raster_segment(face, &points[index - 1], &points[index],
                                radii[pass], color);
        }
    }
}

static void build_eye_curve(eye_curve_t *top, eye_curve_t *bottom,
                            float center_x, float center_y, float half_width,
                            float half_height, float tilt, bool happy)
{
    for (size_t index = 0; index < EYE_POINT_COUNT; index++) {
        const float t = (float)index / (EYE_POINT_COUNT - 1);
        const float x_norm = t * 2.0f - 1.0f;
        const float arch = sinf(t * PI_F);
        const float tilt_offset = tilt * x_norm * half_width;

        if (happy) {
            const float x = center_x + x_norm * half_width;
            const float y = center_y - arch * 15.0f + tilt_offset;
            top->points[index].x = (lv_value_precise_t)x;
            bottom->points[index].x = (lv_value_precise_t)x;
            top->points[index].y = (lv_value_precise_t)y;
            bottom->points[index].y = (lv_value_precise_t)y;
        } else {
            // Sample the upper and lower halves of a true ellipse. At full
            // openness, equal half-width/height produce the circular sockets
            // selected in the off-device face study.
            const float upper_angle = PI_F - t * PI_F;
            const float lower_angle = PI_F + t * PI_F;
            top->points[index].x = (lv_value_precise_t)(
                center_x + cosf(upper_angle) * half_width);
            bottom->points[index].x = (lv_value_precise_t)(
                center_x + cosf(lower_angle) * half_width);
            top->points[index].y = (lv_value_precise_t)(
                center_y - sinf(upper_angle) * half_height + tilt_offset);
            bottom->points[index].y = (lv_value_precise_t)(
                center_y - sinf(lower_angle) * half_height + tilt_offset);
        }
    }
}

static void draw_eye(face_t *face, float center_x, float center_y,
                     float openness, float tilt, bool happy,
                     float gaze_x, float gaze_y, float socket_radius,
                     float pupil_scale, bool force_pupils)
{
    eye_curve_t top;
    eye_curve_t bottom;
    build_eye_curve(&top, &bottom, center_x, center_y, socket_radius,
                    socket_radius * openness, tilt, happy);

    const raster_color_t deep_cyan = {0x00, 0x67, 0x75};
    const raster_color_t ice_cyan = {0xb9, 0xff, 0xff};
    draw_glow_curve(face, top.points, EYE_POINT_COUNT, deep_cyan, ice_cyan);
    if (!happy) {
        draw_glow_curve(face, bottom.points, EYE_POINT_COUNT, deep_cyan, ice_cyan);
    }

    // Keep the eye sockets anchored while the pupils look around. Narrow-eye
    // expressions can request tiny pupils explicitly, matching the face sheet.
    if (!happy && pupil_scale > 0.0f
            && (openness > 0.58f || force_pupils)) {
        const int32_t iris_x = (int32_t)(center_x + gaze_x);
        const int32_t iris_y = (int32_t)(center_y + gaze_y);
        const int32_t outer_radius = LV_MAX(3, (int32_t)(14.0f * pupil_scale));
        const int32_t inner_radius = LV_MAX(2, (int32_t)(11.0f * pupil_scale));
        const int32_t pupil_radius = LV_MAX(1, (int32_t)(6.0f * pupil_scale));
        draw_disc(face, iris_x, iris_y, outer_radius,
                  pack_dimmed_color(deep_cyan, 150));
        draw_disc(face, iris_x, iris_y, inner_radius,
                  pack_dimmed_color(ice_cyan, 135));
        clear_disc(face, iris_x, iris_y, pupil_radius);
        if (pupil_scale >= 0.65f) {
            draw_disc(face, iris_x - 3, iris_y - 4, 2,
                      pack_dimmed_color(
                          (raster_color_t){0xff, 0xff, 0xff}, 255));
        }
    }
}

static void draw_soft_curve(face_t *face, const lv_point_precise_t *points,
                            size_t point_count)
{
    static const int32_t radii[] = {5, 2, 1};
    static const uint8_t brightness[] = {38, 105, 215};
    const raster_color_t deep_cyan = {0x00, 0x67, 0x75};
    const raster_color_t soft_cyan = {0x68, 0xd9, 0xe3};

    for (size_t pass = 0; pass < 3; pass++) {
        const raster_color_t color = pass == 2 ? soft_cyan : deep_cyan;
        const uint16_t packed = pack_dimmed_color(color, brightness[pass]);
        for (size_t index = 1; index < point_count; index++) {
            draw_raster_segment(face, &points[index - 1], &points[index],
                                radii[pass], packed);
        }
    }
}

static void build_mouth(mouth_curve_t *mouth, face_emotion_t emotion,
                        float center_y)
{
    float half_width = 55.0f;
    switch (emotion) {
    case FACE_EMOTION_HAPPY:
    case FACE_EMOTION_INTERESTED:
    case FACE_EMOTION_DEVIOUS:
        half_width = 58.0f;
        break;
    case FACE_EMOTION_DOUBTFUL:
        half_width = 46.0f;
        break;
    case FACE_EMOTION_SLEEPY:
        half_width = 40.0f;
        break;
    case FACE_EMOTION_PEEK:
        half_width = 34.0f;
        break;
    case FACE_EMOTION_ANGRY:
    case FACE_EMOTION_FURIOUS:
    case FACE_EMOTION_SAD:
        half_width = 54.0f;
        break;
    case FACE_EMOTION_SURPRISED:
        half_width = 14.0f;
        break;
    case FACE_EMOTION_NEUTRAL:
    default:
        break;
    }

    for (size_t index = 0; index < MOUTH_POINT_COUNT; index++) {
        const float t = (float)index / (MOUTH_POINT_COUNT - 1);
        const float x_norm = t * 2.0f - 1.0f;
        float y = center_y;

        switch (emotion) {
        case FACE_EMOTION_HAPPY:
            y += -10.0f + sinf(t * PI_F) * 25.0f;
            break;
        case FACE_EMOTION_INTERESTED:
            y += sinf(t * PI_F) * 22.0f;
            break;
        case FACE_EMOTION_DEVIOUS:
            y += -5.0f + sinf(t * PI_F) * 27.0f;
            break;
        case FACE_EMOTION_DOUBTFUL:
            y -= sinf(t * PI_F) * 15.0f;
            break;
        case FACE_EMOTION_SLEEPY:
            y += sinf(t * PI_F) * 13.0f;
            break;
        case FACE_EMOTION_PEEK:
            y += sinf(t * PI_F) * 3.0f;
            break;
        case FACE_EMOTION_ANGRY:
            y -= sinf(t * PI_F) * 19.0f;
            break;
        case FACE_EMOTION_FURIOUS:
            y -= sinf(t * PI_F) * 25.0f;
            break;
        case FACE_EMOTION_SAD:
            y -= sinf(t * PI_F) * 17.0f;
            break;
        case FACE_EMOTION_SURPRISED:
            y += sinf(t * PI_F) * 4.0f;
            break;
        case FACE_EMOTION_NEUTRAL:
        default:
            y += sinf(t * PI_F) * 22.0f;
            break;
        }

        mouth->points[index].x = (lv_value_precise_t)(FACE_WIDTH / 2 + x_norm * half_width);
        mouth->points[index].y = (lv_value_precise_t)y;
    }
}

static void draw_surprised_mouth(face_t *face, float center_y)
{
    lv_point_precise_t ring[17];
    for (size_t index = 0; index < 17; index++) {
        const float angle = (float)index * 2.0f * PI_F / 16.0f;
        ring[index].x = (lv_value_precise_t)(FACE_WIDTH / 2
                                             + cosf(angle) * 14.0f);
        ring[index].y = (lv_value_precise_t)(center_y
                                             + sinf(angle) * 17.0f);
    }
    draw_glow_curve(face, ring, 17,
                    (raster_color_t){0x00, 0x67, 0x75},
                    (raster_color_t){0xa8, 0xff, 0xff});
}

static void draw_speaking_mouth(face_t *face, float center_y, float level)
{
    mouth_curve_t upper;
    mouth_curve_t lower;
    const float eased_level = smoothstep(level);
    const float half_width = 42.0f + eased_level * 12.0f;
    const float half_opening = 2.0f + eased_level * 13.0f;

    for (size_t index = 0; index < MOUTH_POINT_COUNT; index++) {
        const float t = (float)index / (MOUTH_POINT_COUNT - 1);
        const float x_norm = t * 2.0f - 1.0f;
        const float arch = sinf(t * PI_F);
        const lv_value_precise_t x = (lv_value_precise_t)(
            FACE_WIDTH / 2 + x_norm * half_width);
        upper.points[index].x = x;
        lower.points[index].x = x;
        upper.points[index].y = (lv_value_precise_t)(center_y - arch * half_opening);
        lower.points[index].y = (lv_value_precise_t)(center_y + arch * half_opening);
    }

    const raster_color_t deep_cyan = {0x00, 0x67, 0x75};
    const raster_color_t ice_cyan = {0xa8, 0xff, 0xff};
    draw_glow_curve(face, upper.points, MOUTH_POINT_COUNT, deep_cyan, ice_cyan);
    draw_glow_curve(face, lower.points, MOUTH_POINT_COUNT, deep_cyan, ice_cyan);
}

static void draw_mute_control(face_t *face, bool muted)
{
    lv_point_precise_t ring[13];
    for (size_t index = 0; index < 13; index++) {
        const float angle = (float)index * 2.0f * PI_F / 12.0f;
        ring[index].x = MUTE_CONTROL_X + (lv_value_precise_t)(cosf(angle) * 7.0f);
        ring[index].y = MUTE_CONTROL_Y + (lv_value_precise_t)(sinf(angle) * 7.0f);
    }

    if (muted) {
        const raster_color_t coral_glow = {0x72, 0x16, 0x10};
        const raster_color_t coral = {0xff, 0x72, 0x62};
        draw_glow_curve(face, ring, 13, coral_glow, coral);
        const lv_point_precise_t slash[] = {
            {MUTE_CONTROL_X - 7, MUTE_CONTROL_Y - 7},
            {MUTE_CONTROL_X + 7, MUTE_CONTROL_Y + 7},
        };
        draw_glow_curve(face, slash, 2, coral_glow, coral);
    } else {
        draw_soft_curve(face, ring, 13);
    }
}

static void render_face(face_t *face, uint32_t now)
{
    const int64_t started_us = esp_timer_get_time();
    face_emotion_t emotion;
    face_interaction_t interaction;
    float mouth_level;
    bool muted;
    portENTER_CRITICAL(&face->state_lock);
    emotion = face->emotion;
    interaction = face->interaction;
    mouth_level = face->mouth_level;
    muted = face->muted;
    portEXIT_CRITICAL(&face->state_lock);

    const float seconds = (float)now / 1000.0f;
    const float breath = sinf(seconds * (2.0f * PI_F / 3.6f)) * 2.5f;
    const float blink = blink_openness(face, now);
    update_gaze(face, now);

    face_emotion_t render_emotion = emotion;
    float gaze_x = face->gaze_x;
    float gaze_y = face->gaze_y;
    switch (interaction) {
    case FACE_INTERACTION_LISTENING:
        render_emotion = FACE_EMOTION_INTERESTED;
        gaze_x = 10.0f;
        gaze_y = -5.0f;
        break;
    case FACE_INTERACTION_THINKING:
        render_emotion = FACE_EMOTION_DOUBTFUL;
        gaze_x = 6.0f;
        gaze_y = -4.0f;
        break;
    case FACE_INTERACTION_CONFIRM:
        render_emotion = FACE_EMOTION_PEEK;
        gaze_x = 5.0f;
        gaze_y = 0.0f;
        break;
    case FACE_INTERACTION_SUCCESS:
        render_emotion = FACE_EMOTION_HAPPY;
        break;
    case FACE_INTERACTION_ERROR:
    case FACE_INTERACTION_OFFLINE:
        render_emotion = FACE_EMOTION_SAD;
        break;
    case FACE_INTERACTION_SLEEPING:
        render_emotion = FACE_EMOTION_SLEEPY;
        break;
    case FACE_INTERACTION_SPEAKING:
    case FACE_INTERACTION_IDLE:
    default:
        break;
    }

    float left_open = blink;
    float right_open = blink;
    float left_tilt = 0.0f;
    float right_tilt = 0.0f;
    float left_gaze_x = gaze_x;
    float right_gaze_x = gaze_x;
    float expression_gaze_y = gaze_y;
    float socket_radius = 50.0f;
    float pupil_scale = 1.0f;
    bool force_pupils = false;
    bool happy = false;

    switch (render_emotion) {
    case FACE_EMOTION_HAPPY:
        happy = true;
        pupil_scale = 0.0f;
        break;
    case FACE_EMOTION_DOUBTFUL:
        left_open *= 0.45f;
        right_open *= 0.45f;
        left_tilt = -0.28f;
        right_tilt = 0.28f;
        pupil_scale = 0.55f;
        force_pupils = true;
        break;
    case FACE_EMOTION_SLEEPY:
        left_open *= 0.22f;
        right_open *= 0.22f;
        pupil_scale = 0.42f;
        force_pupils = true;
        break;
    case FACE_EMOTION_PEEK:
        left_open *= 0.08f;
        right_open *= 0.08f;
        pupil_scale = 0.36f;
        force_pupils = true;
        break;
    case FACE_EMOTION_SURPRISED:
        socket_radius = 54.0f;
        pupil_scale = 0.78f;
        left_gaze_x = right_gaze_x = 0.0f;
        expression_gaze_y = 15.0f;
        break;
    case FACE_EMOTION_INTERESTED:
        left_gaze_x = right_gaze_x = 12.0f;
        expression_gaze_y = -6.0f;
        break;
    case FACE_EMOTION_DEVIOUS:
        left_open *= 0.06f;
        right_open *= 0.06f;
        left_tilt = 0.38f;
        right_tilt = -0.38f;
        left_gaze_x = 35.0f;
        right_gaze_x = -35.0f;
        expression_gaze_y = 3.0f;
        pupil_scale = 0.34f;
        force_pupils = true;
        break;
    case FACE_EMOTION_ANGRY:
        left_open *= 0.06f;
        right_open *= 0.06f;
        left_tilt = 0.50f;
        right_tilt = -0.50f;
        left_gaze_x = 34.0f;
        right_gaze_x = -34.0f;
        expression_gaze_y = 5.0f;
        pupil_scale = 0.35f;
        force_pupils = true;
        break;
    case FACE_EMOTION_FURIOUS:
        left_open *= 0.13f;
        right_open *= 0.13f;
        left_tilt = 0.62f;
        right_tilt = -0.62f;
        left_gaze_x = 33.0f;
        right_gaze_x = -33.0f;
        expression_gaze_y = 7.0f;
        pupil_scale = 0.32f;
        force_pupils = true;
        break;
    case FACE_EMOTION_SAD:
        left_open *= 0.45f;
        right_open *= 0.45f;
        left_tilt = -0.25f;
        right_tilt = 0.25f;
        pupil_scale = 0.55f;
        force_pupils = true;
        break;
    case FACE_EMOTION_NEUTRAL:
    default:
        break;
    }

    memset(face->canvas_buffer, 0,
           FACE_CANVAS_WIDTH * FACE_CANVAS_HEIGHT * sizeof(*face->canvas_buffer));

    const float eye_y = 188.0f + breath;
    draw_eye(face, 105.0f, eye_y, left_open, left_tilt, happy,
             left_gaze_x, expression_gaze_y, socket_radius,
             pupil_scale, force_pupils);
    draw_eye(face, 263.0f, eye_y, right_open, right_tilt, happy,
             right_gaze_x, expression_gaze_y, socket_radius,
             pupil_scale, force_pupils);

    const float mouth_y = 294.0f + breath * 0.7f;
    if (interaction == FACE_INTERACTION_SPEAKING) {
        draw_speaking_mouth(face, mouth_y, mouth_level);
    } else if (render_emotion == FACE_EMOTION_SURPRISED) {
        draw_surprised_mouth(face, mouth_y + 5.0f);
    } else if (interaction == FACE_INTERACTION_LISTENING) {
        draw_speaking_mouth(face, mouth_y, 0.04f);
    } else {
        mouth_curve_t mouth;
        build_mouth(&mouth, render_emotion, mouth_y);
        draw_glow_curve(face, mouth.points, MOUTH_POINT_COUNT,
                        (raster_color_t){0x00, 0x67, 0x75},
                        (raster_color_t){0xa8, 0xff, 0xff});
    }
    draw_mute_control(face, muted);

    // Mirror LVGL's canvas helpers. With the current software draw handlers
    // this has no cache callback; the PSRAM canvas is CPU-read into LVGL's
    // separate display buffer rather than sent directly to the panel.
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
        const float fps = (float)face->report_frames * 1000.0f / report_elapsed;
        const int64_t average_us = face->report_frames == 0
            ? 0
            : face->render_total_us / face->report_frames;
        ESP_LOGI(TAG, "fps=%.1f render_avg=%lldus render_max=%lldus emotion=%d interaction=%d internal=%u psram=%u",
                 fps, (long long)average_us, (long long)face->render_max_us,
                 emotion, interaction,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        face->report_started_ms = now;
        face->report_frames = 0;
        face->render_total_us = 0;
        face->render_max_us = 0;
    }
}

static uint32_t frame_period_for_interaction(face_interaction_t interaction)
{
    switch (interaction) {
    case FACE_INTERACTION_LISTENING:
    case FACE_INTERACTION_SPEAKING:
    case FACE_INTERACTION_CONFIRM:
    case FACE_INTERACTION_SUCCESS:
    case FACE_INTERACTION_ERROR:
        return FACE_ACTIVE_FRAME_PERIOD_MS;
    case FACE_INTERACTION_THINKING:
        return FACE_THINKING_FRAME_PERIOD_MS;
    case FACE_INTERACTION_OFFLINE:
        return FACE_OFFLINE_FRAME_PERIOD_MS;
    case FACE_INTERACTION_SLEEPING:
        return FACE_SLEEPING_FRAME_PERIOD_MS;
    case FACE_INTERACTION_IDLE:
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
    const face_interaction_t interaction = face->interaction;
    portEXIT_CRITICAL(&face->state_lock);
    const uint32_t period = frame_period_for_interaction(interaction);
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
        notify_input(face, face->mute_pressed
                     ? FACE_INPUT_MUTE_TOGGLE
                     : FACE_INPUT_PTT_START);
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

    const size_t buffer_size = FACE_CANVAS_WIDTH * FACE_CANVAS_HEIGHT * sizeof(lv_color16_t);
    face->canvas_buffer = heap_caps_aligned_alloc(64, buffer_size,
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
    face->emotion = FACE_EMOTION_NEUTRAL;
    face->interaction = FACE_INTERACTION_IDLE;
    face->output_volume_percent = 20;
    face->next_drift_ms = now + FACE_DRIFT_PERIOD_MS;
    face->next_blink_ms = now + 1200;
    face->next_saccade_ms = now + 400;
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

void face_set_emotion(face_t *face, face_emotion_t emotion)
{
    if (face == NULL || emotion < 0 || emotion >= FACE_EMOTION_COUNT) {
        return;
    }
    portENTER_CRITICAL(&face->state_lock);
    face->emotion = emotion;
    portEXIT_CRITICAL(&face->state_lock);
}

face_emotion_t face_get_emotion(face_t *face)
{
    if (face == NULL) {
        return FACE_EMOTION_NEUTRAL;
    }
    portENTER_CRITICAL(&face->state_lock);
    const face_emotion_t emotion = face->emotion;
    portEXIT_CRITICAL(&face->state_lock);
    return emotion;
}

void face_set_interaction(face_t *face, face_interaction_t interaction)
{
    if (face == NULL || interaction < 0 || interaction >= FACE_INTERACTION_COUNT) {
        return;
    }
    portENTER_CRITICAL(&face->state_lock);
    face->interaction = interaction;
    portEXIT_CRITICAL(&face->state_lock);
}

void face_set_mouth_level(face_t *face, float level)
{
    if (face == NULL) {
        return;
    }
    portENTER_CRITICAL(&face->state_lock);
    face->mouth_level = clampf(level, 0.0f, 1.0f);
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
