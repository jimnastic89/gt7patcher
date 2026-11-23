/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stddef.h>

/* Gameplay modifiers that edit GT7's loaded data tables (no code patches).
 * Settings live in <root>/mods.json; a background thread applies them to every
 * supported GT7 session and keeps them in place. */
void mods_start(const char *root);
/* Writes the current settings and live state as JSON. Returns the length or -1. */
int mods_status_json(char *out, size_t size);
/* Parses a JSON settings body, saves it and applies it to a running game.
 * Returns 0 on success, -1 on a malformed body, -2 if it could not be saved. */
int mods_save(const char *body);
