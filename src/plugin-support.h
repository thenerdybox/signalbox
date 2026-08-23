/*
 * SignalBox - lightweight OBS module logging helper.
 *
 * Copyright (c) 2026 TheNerdyBox
 * SPDX-License-Identifier: MIT (see LICENSE)
 *
 * This is glue required by every OBS plugin to prefix log lines with the
 * module name; it carries no plugin-specific behavior. Kept as a tiny
 * separate C translation unit (compiled outside the Qt/AUTOMOC pipeline)
 * so it stays trivially portable and dependency-free.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

extern const char *PLUGIN_NAME;
extern const char *PLUGIN_VERSION;

/* Thin wrapper around OBS's blogva() that prefixes every line with
 * "[signalbox]". log_level is one of the OBS LOG_* constants
 * from obs-module.h (LOG_ERROR, LOG_WARNING, LOG_INFO, LOG_DEBUG). */
void obs_log(int log_level, const char *format, ...);
extern void blogva(int log_level, const char *format, va_list args);

#ifdef __cplusplus
}
#endif
