#include "pocket_pal_model.h"

// Decay periods and action tuning. Keep in sync with docs/pocket-pal-design.md.
#define PP_STAT_MAX 100U
#define PP_STAT_START 80U

#define PP_FULLNESS_DECAY_S 180U
#define PP_MOOD_DECAY_S 240U
#define PP_ENERGY_DECAY_S 300U

// Least common multiple of the three decay periods. Reducing the shared
// accumulator modulo this value keeps each stat's remainder exact, so one long
// tick decays identically to the same span split into short ticks.
#define PP_DECAY_CYCLE_S 3600U

#define PP_FEED_AMOUNT 25U
#define PP_FEED_FULL_LIMIT 95U
#define PP_FEED_COOLDOWN_S 90U
#define PP_PLAY_MOOD_GAIN 22U
#define PP_PLAY_ENERGY_COST 12U
#define PP_PLAY_FULLNESS_COST 5U
#define PP_PLAY_MIN_ENERGY 15U
#define PP_REST_ENERGY_GAIN 30U
#define PP_REST_COOLDOWN_S 120U

#define PP_NEED_LOW 25U  // below this a stat shows a need icon
#define PP_HAPPY_MIN 60U // at or above this a stat counts as fully content

static uint8_t pp_clamp_add(uint8_t value, uint32_t amount)
{
    uint32_t sum = (uint32_t)value + amount;
    return (uint8_t)(sum > PP_STAT_MAX ? PP_STAT_MAX : sum);
}

static uint8_t pp_clamp_sub(uint8_t value, uint32_t amount)
{
    return value > amount ? (uint8_t)(value - amount) : 0U;
}

static bool pp_is_sad(const pp_model_t *model)
{
    uint32_t low = 0U;

    if (model->fullness == 0U || model->mood == 0U || model->energy == 0U) {
        return true;
    }
    low += model->fullness < PP_NEED_LOW ? 1U : 0U;
    low += model->mood < PP_NEED_LOW ? 1U : 0U;
    low += model->energy < PP_NEED_LOW ? 1U : 0U;
    return low >= 2U;
}

// Cooldowns do not bank overflow: seconds spent past the end of a cooldown are
// not carried towards the next one.
static void pp_tick_down(uint16_t *remaining_seconds, uint32_t elapsed_seconds)
{
    if (elapsed_seconds >= *remaining_seconds) {
        *remaining_seconds = 0U;
    } else {
        *remaining_seconds = (uint16_t)(*remaining_seconds - elapsed_seconds);
    }
}

// Decays every stat from the shared second accumulator. The accumulator holds
// cumulative active seconds reduced modulo PP_DECAY_CYCLE_S, which no stat loses
// because every period divides that value; the decay charged to this call is the
// difference between the periods completed before and after it.
static void pp_decay_all(pp_model_t *model, uint32_t elapsed_seconds)
{
    uint64_t total = (uint64_t)model->second_accumulator + elapsed_seconds;
    uint64_t before = model->second_accumulator;

    model->fullness = pp_clamp_sub(model->fullness,
                                   (uint32_t)(total / PP_FULLNESS_DECAY_S -
                                              before / PP_FULLNESS_DECAY_S));
    model->mood = pp_clamp_sub(model->mood,
                               (uint32_t)(total / PP_MOOD_DECAY_S -
                                          before / PP_MOOD_DECAY_S));
    model->energy = pp_clamp_sub(model->energy,
                                 (uint32_t)(total / PP_ENERGY_DECAY_S -
                                            before / PP_ENERGY_DECAY_S));
    model->second_accumulator = (uint32_t)(total % PP_DECAY_CYCLE_S);
}

void pp_model_init(pp_model_t *model, uint32_t seed)
{
    if (model == NULL) {
        return;
    }
    *model = (pp_model_t){0};
    model->version = PP_MODEL_VERSION;
    model->fullness = PP_STAT_START;
    model->mood = PP_STAT_START;
    model->energy = PP_STAT_START;
    model->seed = seed;
}

