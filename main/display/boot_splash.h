#pragma once
/*
 * boot_splash.h - power-on animation for the OLED.
 *
 * Deliberately minimal: the wordmark "NexLink" slides in and settles, then
 * holds for a moment before the menu takes the panel over. No progress bar, no
 * status lines - those compete with the menu for attention and, on a 64px-tall
 * panel, for room.
 *
 * Kept out of menu_ui.c because it runs once, before the menu exists, and must
 * not be able to disturb menu state.
 */
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Play the splash. Safe when the panel is absent: every call is a framebuffer
 * write and oled_flush() fails harmlessly.
 *
 *   ms   approximate duration of the motion, before the hold
 */
void boot_splash_run(int ms);

/* Draw the settled wordmark (plus the address if one is known) without motion. */
void boot_splash_ready(const char *ip);

#ifdef __cplusplus
}
#endif
