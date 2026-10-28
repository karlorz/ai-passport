<p align="right">
  <a href="pocket-pal-design.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Pocket Pal Design (feature/pocket-pal)

Status: v1 scope baseline for implementation. Branch: `feature/pocket-pal`.
Chinese strings are written here as `\uXXXX` escapes; the paired Chinese
document shows the rendered text.

Pocket Pal is an original, compact digital pet for the FoloToy AI Passport. It
keeps the care-and-feedback idea of classic handheld pets and implements it with
its own screens, character, and interactions for three buttons and a 240 × 320
portrait display. It does not reuse the baseline hardware-test menu, screens, or
visual shell.

## Goals

- Understand what the pet needs within three seconds; finish one care action in
  at most three button presses.
- State persists across reboots (NVS snapshot) and freezes while powered off:
  the board has no confirmed independent RTC, so decay accrues from device
  uptime only.
- Neglect makes the pet sad, never dead; no irreversible failure and no data
  wipes.

## Pet and stats

One pet named \u56E2\u56E2 ("Tuan Tuan"), drawn procedurally (LVGL shapes, no
image assets), with an idle bob animation and expression changes.

| Stat | Range | Decay (active time) | Restored by |
| --- | --- | --- | --- |
| Fullness (`\u9971\u98DF`) | 0–100, starts 80 | −1 per 3 min | Feed +25 |
| Mood (`\u5FC3\u60C5`) | 0–100, starts 80 | −1 per 4 min | Play +22 |
| Energy (`\u7CBE\u529B`) | 0–100, starts 80 | −1 per 5 min | Rest +30 |

Expressions derive from stats: `HAPPY` (all ≥ 60), `CONTENT` (all ≥ 25),
`HUNGRY` (fullness < 25), `TIRED` (energy < 25), `SAD` (any stat at 0, or two
stats < 25).

## Controls

- Home screen: pet centered, pet name and battery indicator at top (battery
  degrades gracefully when unavailable), action bar at the bottom.
- `UP` / `DOWN` (click): move the action-bar highlight.
- `OK` (click): perform the highlighted action.
- `OK` (long press): toggle the status page.
- Action bar: `\u5582\u98DF` Feed · `\u73A9\u800D` Play · `\u4F11\u606F` Rest.

## Action rules (pure model)

- Feed: +25 fullness. Cooldown 90 s (`PP_RESULT_COOLDOWN`). Refused when
  fullness ≥ 95 (`PP_RESULT_FULL`).
- Play: needs energy ≥ 15 (`PP_RESULT_NEED_ENERGY` otherwise); +22 mood,
  −12 energy, −5 fullness.
- Rest: +30 energy. Cooldown 120 s.
- Events returned by the tick: `PP_EVENT_HUNGRY` when fullness crosses below
  25, `PP_EVENT_SAD` when entering the sad condition, `PP_EVENT_RECOVERED` when
  leaving it. Events drive a beep and a speech bubble, not a modal.
- Care counter increments on each successful action; shown on the status page.
- `pp_model_sanitize()` validates a loaded snapshot (version, range clamping).
- `CONFIG_POCKET_PAL_TIME_SCALE` (default 1) multiplies elapsed active seconds
  for development and acceptance, following the demo/tamagezi precedent.

## Screens (redesigned, not the test menu)

1. Home: pet area (center, animated), top bar (pet name left, battery right),
   need chips when a stat is < 25, speech bubble for reactions, bottom action
   bar with highlighted selection.
2. Status page (long-press OK): three labeled bars for fullness/mood/energy
   with values, `\u7167\u987E\u6B21\u6570` care count, and the hint
   `\u957F\u6309\u8FD4\u56DE` (long-press to return).

## Audio

Short synthesized beeps (16 kHz 16-bit mono) played from a worker task via
`bsp_audio_write()`; button callbacks only enqueue. Feed: rising two-tone.
Play: quick triplet. Rest: single low tone. Sad event: descending two-tone.

## Persistence

NVS namespace `pocket_pal`, one versioned blob (`PP_MODEL_VERSION 1`): stats,
cooldown remaining, care/neglect counters. Save after each action and at most
once per 60 s on tick; load and sanitize at boot; a corrupt or missing snapshot
starts a fresh pet.

## Strings inventory (fixed set, drives the font subset)

```text
\u56E2\u56E2                       pet name
\u5582\u98DF \u73A9\u800D \u4F11\u606F   actions: feed / play / rest
\u72B6\u6001                       status title
\u9971\u98DF \u5FC3\u60C5 \u7CBE\u529B   stats: fullness / mood / energy
\u7167\u987E\u6B21\u6570               care count
\u6211\u997F\u4E86                   hungry bubble
\u966A\u6211\u73A9                   play-with-me bubble
\u597D\u7D2F                       tired bubble
\u597D\u5403                       yummy reaction
\u597D\u5F00\u5FC3                   happy reaction
\u8FD8\u4E0D\u997F                   not-hungry refusal
\u592A\u7D2F\u4E86                   too-tired refusal
\u7A0D\u7B49\u4E00\u4E0B               cooldown refusal
\u957F\u6309\u8FD4\u56DE               long-press-to-return hint
```

Font route: the LVGL 9.5 built-in `lv_font_source_han_sans_sc_16_cjk` subset
was checked against this inventory and lacks 15 of the 35 unique characters
(including the pet name and the feed/play labels), so the app generates a
reproducible subset per Option B in the Chinese-font guide:
`tools/gen_pocket_pal_font.py` rasterizes Noto Sans SC (SIL OFL 1.1) into
`assets/fonts/pocket_pal_font_16.c`, with a stdlib-only `--check` coverage gate
in the static validation gate. The source font file is not committed.

## Acceptance

- Host: model unit tests (decay, actions, cooldowns, events, sanitize) in the
  static gate; static font-coverage check over the strings above.
- Device: boot shows the home screen with Chinese text rendered (no boxes);
  UP/DOWN move the highlight; OK performs actions with animation + beep;
  long-press OK toggles the status page; stats decay with uptime; reboot
  preserves stats; battery icon shown top-right.
