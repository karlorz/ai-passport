// Pocket Pal care model host test: no SDK, no LVGL, only the pure C model.
#include <assert.h>
#include <stdio.h>

#include "pocket_pal_model.h"

static void test_init_defaults(void)
{
    pp_model_t model;

    pp_model_init(&model, 1234U);
    assert(model.version == PP_MODEL_VERSION);
    assert(model.fullness == 80U);
    assert(model.mood == 80U);
    assert(model.energy == 80U);
    assert(model.care_count == 0U);
    assert(model.neglect_events == 0U);
    assert(model.feed_cooldown_s == 0U);
    assert(model.rest_cooldown_s == 0U);
    assert(model.second_accumulator == 0U);
    assert(!model.sad_active);
    assert(model.seed == 1234U);
    assert(pp_model_expression(&model) == PP_EXPR_HAPPY);
}

static void test_decay_exact_periods(void)
{
    pp_model_t model;

    // 900 s covers 5 fullness periods, 3 mood periods, and 3 energy periods.
    pp_model_init(&model, 0U);
    assert(pp_model_tick(&model, 900U) == PP_EVENT_NONE);
    assert(model.fullness == 75U);
    assert(model.mood == 77U);
    assert(model.energy == 77U);

    // Each period is inclusive at its boundary and one second short of it.
    pp_model_init(&model, 0U);
    assert(pp_model_tick(&model, 179U) == PP_EVENT_NONE);
    assert(model.fullness == 80U);
    assert(pp_model_tick(&model, 239U) == PP_EVENT_NONE); // 418 s total
    assert(model.fullness == 78U);                        // 418 / 180 = 2
    assert(model.mood == 79U);                            // 418 / 240 = 1
    assert(model.energy == 79U);                          // 418 / 300 = 1

    pp_model_init(&model, 0U);
    pp_model_tick(&model, 300U);
    assert(model.fullness == 79U);
    assert(model.mood == 79U);
    assert(model.energy == 79U);
}

static void test_decay_remainders_survive_split_ticks(void)
{
    pp_model_t one_shot;
    pp_model_t split;
    unsigned index;

    // One long tick must equal the same span delivered a second at a time.
    pp_model_init(&one_shot, 0U);
    pp_model_tick(&one_shot, 1800U);
    pp_model_init(&split, 0U);
    for (index = 0U; index < 1800U; index++) {
        pp_model_tick(&split, 1U);
    }
    assert(one_shot.fullness == 70U); // 1800 / 180
    assert(one_shot.mood == 73U);     // 1800 / 240
    assert(one_shot.energy == 74U);   // 1800 / 300
    assert(split.fullness == one_shot.fullness);
    assert(split.mood == one_shot.mood);
    assert(split.energy == one_shot.energy);

    // A span that leaves a remainder for every stat decays the same either way.
    pp_model_init(&one_shot, 0U);
    pp_model_tick(&one_shot, 3599U);
    pp_model_init(&split, 0U);
    for (index = 0U; index < 3599U; index++) {
        pp_model_tick(&split, 1U);
    }
    assert(one_shot.fullness == 61U); // 80 - (3599 / 180)
    assert(one_shot.mood == 66U);     // 80 - (3599 / 240)
    assert(one_shot.energy == 69U);   // 80 - (3599 / 300)
    assert(split.fullness == one_shot.fullness);
    assert(split.mood == one_shot.mood);
    assert(split.energy == one_shot.energy);
    assert(split.second_accumulator == 3599U);

    // A many-hour span in one call must not overflow, and keep the leftover that
    // the next call continues from.
    pp_model_init(&one_shot, 0U);
    pp_model_tick(&one_shot, (7U * 3600U) + 137U);
    pp_model_init(&split, 0U);
    for (index = 0U; index < 7U; index++) {
        pp_model_tick(&split, 3600U);
    }
    pp_model_tick(&split, 137U);
    assert(one_shot.fullness == 0U); // 25337 s is far past zero
    assert(one_shot.mood == 0U);
    assert(one_shot.energy == 0U);
    assert(split.fullness == one_shot.fullness);
    assert(split.mood == one_shot.mood);
    assert(split.energy == one_shot.energy);
    assert(one_shot.second_accumulator == 137U);
    assert(split.second_accumulator == 137U);

    // Sub-period time is retained rather than dropped on every short tick.
    pp_model_init(&one_shot, 0U);
    for (index = 0U; index < 179U; index++) {
        pp_model_tick(&one_shot, 1U);
    }
    assert(one_shot.fullness == 80U);
    pp_model_tick(&one_shot, 1U);
    assert(one_shot.fullness == 79U);
}

