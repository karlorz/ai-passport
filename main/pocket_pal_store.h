#pragma once

#include <stdbool.h>

#include "pocket_pal_model.h"

// Bring up NVS and start the save worker. Also runs the NVS recovery path when
// the partition carries an unreadable or older layout. Returns false when
// storage is unusable; the caller then runs on a volatile in-memory pet.
bool pp_store_init(void);

// Load the newest valid snapshot into model and sanitize it. Returns false when
// no usable snapshot exists, leaving the model untouched.
bool pp_store_load(pp_model_t *model);

// Hand a snapshot to the save worker, which coalesces pending writes and keeps
// only the latest. Never blocks and never touches NVS on the caller's task.
bool pp_store_save_async(const pp_model_t *model);
