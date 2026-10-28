// Pocket Pal care model: pure C11 with no ESP-IDF, LVGL, or FreeRTOS dependency.
//
// All state lives in pp_model_t so a snapshot can be stored in NVS, sanitized on
// load, and exercised by host tests. The model never reads a clock; time only
// advances through pp_model_tick(), so decay follows active device uptime and
// freezes while the board is powered off.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Bump whenever the meaning or layout of pp_model_t changes. Snapshots carrying
// any other version are rejected by pp_model_sanitize().
#define PP_MODEL_VERSION 1U

typedef enum {
    PP_RESULT_OK = 0,
    PP_RESULT_COOLDOWN,    // action is still cooling down
    PP_RESULT_FULL,        // pet refused food because it is nearly full
    PP_RESULT_NEED_ENERGY, // pet is too tired to play
    PP_RESULT_INVALID,     // null or unusable model
} pp_result_t;

// Bitmask returned by pp_model_tick().
#define PP_EVENT_NONE      0U
#define PP_EVENT_HUNGRY    (1U << 0) // fullness crossed below 25
#define PP_EVENT_SAD       (1U << 1) // sad condition just became true
#define PP_EVENT_RECOVERED (1U << 2) // sad condition just cleared

typedef enum {
    PP_EXPR_HAPPY = 0, // every stat at or above 60
    PP_EXPR_CONTENT,   // no need outstanding, but not all stats high
    PP_EXPR_HUNGRY,    // fullness below 25
    PP_EXPR_TIRED,     // energy below 25
    PP_EXPR_SAD,       // a stat is at 0, or two stats are below 25
} pp_expression_t;

typedef struct pp_model_t {
    uint32_t version;         // PP_MODEL_VERSION of this state
    uint8_t fullness;         // 0..100
    uint8_t mood;             // 0..100
    uint8_t energy;           // 0..100
    uint32_t care_count;      // successful care actions
    uint32_t neglect_events;  // times the pet entered the sad condition
    uint16_t feed_cooldown_s; // seconds remaining before the next feed
    uint16_t rest_cooldown_s; // seconds remaining before the next rest
    // Cumulative active seconds since the last full decay cycle, held modulo the
    // least common multiple of the three periods. Because every period divides
    // that value, the remainder for each stat stays exact and one long tick
    // decays exactly like the same span split into many short ticks.
    uint32_t second_accumulator;
    bool sad_active; // sad condition as of the last tick, for edge detection
    uint32_t seed;   // reserved for future variation; v1 behavior ignores it
} pp_model_t;

// Start a fresh pet: stats 80, no cooldowns, no counters.
void pp_model_init(pp_model_t *model, uint32_t seed);

// Validate a snapshot loaded from storage. Clamps stats to 0..100, folds the
// decay remainder back into range, and realigns sad_active so a restore does
// not emit a spurious edge. Returns true when the snapshot was usable; on a
// version mismatch (or a null pointer) the model is reinitialized as a fresh
// pet and false is returned.
bool pp_model_sanitize(pp_model_t *model);

// Advance decay and cooldowns by elapsed active seconds and report transitions
// as a PP_EVENT_* bitmask. Handles multi-hour spans in one call; remainders are
// preserved across calls.
uint32_t pp_model_tick(pp_model_t *model, uint32_t elapsed_seconds);

// Feed: +25 fullness, 90 s cooldown. Refused while cooling down or when
// fullness is at least 95.
pp_result_t pp_model_feed(pp_model_t *model);

// Play: +22 mood, -12 energy, -5 fullness. Refused below 15 energy.
pp_result_t pp_model_play(pp_model_t *model);

// Rest: +30 energy, 120 s cooldown.
pp_result_t pp_model_rest(pp_model_t *model);

// Expression for the current stats. Precedence: SAD, HUNGRY, TIRED, HAPPY,
// then CONTENT.
pp_expression_t pp_model_expression(const pp_model_t *model);
