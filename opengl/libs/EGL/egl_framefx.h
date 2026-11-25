/*
 * SPDX-FileCopyrightText: The Yasvara Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <EGL/egl.h>

struct framefx_api;

namespace android {

struct egl_connection_t;
class egl_context_t;
class egl_display_t;
class egl_surface_t;

// framefx present hook (Luwes frame generation, Kelir) for processes Pentas armed.
//
// Arming is decided once, when the driver loads: an unarmed process never loads libframefx and
// its swaps only test armed(). In an armed one, each swap of a window surface goes to the
// engine, which keeps its per-context state in egl_context_t::framefx.
namespace framefx_egl {

extern const framefx_api* gEngine;

inline bool armed() {
    return gEngine != nullptr;
}

// After the driver and layers are loaded.
void init(egl_connection_t* cnx);

// Presents the current context's draw surface through the engine. False: swap as usual.
bool present(const egl_display_t* dp, egl_surface_t* s, egl_context_t* c, EGLBoolean* result);

EGLint swapInterval(EGLint requested);

// The surface's last frame went out redrawn by the engine: its buffer age reads 0.
bool redrawn(EGLSurface driverSurface);

void release(void* state);

// The attributes to create a context with: `asked`, or the engine's list written to `out`.
const EGLint* contextAttribs(const EGLint* asked, EGLint* out, int capacity);

} // namespace framefx_egl
} // namespace android
