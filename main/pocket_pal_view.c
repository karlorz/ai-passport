#include "pocket_pal_view.h"

// The pet is drawn from plain LVGL shapes: this application ships no image
// assets, and a procedural body keeps Flash free for the Chinese subset. The
// container is taller than the body so reaction overlays have room to move
// inside it; children are clipped to the container, so nothing leaves the box.
#define PET_W 110
#define PET_H 130
#define BODY_X 0
#define BODY_Y 40
#define BODY_W 110
#define BODY_H 90
#define EYE_W 14
#define EYE_H 18
#define EYE_Y 66
#define EYE_LEFT_X 33
#define EYE_RIGHT_X 63
#define MOUTH_W 14
#define MOUTH_H 8
#define MOUTH_X 48
#define MOUTH_Y 94
#define CHEEK_W 16
#define CHEEK_H 10
#define CHEEK_Y 88
#define CHEEK_LEFT_X 11
#define CHEEK_RIGHT_X 83
#define BLINK_PERIOD_MS 4000U
#define BLINK_HALF_MS 60U
#define BOB_OFFSET 4
#define BOB_HALF_MS 600U

// Mouth shapes are polylines so one object covers smile, flat, and frown without
// swapping widgets. lv_line keeps only the pointer, so these must outlive every
// pet: they are file-scope constants shared by all instances.
static const lv_point_precise_t MOUTH_SMILE[] = { { 0, 0 }, { 7, 5 }, { 14, 0 } };
static const lv_point_precise_t MOUTH_FLAT[] = { { 0, 2 }, { 14, 2 } };
static const lv_point_precise_t MOUTH_FROWN[] = { { 0, 5 }, { 7, 0 }, { 14, 5 } };

// Outline heart, traced once at 14 x 13 px and shared by every floating heart.
static const lv_point_precise_t HEART[] = {
    {  6,  3 }, {  7,  2 }, {  9,  0 }, { 12,  1 },
    { 13,  3 }, { 12,  6 }, {  9,  9 }, {  7, 11 },
    {  6, 12 }, {  6, 11 }, {  4,  9 }, {  1,  6 },
    {  0,  3 }, {  1,  1 }, {  4,  0 }, {  6,  2 },
    {  6,  3 },
};

typedef struct {
    lv_obj_t *eye[2];
    lv_obj_t *mouth;
    lv_obj_t *cheek[2];
    lv_obj_t *tear;
    lv_timer_t *blink;
    pp_expression_t expr;
} pet_state_t;

static lv_obj_t *shape(lv_obj_t *parent, int x, int y, int w, int h, int radius,
                       uint32_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    return obj;
}

// Invisible container: the theme paints every lv_obj, so a grouping or drawing
// object has to clear its own background explicitly.
static lv_obj_t *holder(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *obj = shape(parent, x, y, w, h, 0, PP_COLOR_BG);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    return obj;
}

