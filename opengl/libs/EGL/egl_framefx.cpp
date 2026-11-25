/*
 * SPDX-FileCopyrightText: The Yasvara Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "egl_framefx.h"

#include <framefx/hook.h>
#include <log/log.h>

#include <unistd.h>

#include <cstring>

#include "egl_display.h"
#include "egl_object.h"
#include "egldefs.h"

namespace android {
namespace framefx_egl {

const framefx_api* gEngine = nullptr;

namespace {

egl_connection_t* gCnx = nullptr;
// The driver's GLES entry points as the loader filled them, before the engine's wrappers.
gl_hooks_t::gl_t gDriverGl;

// Driver entry points by name: EGL and GLES from the driver tables the loader filled, anything
// else from the driver's eglGetProcAddress.
void* proc(const char* name) {
    auto* egl = reinterpret_cast<EGLFuncPointer*>(&gCnx->egl);
    for (int i = 0; egl_names[i] != nullptr; i++) {
        if (strcmp(egl_names[i], name) == 0) return reinterpret_cast<void*>(egl[i]);
    }
    auto* gl = reinterpret_cast<EGLFuncPointer*>(&gDriverGl);
    for (int i = 0; gl_names[i] != nullptr; i++) {
        if (strcmp(gl_names[i], name) == 0) return reinterpret_cast<void*>(gl[i]);
    }
    return reinterpret_cast<void*>(gCnx->egl.eglGetProcAddress(name));
}

struct SwapTarget {
    const egl_display_t* dp;
    egl_surface_t* s;
};

EGLBoolean swap(void* cookie) {
    const auto* t = static_cast<const SwapTarget*>(cookie);
    return t->s->cnx->egl.eglSwapBuffers(t->dp->disp.dpy, t->s->surface);
}

} // namespace

void init(egl_connection_t* cnx) {
    // Runs under the driver-load lock. The zygote and system processes are never armed, and
    // must not decide for their forks.
    static bool decided = false;
    if (decided || cnx->dso == nullptr || !framefx::IsAppUid(getuid())) return;
    decided = true;
    // ANGLE draws through Vulkan; libvulkan's hook takes those frames.
    if (cnx->angleLoaded) return;
    const char* error = nullptr;
    const framefx_api* engine = framefx::LoadEngine(&error);
    if (engine == nullptr) {
        if (error != nullptr) ALOGE("framefx: armed but the engine did not load: %s", error);
        return;
    }
    gCnx = cnx;
    gDriverGl = cnx->hooks[egl_connection_t::GLESv2_INDEX]->gl;
    const framefx_gl_host host = {proc};
    engine->gl_attach(&host);
    // The engine's wrappers go into both dispatch tables the current context's thread reads.
    if (engine->abi >= 5 && engine->gl_intercept != nullptr) {
        int wrapped = 0;
        for (int index : {egl_connection_t::GLESv1_INDEX, egl_connection_t::GLESv2_INDEX}) {
            auto* gl = reinterpret_cast<EGLFuncPointer*>(&cnx->hooks[index]->gl);
            for (int i = 0; gl_names[i] != nullptr; i++) {
                void* own = engine->gl_intercept(gl_names[i], reinterpret_cast<void*>(gl[i]));
                if (own != nullptr) {
                    gl[i] = reinterpret_cast<EGLFuncPointer>(own);
                    wrapped++;
                }
            }
        }
        if (wrapped > 0) ALOGI("framefx: %d GLES entry points wrapped", wrapped);
    }
    gEngine = engine;
}

const EGLint* contextAttribs(const EGLint* asked, EGLint* out, int capacity) {
    if (gEngine->abi < 5 || gEngine->gl_context_attribs == nullptr) return asked;
    return gEngine->gl_context_attribs(asked, out, capacity);
}

bool present(const egl_display_t* dp, egl_surface_t* s, egl_context_t* c, EGLBoolean* result) {
    if (s->getNativeWindow() == nullptr) return false;
    SwapTarget target = {dp, s};
    const framefx_gl_frame frame = {
            .state = &c->framefx,
            .display = dp->disp.dpy,
            .surface = s->surface,
            .config = s->config,
            .window = s->getNativeWindow(),
            .swap = swap,
            .cookie = &target,
    };
    const int32_t presented = gEngine->gl_present(&frame);
    if (presented == FRAMEFX_PASS) return false;
    *result = static_cast<EGLBoolean>(presented);
    return true;
}

EGLint swapInterval(EGLint requested) {
    return gEngine->gl_swap_interval(requested);
}

bool redrawn(EGLSurface driverSurface) {
    return gEngine->gl_redrawn(driverSurface) != 0;
}

void release(void* state) {
    gEngine->gl_release(state);
}

} // namespace framefx_egl
} // namespace android
