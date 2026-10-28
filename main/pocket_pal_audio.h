#pragma once

#include <stdbool.h>

// Short cues for one care action or one state transition. The set is fixed so
// the tone worker never needs a data file, and so a missing codec can degrade to
// silence without changing the model.
typedef enum {
    PP_SND_MOVE = 0, // action-bar selection moved
    PP_SND_FEED,     // feed accepted
    PP_SND_PLAY,     // play accepted
    PP_SND_REST,     // rest accepted
    PP_SND_REFUSED,  // action rejected by the model
    PP_SND_CALL,     // pet asks for attention
    PP_SND_SAD,      // pet entered the sad condition
} pp_sound_t;

// Initialize the ES8311/I2S output and start the tone worker. Safe to call once
// at boot. Returns false when the codec cannot be brought up; every play call
// then becomes a no-op instead of blocking or failing loudly.
bool pp_audio_start(void);

// Queue one cue. Callable from any task: it enqueues and returns immediately, so
// it is safe inside button or LVGL callbacks. Drops the request when the queue is
// full or audio is unavailable.
void pp_audio_play(pp_sound_t sound);
