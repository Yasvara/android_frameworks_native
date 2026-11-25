/*
 * SPDX-FileCopyrightText: The Yasvara Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <EGL/egl.h>
#include <GLES3/gl32.h>

#include <vector>

struct ANativeWindow;

namespace android {

class egl_context_t;
struct egl_connection_t;

// Pentas render scale for GLES games that fix their own resolution.
//
// A game that set its size with ANativeWindow_setBuffersGeometry keeps drawing in that size even
// when its surface is scaled down. libgui fits such a size into the scaled buffer
// (native_window_get_fitted_user_dimensions); here the game's coordinates on the default
// framebuffer (viewport, scissor, blits, reads, damage) are scaled by the same factor, so the
// whole frame lands in the smaller buffer and the game renders fewer pixels.
//
// Only a context whose draw surface is fitted gets the scaled GL table; every other context runs
// the plain table, so nothing changes for it.
namespace render_scale {

// Per-context state. Only the thread the context is current on touches it.
struct ContextState {
    bool scaled = false;     // the scaled GL table is bound for this context
    int esMajor = 0;         // 0 until first scaled
    float sx = 1.f, sy = 1.f; // buffer pixels per game pixel on the default framebuffer
    GLuint drawFb = 0, readFb = 0;
    GLint viewport[4] = {};  // as the game set them, in game pixels
    GLint scissor[4] = {};
    GLint appliedViewport[4] = {-1, -1, -1, -1}; // last values sent to the driver
    GLint appliedScissor[4] = {-1, -1, -1, -1};
    // Scratch color buffer at game resolution, for reads and multisample resolves.
    GLuint scratchFb = 0, scratchRb = 0;
    GLsizei scratchWidth = 0, scratchHeight = 0;
};

// Picks the GL table for the context's draw surface. Call with c current, after eglMakeCurrent
// and after each eglSwapBuffers of its draw surface (the next frame's buffer size is final then).
void update(egl_context_t* c);

// EGL_WIDTH / EGL_HEIGHT of a fitted window surface: the game's own size, as before fitting.
bool querySurfaceSize(ANativeWindow* window, EGLint attribute, EGLint* value);

// Damage rects (x, y, w, h) in game pixels -> buffer pixels. False when c is not scaled.
bool scaleRects(const egl_context_t* c, const EGLint* rects, EGLint count,
                std::vector<EGLint>* out);

// Mirrors an extension slot resolved by eglGetProcAddress into the scaled table.
void onExtensionSlot(int slot, __eglMustCastToProperFunctionPointerType proc);

} // namespace render_scale
} // namespace android
