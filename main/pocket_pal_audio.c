#include "pocket_pal_audio.h"

#include <stdint.h>

#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define SAMPLE_RATE 16000U
#define CHUNK_SAMPLES 256U
#define QUEUE_DEPTH 4

// Peak sample of a cue. The codec runs at a modest output volume, so the tone
// itself is kept well below full scale to stay a friendly beep rather than a
// click when the amplifier is turned up by a future setting.
#define TONE_AMPLITUDE 8000

// 12.5% of a note is spent fading in and the same span fading out. Without it a
// square-edged sine starts and stops mid-cycle and the speaker pops.
#define ENVELOPE_DIVISOR 8U

static const char *TAG = "pp_audio";

// One cycle of sin(2*pi*i/128) scaled to int16 range. A note indexes this table
// with the top seven bits of its 32-bit phase accumulator.
static const int16_t SINE[128] = {
         0,   1608,   3212,   4808,   6393,   7962,   9512,  11039,
     12539,  14010,  15446,  16846,  18204,  19519,  20787,  22005,
     23170,  24279,  25329,  26319,  27245,  28105,  28898,  29621,
     30273,  30852,  31356,  31785,  32137,  32412,  32609,  32728,
     32767,  32728,  32609,  32412,  32137,  31785,  31356,  30852,
     30273,  29621,  28898,  28105,  27245,  26319,  25329,  24279,
     23170,  22005,  20787,  19519,  18204,  16846,  15446,  14010,
     12539,  11039,   9512,   7962,   6393,   4808,   3212,   1608,
         0,  -1608,  -3212,  -4808,  -6393,  -7962,  -9512, -11039,
    -12539, -14010, -15446, -16846, -18204, -19519, -20787, -22005,
    -23170, -24279, -25329, -26319, -27245, -28105, -28898, -29621,
    -30273, -30852, -31356, -31785, -32137, -32412, -32609, -32728,
    -32767, -32728, -32609, -32412, -32137, -31785, -31356, -30852,
    -30273, -29621, -28898, -28105, -27245, -26319, -25329, -24279,
    -23170, -22005, -20787, -19519, -18204, -16846, -15446, -14010,
    -12539, -11039,  -9512,  -7962,  -6393,  -4808,  -3212,  -1608,
};

static QueueHandle_t s_queue;
static bool s_ready;

// Render one note in 256-sample chunks. bsp_audio_write blocks until the DMA
// buffer accepts the block, so this must only ever run on the tone worker.
static void play_note(uint16_t frequency, uint16_t milliseconds)
{
    if (frequency == 0U || milliseconds == 0U) return;

    const uint32_t total = SAMPLE_RATE * (uint32_t)milliseconds / 1000U;
    const uint32_t step = (uint32_t)(((uint64_t)frequency << 32) / SAMPLE_RATE);
    const uint32_t edge = total / ENVELOPE_DIVISOR + 1U;
    uint32_t phase = 0;
    uint32_t rendered = 0;
    int16_t samples[CHUNK_SAMPLES];

    while (rendered < total) {
        uint32_t count = total - rendered;
        if (count > CHUNK_SAMPLES) count = CHUNK_SAMPLES;

        for (uint32_t i = 0; i < count; i++) {
            const uint32_t position = rendered + i;
            const uint32_t fade = position < edge ? position :
                                  total - position < edge ? total - position : edge;
            const int32_t gain = (int32_t)(fade * 256U / edge);
            const int32_t wave = (int32_t)SINE[(phase >> 25) & 0x7FU];
            samples[i] = (int16_t)(wave * TONE_AMPLITUDE / 32767 * gain / 256);
            phase += step;
        }

        if (bsp_audio_write(samples, count * sizeof(samples[0])) != ESP_OK) {
            return;
        }
        rendered += count;
    }
}

// Gap between the notes of a multi-tone cue.
static void gap(uint16_t milliseconds)
{
    vTaskDelay(pdMS_TO_TICKS(milliseconds));
}

// Note lengths beyond the specified pitch and order are a design choice: they
// keep every cue under half a second so a burst of key presses cannot queue up
// behind a long melody.
static void play_cue(pp_sound_t sound)
{
    switch (sound) {
    case PP_SND_MOVE:
        play_note(440, 40);
        break;
    case PP_SND_FEED:
        play_note(523, 90);
        gap(20);
        play_note(659, 120);
        break;
    case PP_SND_PLAY:
        play_note(587, 70);
        play_note(740, 70);
        play_note(880, 110);
        break;
    case PP_SND_REST:
        play_note(330, 250);
        break;
    case PP_SND_REFUSED:
        play_note(220, 120);
        break;
    case PP_SND_CALL:
        play_note(880, 90);
        gap(80);
        play_note(880, 90);
        break;
    case PP_SND_SAD:
        play_note(392, 140);
        play_note(311, 200);
        break;
    }
}

static void audio_task(void *arg)
{
    (void)arg;

    if (bsp_audio_set_format(SAMPLE_RATE, 16, 1) != ESP_OK) {
        ESP_LOGE(TAG, "cannot select 16 kHz mono; cues stay silent");
    }

    for (;;) {
        uint8_t request = 0;
        if (xQueueReceive(s_queue, &request, portMAX_DELAY) == pdTRUE) {
            play_cue((pp_sound_t)request);
        }
    }
}

bool pp_audio_start(void)
{
    if (bsp_audio_init() != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 init failed; audio unavailable");
        s_ready = false;
        return false;
    }
    bsp_audio_set_volume(60);

    if (s_queue) {
        s_ready = true;
        return true;
    }
    s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(uint8_t));
    if (!s_queue) return false;
    if (xTaskCreate(audio_task, "pp_audio", 3072, NULL, 4, NULL) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return false;
    }
    s_ready = true;
    return true;
}

void pp_audio_play(pp_sound_t sound)
{
    if (!s_ready || !s_queue) return;
    const uint8_t request = (uint8_t)sound;
    // Callers include the button callback on the shared esp_timer task, so this
    // must never wait for room in the queue.
    (void)xQueueSend(s_queue, &request, 0);
}
