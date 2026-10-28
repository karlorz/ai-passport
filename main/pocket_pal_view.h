#pragma once

#include "lvgl.h"
#include "pocket_pal_model.h"

// Pocket Pal palette. Screens, chrome, and the pet all draw from these five
// colors so the application reads as one product instead of a themed demo.
#define PP_COLOR_BG    0xFFF7E8 // cream page
#define PP_COLOR_INK   0x3A2E2A // dark brown text and outlines
#define PP_COLOR_CORAL 0xFF8A65 // selection, low-stat warnings, food
#define PP_COLOR_MINT  0x7FC8A9 // pet body and healthy bars
#define PP_COLOR_PINK  0xFFC1CC // cheeks, hearts, bar tracks
#define PP_COLOR_WHITE 0xFFFFFF

// Short one-shot animations. Each overlay cleans itself up when its animation
// finishes, so the caller never owns any reaction object.
typedef enum {
    PP_REACT_FEED = 0, // a food dot falls onto the pet
    PP_REACT_PLAY,     // hearts float up
    PP_REACT_REST,     // "Zzz" drifts away
} pp_react_t;

// Build the pet at x,y inside parent. The returned container owns the idle bob,
// the blink loop, and every reaction overlay; deleting it stops all of them
// because LVGL removes animations whose target is being destroyed.
lv_obj_t *pp_pet_create(lv_obj_t *parent, int x, int y);

// Apply an expression. Repeated calls with the current expression are ignored so
// a periodic refresh cannot restart animations or churn styles.
void pp_pet_set_expression(lv_obj_t *pet, pp_expression_t expr);

// Play a one-shot reaction. Safe to call while another reaction is running.
void pp_pet_react(lv_obj_t *pet, pp_react_t react);
