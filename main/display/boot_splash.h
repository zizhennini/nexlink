#pragma once
/*
 * boot_splash.h - power-on animation for the OLED.
 *
 * Kept in its own small module rather than inside menu_ui.c because it is not
 * part of the menu: it runs once, before the menu exists, and must never be
 * able to disturb menu state. It can also be skipped entirely (headless boot,
 * or when a fast start matters more than the animation).
 */
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Play the splash. Safe to call when the panel is absent: every drawing call
 * is a framebuffer write and oled_flush() fails harmlessly.
 *
 *   ip      optional address shown under the wordmark; NULL or empty shows a
 *           generic line instead. Passed in rather than looked up here so the
 *           splash has no dependency on the WiFi manager.
 *   ms      approximate duration of the progress sweep
 */
void boot_splash_run(const char *ip, int ms);

/* Draw the static "ready" screen (wordmark + address) without the sweep. */
void boot_splash_ready(const char *ip);

#ifdef __cplusplus
}
#endif