bool pp_model_sanitize(pp_model_t *model)
{
    if (model == NULL) {
        return false;
    }
    if (model->version != PP_MODEL_VERSION) {
        pp_model_init(model, model->seed);
        return false;
    }

    // A corrupt blob can carry any byte value. Clamp instead of discarding a pet
    // whose counters survived, and fold an out-of-range remainder back into one
    // decay cycle rather than replaying it against the stats.
    model->fullness = model->fullness > PP_STAT_MAX ? PP_STAT_MAX : model->fullness;
    model->mood = model->mood > PP_STAT_MAX ? PP_STAT_MAX : model->mood;
    model->energy = model->energy > PP_STAT_MAX ? PP_STAT_MAX : model->energy;
    if (model->feed_cooldown_s > PP_FEED_COOLDOWN_S) {
        model->feed_cooldown_s = PP_FEED_COOLDOWN_S;
    }
    if (model->rest_cooldown_s > PP_REST_COOLDOWN_S) {
        model->rest_cooldown_s = PP_REST_COOLDOWN_S;
    }
    model->second_accumulator %= PP_DECAY_CYCLE_S;

    // Align the edge detector with the restored stats so the first tick after a
    // load does not report a transition that already happened before the reboot.
    model->sad_active = pp_is_sad(model);
    return true;
}

uint32_t pp_model_tick(pp_model_t *model, uint32_t elapsed_seconds)
{
    uint32_t events = PP_EVENT_NONE;
    uint8_t fullness_before;
    bool sad_before;

    if (model == NULL) {
        return PP_EVENT_NONE;
    }
    fullness_before = model->fullness;
    sad_before = model->sad_active;

    pp_tick_down(&model->feed_cooldown_s, elapsed_seconds);
    pp_tick_down(&model->rest_cooldown_s, elapsed_seconds);
    pp_decay_all(model, elapsed_seconds);

    if (fullness_before >= PP_NEED_LOW && model->fullness < PP_NEED_LOW) {
        events |= PP_EVENT_HUNGRY;
    }

    model->sad_active = pp_is_sad(model);
    if (model->sad_active && !sad_before) {
        events |= PP_EVENT_SAD;
        model->neglect_events++;
    } else if (!model->sad_active && sad_before) {
        events |= PP_EVENT_RECOVERED;
    }
    return events;
}

pp_result_t pp_model_feed(pp_model_t *model)
{
    if (model == NULL) {
        return PP_RESULT_INVALID;
    }
    if (model->feed_cooldown_s > 0U) {
        return PP_RESULT_COOLDOWN;
    }
    if (model->fullness >= PP_FEED_FULL_LIMIT) {
        return PP_RESULT_FULL;
    }
    model->fullness = pp_clamp_add(model->fullness, PP_FEED_AMOUNT);
    model->feed_cooldown_s = PP_FEED_COOLDOWN_S;
    model->care_count++;
    return PP_RESULT_OK;
}

pp_result_t pp_model_play(pp_model_t *model)
{
    if (model == NULL) {
        return PP_RESULT_INVALID;
    }
    if (model->energy < PP_PLAY_MIN_ENERGY) {
        return PP_RESULT_NEED_ENERGY;
    }
    model->mood = pp_clamp_add(model->mood, PP_PLAY_MOOD_GAIN);
    model->energy = pp_clamp_sub(model->energy, PP_PLAY_ENERGY_COST);
    model->fullness = pp_clamp_sub(model->fullness, PP_PLAY_FULLNESS_COST);
    model->care_count++;
    return PP_RESULT_OK;
}

pp_result_t pp_model_rest(pp_model_t *model)
{
    if (model == NULL) {
        return PP_RESULT_INVALID;
    }
    if (model->rest_cooldown_s > 0U) {
        return PP_RESULT_COOLDOWN;
    }
    model->energy = pp_clamp_add(model->energy, PP_REST_ENERGY_GAIN);
    model->rest_cooldown_s = PP_REST_COOLDOWN_S;
    model->care_count++;
    return PP_RESULT_OK;
}

pp_expression_t pp_model_expression(const pp_model_t *model)
{
    if (model == NULL) {
        return PP_EXPR_CONTENT;
    }
    if (pp_is_sad(model)) {
        return PP_EXPR_SAD;
    }
    if (model->fullness < PP_NEED_LOW) {
        return PP_EXPR_HUNGRY;
    }
    if (model->energy < PP_NEED_LOW) {
        return PP_EXPR_TIRED;
    }
    if (model->fullness >= PP_HAPPY_MIN && model->mood >= PP_HAPPY_MIN &&
        model->energy >= PP_HAPPY_MIN) {
        return PP_EXPR_HAPPY;
    }
    return PP_EXPR_CONTENT;
}