static void test_decay_saturates_at_zero(void)
{
    pp_model_t model;

    pp_model_init(&model, 0U);
    pp_model_tick(&model, 180U * 100U);
    assert(model.fullness == 0U);
    pp_model_tick(&model, 0xFFFFFFFFU); // upper bound of elapsed_seconds
    assert(model.fullness == 0U);
    assert(model.mood == 0U);
    assert(model.energy == 0U);
}

static void test_feed(void)
{
    pp_model_t model;

    pp_model_init(&model, 0U);
    assert(pp_model_feed(&model) == PP_RESULT_OK);
    assert(model.fullness == 100U); // 80 + 25 clamps to the maximum
    assert(model.feed_cooldown_s == 90U);
    assert(model.care_count == 1U);

    assert(pp_model_feed(&model) == PP_RESULT_COOLDOWN);
    assert(model.care_count == 1U);
    assert(model.fullness == 100U);

    // The cooldown expires with elapsed time, then fullness gates the feed.
    pp_model_tick(&model, 89U);
    assert(model.feed_cooldown_s == 1U);
    assert(pp_model_feed(&model) == PP_RESULT_COOLDOWN);
    pp_model_tick(&model, 1U);
    assert(model.feed_cooldown_s == 0U);
    assert(pp_model_feed(&model) == PP_RESULT_FULL);
    assert(model.care_count == 1U);

    // Just below the refusal threshold the feed succeeds and saturates.
    model.fullness = 94U;
    assert(pp_model_feed(&model) == PP_RESULT_OK);
    assert(model.fullness == 100U);
    assert(model.care_count == 2U);

    model.feed_cooldown_s = 0U;
    model.fullness = 70U;
    assert(pp_model_feed(&model) == PP_RESULT_OK);
    assert(model.fullness == 95U);
    assert(model.care_count == 3U);
}

static void test_play(void)
{
    pp_model_t model;

    pp_model_init(&model, 0U);
    assert(pp_model_play(&model) == PP_RESULT_OK);
    assert(model.mood == 100U);     // 80 + 22 clamps to the maximum
    assert(model.energy == 68U);    // 80 - 12
    assert(model.fullness == 75U);  // 80 - 5
    assert(model.care_count == 1U);

    // Play has no cooldown; repeated play is limited by energy instead.
    assert(pp_model_play(&model) == PP_RESULT_OK);
    assert(model.energy == 56U);

    model.energy = 15U;
    assert(pp_model_play(&model) == PP_RESULT_OK);
    assert(model.energy == 3U);
    assert(model.care_count == 3U);

    model.energy = 14U;
    assert(pp_model_play(&model) == PP_RESULT_NEED_ENERGY);
    assert(model.energy == 14U);
    assert(model.care_count == 3U);

    // Play is refused before it can touch the other stats.
    model.mood = 50U;
    model.fullness = 50U;
    assert(pp_model_play(&model) == PP_RESULT_NEED_ENERGY);
    assert(model.mood == 50U);
    assert(model.fullness == 50U);

    // Fullness saturates at zero instead of wrapping.
    model.energy = 80U;
    model.fullness = 3U;
    assert(pp_model_play(&model) == PP_RESULT_OK);
    assert(model.fullness == 0U);
}

static void test_rest(void)
{
    pp_model_t model;

    pp_model_init(&model, 0U);
    model.energy = 50U;
    assert(pp_model_rest(&model) == PP_RESULT_OK);
    assert(model.energy == 80U);
    assert(model.rest_cooldown_s == 120U);
    assert(model.care_count == 1U);

    assert(pp_model_rest(&model) == PP_RESULT_COOLDOWN);
    assert(model.care_count == 1U);

    pp_model_tick(&model, 119U);
    assert(model.rest_cooldown_s == 1U);
    assert(pp_model_rest(&model) == PP_RESULT_COOLDOWN);
    pp_model_tick(&model, 1U);
    assert(model.rest_cooldown_s == 0U);

    assert(pp_model_rest(&model) == PP_RESULT_OK);
    assert(model.energy == 100U); // 80 + 30 clamps at the maximum
    assert(model.care_count == 2U);
}