static lv_obj_t *stroked_line(lv_obj_t *parent, int x, int y, int w, int h,
                              uint32_t color, int width)
{
    lv_obj_t *line = lv_line_create(parent);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(line, x, y);
    lv_obj_set_size(line, w, h);
    lv_obj_set_style_pad_all(line, 0, 0);
    lv_obj_set_style_line_color(line, lv_color_hex(color), 0);
    lv_obj_set_style_line_width(line, width, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    return line;
}

static pet_state_t *pet_state(lv_obj_t *pet)
{
    return pet ? (pet_state_t *)lv_obj_get_user_data(pet) : NULL;
}

static void bob_cb(void *obj, int32_t value)
{
    lv_obj_set_y((lv_obj_t *)obj, value);
}

static void eye_scale_cb(void *obj, int32_t value)
{
    lv_obj_set_style_transform_scale_y((lv_obj_t *)obj, value, 0);
}

static void set_y_cb(void *obj, int32_t value)
{
    lv_obj_set_y((lv_obj_t *)obj, value);
}

static void set_x_cb(void *obj, int32_t value)
{
    lv_obj_set_x((lv_obj_t *)obj, value);
}

static void set_opa_cb(void *obj, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

static void blink_timer_cb(lv_timer_t *timer)
{
    pet_state_t *state = pet_state((lv_obj_t *)lv_timer_get_user_data(timer));
    if (!state) return;

    for (int i = 0; i < 2; i++) {
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, state->eye[i]);
        lv_anim_set_exec_cb(&anim, eye_scale_cb);
        // 10% of the natural height reads as a closed lid. The eye pivot is its
        // own centre, so the lid collapses in place instead of sliding down.
        lv_anim_set_values(&anim, LV_SCALE_NONE, LV_SCALE_NONE / 10);
        lv_anim_set_duration(&anim, BLINK_HALF_MS);
        lv_anim_set_reverse_duration(&anim, BLINK_HALF_MS);
        lv_anim_start(&anim);
    }
}

// Overlays delete themselves when their animation completes, so a reaction never
// leaves an object behind and the app owns no reaction state. A power-off or
// screen change instead deletes the pet, which drops the animations with it.
static void overlay_done_cb(lv_anim_t *anim)
{
    lv_obj_delete((lv_obj_t *)anim->var);
}

static void pet_delete_cb(lv_event_t *event)
{
    lv_obj_t *pet = lv_event_get_target(event);
    pet_state_t *state = pet_state(pet);
    if (!state) return;

    // LVGL drops animations whose target is deleted, but a plain lv_timer is not
    // tied to an object: without this delete it would later fire against freed
    // state.
    if (state->blink) lv_timer_delete(state->blink);
    lv_obj_set_user_data(pet, NULL);
    lv_free(state);
}

static void apply_expression(lv_obj_t *pet, pp_expression_t expr)
{
    pet_state_t *state = pet_state(pet);
    if (!state) return;

    bool cheeks = false;
    bool tear = false;

    switch (expr) {
    case PP_EXPR_HAPPY:
        lv_line_set_points(state->mouth, MOUTH_SMILE, 3);
        lv_obj_set_style_line_width(state->mouth, 5, 0);
        cheeks = true;
        break;
    case PP_EXPR_CONTENT:
        lv_line_set_points(state->mouth, MOUTH_SMILE, 3);
        lv_obj_set_style_line_width(state->mouth, 3, 0);
        cheeks = true;
        break;
    case PP_EXPR_HUNGRY:
        lv_line_set_points(state->mouth, MOUTH_FLAT, 2);
        lv_obj_set_style_line_width(state->mouth, 3, 0);
        break;
    case PP_EXPR_TIRED:
        lv_line_set_points(state->mouth, MOUTH_FLAT, 2);
        lv_obj_set_style_line_width(state->mouth, 3, 0);
        break;
    case PP_EXPR_SAD:
        lv_line_set_points(state->mouth, MOUTH_FROWN, 3);
        lv_obj_set_style_line_width(state->mouth, 3, 0);
        break;
    }

    const bool half_lidded = expr == PP_EXPR_TIRED;
    for (int i = 0; i < 2; i++) {
        // A half-lidded eye shows only the lower half of the socket, as if a lid
        // had drooped over it. The blink pivot follows the current height so the
        // lid always closes towards its own centre.
        const int height = half_lidded ? EYE_H / 2 : EYE_H;
        lv_obj_set_y(state->eye[i], half_lidded ? EYE_Y + EYE_H / 2 : EYE_Y);
        lv_obj_set_height(state->eye[i], height);
        lv_obj_set_style_transform_pivot_y(state->eye[i], height / 2, 0);
        if (cheeks) {
            lv_obj_remove_flag(state->cheek[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(state->cheek[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (tear) {
        lv_obj_remove_flag(state->tear, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(state->tear, LV_OBJ_FLAG_HIDDEN);
    }
}

lv_obj_t *pp_pet_create(lv_obj_t *parent, int x, int y)
{
    if (!parent) return NULL;

    lv_obj_t *pet = holder(parent, x, y, PET_W, PET_H);
    pet_state_t *state = lv_malloc(sizeof(*state));
    if (!state) {
        lv_obj_delete(pet);
        return NULL;
    }
    *state = (pet_state_t){ .expr = PP_EXPR_CONTENT };
    lv_obj_set_user_data(pet, state);
    lv_obj_add_event_cb(pet, pet_delete_cb, LV_EVENT_DELETE, NULL);

    shape(pet, BODY_X, BODY_Y, BODY_W, BODY_H, 28, PP_COLOR_MINT);

    state->eye[0] = shape(pet, EYE_LEFT_X, EYE_Y, EYE_W, EYE_H, EYE_W / 2,
                          PP_COLOR_INK);
    state->eye[1] = shape(pet, EYE_RIGHT_X, EYE_Y, EYE_W, EYE_H, EYE_W / 2,
                          PP_COLOR_INK);
    for (int i = 0; i < 2; i++) {
        lv_obj_set_style_transform_pivot_x(state->eye[i], EYE_W / 2, 0);
        lv_obj_set_style_transform_pivot_y(state->eye[i], EYE_H / 2, 0);
    }

    state->mouth = stroked_line(pet, MOUTH_X, MOUTH_Y, MOUTH_W, MOUTH_H,
                                PP_COLOR_INK, 3);

    state->cheek[0] = shape(pet, CHEEK_LEFT_X, CHEEK_Y, CHEEK_W, CHEEK_H,
                            CHEEK_H / 2, PP_COLOR_PINK);
    state->cheek[1] = shape(pet, CHEEK_RIGHT_X, CHEEK_Y, CHEEK_W, CHEEK_H,
                            CHEEK_H / 2, PP_COLOR_PINK);

    state->tear = shape(pet, EYE_LEFT_X + 4, CHEEK_Y, 8, 11, 4, PP_COLOR_CORAL);
    lv_obj_add_flag(state->tear, LV_OBJ_FLAG_HIDDEN);

    apply_expression(pet, PP_EXPR_CONTENT);

    // Idle bob on the container, so the body and every overlay travel together.
    // One reversing animation both moves and restores the resting position.
    lv_anim_t bob;
    lv_anim_init(&bob);
    lv_anim_set_var(&bob, pet);
    lv_anim_set_exec_cb(&bob, bob_cb);
    lv_anim_set_values(&bob, y, y - BOB_OFFSET);
    lv_anim_set_duration(&bob, BOB_HALF_MS);
    lv_anim_set_reverse_duration(&bob, BOB_HALF_MS);
    lv_anim_set_repeat_count(&bob, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&bob);

    state->blink = lv_timer_create(blink_timer_cb, BLINK_PERIOD_MS, pet);
    return pet;
}

void pp_pet_set_expression(lv_obj_t *pet, pp_expression_t expr)
{
    pet_state_t *state = pet_state(pet);
    if (!state || state->expr == expr) return;
    state->expr = expr;
    apply_expression(pet, expr);
}

static void spawn_heart(lv_obj_t *pet, int x, int y, uint32_t delay_ms)
{
    lv_obj_t *heart = holder(pet, x, y, 14, 13);
    lv_obj_set_style_opa(heart, LV_OPA_TRANSP, 0);

    lv_obj_t *outline = stroked_line(heart, 0, 0, 14, 13, PP_COLOR_PINK, 2);
    lv_line_set_points(outline, HEART, sizeof(HEART) / sizeof(HEART[0]));

    // Fade the container: the line part inherits the object opacity, so one
    // animation covers the whole heart and the completion deletes both.
    lv_anim_t fade;
    lv_anim_init(&fade);
    lv_anim_set_var(&fade, heart);
    lv_anim_set_exec_cb(&fade, set_opa_cb);
    lv_anim_set_values(&fade, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&fade, 160);
    lv_anim_set_delay(&fade, delay_ms);
    lv_anim_set_reverse_delay(&fade, 380);
    lv_anim_set_reverse_duration(&fade, 320);
    lv_anim_set_completed_cb(&fade, overlay_done_cb);
    lv_anim_start(&fade);

    lv_anim_t rise;
    lv_anim_init(&rise);
    lv_anim_set_var(&rise, heart);
    lv_anim_set_exec_cb(&rise, set_y_cb);
    lv_anim_set_values(&rise, y, y - 46);
    lv_anim_set_duration(&rise, 700);
    lv_anim_set_delay(&rise, delay_ms);
    lv_anim_set_path_cb(&rise, lv_anim_path_ease_out);
    lv_anim_start(&rise);
}

void pp_pet_react(lv_obj_t *pet, pp_react_t react)
{
    if (!pet) return;

    switch (react) {
    case PP_REACT_FEED: {
        // A food dot drops onto the pet's head; the bounce path supplies the
        // little hop on landing without needing a second animation.
        lv_obj_t *dot = shape(pet, 49, 2, 12, 12, 6, PP_COLOR_CORAL);
        lv_anim_t drop;
        lv_anim_init(&drop);
        lv_anim_set_var(&drop, dot);
        lv_anim_set_exec_cb(&drop, set_y_cb);
        lv_anim_set_values(&drop, 2, BODY_Y - 6);
        lv_anim_set_duration(&drop, 420);
        lv_anim_set_path_cb(&drop, lv_anim_path_bounce);
        lv_anim_set_completed_cb(&drop, overlay_done_cb);
        lv_anim_start(&drop);
        break;
    }
    case PP_REACT_PLAY:
        spawn_heart(pet, 30, 60, 0);
        spawn_heart(pet, 46, 56, 120);
        spawn_heart(pet, 62, 60, 240);
        break;
    case PP_REACT_REST: {
        lv_obj_t *zzz = holder(pet, 80, 50, 26, 20);
        lv_obj_t *label = lv_label_create(zzz);
        lv_label_set_text(label, "Zzz");
        lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(PP_COLOR_INK), 0);
        lv_obj_center(label);

        // The vertical drift owns the lifetime: deleting the label in its
        // completion also drops the sideways animation on the same object.
        lv_anim_t drift;
        lv_anim_init(&drift);
        lv_anim_set_var(&drift, zzz);
        lv_anim_set_exec_cb(&drift, set_y_cb);
        lv_anim_set_values(&drift, 50, 20);
        lv_anim_set_duration(&drift, 900);
        lv_anim_set_path_cb(&drift, lv_anim_path_ease_out);
        lv_anim_set_completed_cb(&drift, overlay_done_cb);
        lv_anim_start(&drift);

        lv_anim_t slide;
        lv_anim_init(&slide);
        lv_anim_set_var(&slide, zzz);
        lv_anim_set_exec_cb(&slide, set_x_cb);
        lv_anim_set_values(&slide, 80, 84);
        lv_anim_set_duration(&slide, 900);
        lv_anim_set_path_cb(&slide, lv_anim_path_ease_out);
        lv_anim_start(&slide);
        break;
    }
    }
}