static void test_cooldowns_tick_down(void)
{
    pp_model_t model;

    pp_model_init(&model, 0U);
    pp_model_feed(&model);
    pp_model_rest(&model);
    assert(model.feed_cooldown_s == 90U);
    assert(model.rest_cooldown_s == 120U);

    pp_model_tick(&model, 30U);
    assert(model.feed_cooldown_s == 60U);
    assert(model.rest_cooldown_s == 90U);

    // Elapsed time past the end of a cooldown does not bank towards the next one.
    pp_model_tick(&model, 1000U);
    assert(model.feed_cooldown_s == 0U);
    assert(model.rest_cooldown_s == 0U);

    // A long span while the model is already off cooldown stays at zero.
    pp_model_tick(&model, 7200U);
    assert(model.feed_cooldown_s == 0U);
    assert(model.rest_cooldown_s == 0U);
}

static void test_hungry_event_edge(void)
{
    pp_model_t model;

    pp_model_init(&model, 0U);
    model.fullness = 26U;
    assert(pp_model_tick(&model, 180U) == PP_EVENT_NONE); // 26 -> 25, not below
    assert(model.fullness == 25U);
    assert(pp_model_tick(&model, 180U) == PP_EVENT_HUNGRY); // 25 -> 24
    assert(model.fullness == 24U);
    assert(pp_model_tick(&model, 180U) == PP_EVENT_NONE); // already below the threshold
    assert(model.fullness == 23U);

    // Crossing down does not fire twice.
    pp_model_init(&model, 0U);
    model.fullness = 25U;
    assert(pp_model_tick(&model, 180U) == PP_EVENT_HUNGRY);
    assert(pp_model_tick(&model, 180U) == PP_EVENT_NONE);

    // A recovery above the threshold re-arms the edge.
    model.fullness = 30U;
    assert(pp_model_tick(&model, 180U) == PP_EVENT_NONE); // 30 -> 29
    model.fullness = 25U;
    assert(pp_model_tick(&model, 180U) == PP_EVENT_HUNGRY);
}

static void test_sad_events(void)
{
    pp_model_t model;
    pp_model_t rescued;

    // Two low stats on the same tick enter the sad condition exactly once.
    pp_model_init(&model, 0U);
    model.fullness = 24U;
    model.mood = 25U;
    assert(pp_model_tick(&model, 240U) == PP_EVENT_SAD);
    assert(model.fullness == 23U);
    assert(model.mood == 24U);
    assert(model.sad_active);
    assert(model.neglect_events == 1U);
    assert(pp_model_expression(&model) == PP_EXPR_SAD);

    assert(pp_model_tick(&model, 0U) == PP_EVENT_NONE);
    assert(model.neglect_events == 1U);

    // Feeding lifts one stat out of the low band; the next tick reports recovery.
    assert(pp_model_feed(&model) == PP_RESULT_OK);
    assert(model.fullness == 48U);
    assert(pp_model_tick(&model, 0U) == PP_EVENT_RECOVERED);
    assert(!model.sad_active);
    assert(model.care_count == 1U);
    assert(pp_model_tick(&model, 0U) == PP_EVENT_NONE);

    // A zero stat is sad even when the other two are healthy.
    pp_model_init(&rescued, 0U);
    rescued.fullness = 0U;
    assert(pp_model_tick(&rescued, 0U) == PP_EVENT_SAD);
    assert(pp_model_expression(&rescued) == PP_EXPR_SAD);

    // Playing out of the sad band also recovers.
    pp_model_init(&rescued, 0U);
    rescued.fullness = 24U;
    rescued.mood = 24U;
    assert(pp_model_tick(&rescued, 0U) == PP_EVENT_SAD);
    assert(pp_model_play(&rescued) == PP_RESULT_OK); // mood 46, fullness 19
    assert(pp_model_tick(&rescued, 0U) == PP_EVENT_RECOVERED);
    assert(!rescued.sad_active);
    assert(rescued.neglect_events == 1U);
}

static void test_expression_precedence(void)
{
    pp_model_t model;

    pp_model_init(&model, 0U);
    assert(pp_model_expression(&model) == PP_EXPR_HAPPY);

    model.fullness = 60U;
    model.mood = 60U;
    model.energy = 60U;
    assert(pp_model_expression(&model) == PP_EXPR_HAPPY);

    model.fullness = 59U;
    assert(pp_model_expression(&model) == PP_EXPR_CONTENT);

    model.fullness = 40U;
    model.mood = 40U;
    model.energy = 40U;
    assert(pp_model_expression(&model) == PP_EXPR_CONTENT);

    model.fullness = 24U;
    model.mood = 80U;
    model.energy = 80U;
    assert(pp_model_expression(&model) == PP_EXPR_HUNGRY);

    model.fullness = 80U;
    model.energy = 24U;
    assert(pp_model_expression(&model) == PP_EXPR_TIRED);

    // Two low stats outrank the single-need expressions.
    model.fullness = 24U;
    model.mood = 80U;
    model.energy = 24U;
    assert(pp_model_expression(&model) == PP_EXPR_SAD);

    model.fullness = 10U;
    model.mood = 10U;
    model.energy = 90U;
    assert(pp_model_expression(&model) == PP_EXPR_SAD);

    // Any zero stat is sad regardless of the other two.
    model.fullness = 0U;
    model.mood = 100U;
    model.energy = 100U;
    assert(pp_model_expression(&model) == PP_EXPR_SAD);

    // A single low stat only reports its own need, even at 100 elsewhere.
    model.fullness = 24U;
    model.mood = 100U;
    model.energy = 100U;
    assert(pp_model_expression(&model) == PP_EXPR_HUNGRY);

    // Sad is checked before TIRED, so low energy alone cannot mask two low stats.
    model.fullness = 20U;
    model.mood = 20U;
    model.energy = 0U;
    assert(pp_model_expression(&model) == PP_EXPR_SAD);
}

static void test_sanitize(void)
{
    pp_model_t model;

    // A matching version is usable and keeps its values.
    pp_model_init(&model, 7U);
    model.fullness = 42U;
    model.mood = 31U;
    model.energy = 17U;
    model.care_count = 9U;
    assert(pp_model_sanitize(&model));
    assert(model.version == PP_MODEL_VERSION);
    assert(model.fullness == 42U);
    assert(model.mood == 31U);
    assert(model.energy == 17U);
    assert(model.care_count == 9U);

    // Out-of-range fields from a corrupt blob are clamped, not trusted.
    model.fullness = 200U;
    model.mood = 255U;
    model.energy = 101U;
    model.feed_cooldown_s = 600U;
    model.rest_cooldown_s = 900U;
    model.second_accumulator = 7000U;
    assert(pp_model_sanitize(&model));
    assert(model.fullness == 100U);
    assert(model.mood == 100U);
    assert(model.energy == 100U);
    assert(model.feed_cooldown_s == 90U);
    assert(model.rest_cooldown_s == 120U);
    assert(model.second_accumulator < 3600U);

    // A restored sad pet must not emit a spurious transition on the first tick.
    pp_model_init(&model, 0U);
    model.fullness = 0U;
    model.sad_active = false;
    assert(pp_model_sanitize(&model));
    assert(model.sad_active);
    assert(pp_model_tick(&model, 0U) == PP_EVENT_NONE);

    pp_model_init(&model, 0U);
    model.sad_active = true; // stale flag from before the reboot
    assert(pp_model_sanitize(&model));
    assert(!model.sad_active);
    assert(pp_model_tick(&model, 0U) == PP_EVENT_NONE);

    // A foreign version is rejected and replaced by a fresh pet.
    pp_model_init(&model, 4321U);
    model.version = 99U;
    model.fullness = 0U;
    model.mood = 0U;
    model.energy = 0U;
    model.care_count = 55U;
    model.feed_cooldown_s = 40U;
    model.sad_active = true;
    assert(!pp_model_sanitize(&model));
    assert(model.version == PP_MODEL_VERSION);
    assert(model.fullness == 80U);
    assert(model.mood == 80U);
    assert(model.energy == 80U);
    assert(model.care_count == 0U);
    assert(model.feed_cooldown_s == 0U);
    assert(!model.sad_active);
    assert(model.seed == 4321U);
}

static void test_null_and_noop_inputs(void)
{
    pp_model_t model;

    pp_model_init(NULL, 0U);
    assert(!pp_model_sanitize(NULL));
    assert(pp_model_tick(NULL, 100U) == PP_EVENT_NONE);
    assert(pp_model_feed(NULL) == PP_RESULT_INVALID);
    assert(pp_model_play(NULL) == PP_RESULT_INVALID);
    assert(pp_model_rest(NULL) == PP_RESULT_INVALID);
    assert(pp_model_expression(NULL) == PP_EXPR_CONTENT);

    pp_model_init(&model, 0U);
    assert(pp_model_tick(&model, 0U) == PP_EVENT_NONE);
    assert(model.fullness == 80U);
    assert(model.mood == 80U);
    assert(model.energy == 80U);
}

int main(void)
{
    test_init_defaults();
    test_decay_exact_periods();
    test_decay_remainders_survive_split_ticks();
    test_decay_saturates_at_zero();
    test_feed();
    test_play();
    test_rest();
    test_cooldowns_tick_down();
    test_hungry_event_edge();
    test_sad_events();
    test_expression_precedence();
    test_sanitize();
    test_null_and_noop_inputs();

    puts("pocket_pal_model tests: PASS");
    return 0;
}
