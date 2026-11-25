/*
 * SPDX-FileCopyrightText: The Yasvara Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "egl_render_scale.h"

#include <log/log.h>
#include <system/window.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

#include "../hooks.h"
#include "egl_object.h"
#include "egl_tls.h"
#include "egldefs.h"

namespace android {

extern void setGLHooksThreadSpecific(gl_hooks_t const* value);

namespace render_scale {
namespace {

std::mutex sTableMutex;
// The GLESv2 table with the wrappers below in place; built the first time a context scales.
gl_hooks_t sScaledHooks;
std::atomic<bool> sScaledHooksBuilt{false};
// What the wrappers call: the GLESv2 entries they replaced (driver or GLES layer).
gl_hooks_t::gl_t sNext;

ContextState* current() {
    egl_context_t* c = get_context(egl_tls_t::getContext());
    return c != nullptr && c->renderScale.scaled ? &c->renderScale : nullptr;
}

GLint scaleX(const ContextState& st, GLint x) {
    return static_cast<GLint>(std::lround(x * st.sx));
}

GLint scaleY(const ContextState& st, GLint y) {
    return static_cast<GLint>(std::lround(y * st.sy));
}

// Edges are scaled, not sizes, so rects that touch in game pixels still touch.
void scaleBox(const ContextState& st, const GLint in[4], GLint out[4]) {
    const GLint x0 = scaleX(st, in[0]);
    const GLint y0 = scaleY(st, in[1]);
    out[0] = x0;
    out[1] = y0;
    out[2] = scaleX(st, in[0] + in[2]) - x0;
    out[3] = scaleY(st, in[1] + in[3]) - y0;
}

void applyBox(const ContextState& st, const GLint box[4], GLint applied[4],
              void (*set)(GLint, GLint, GLsizei, GLsizei)) {
    GLint out[4];
    if (st.scaled && st.drawFb == 0) {
        scaleBox(st, box, out);
    } else {
        std::copy(box, box + 4, out);
    }
    if (std::equal(out, out + 4, applied)) return;
    std::copy(out, out + 4, applied);
    set(out[0], out[1], out[2], out[3]);
}

void applyViewportAndScissor(ContextState& st) {
    applyBox(st, st.viewport, st.appliedViewport, sNext.glViewport);
    applyBox(st, st.scissor, st.appliedScissor, sNext.glScissor);
}

GLenum defaultColorFormat() {
    GLint r = 8, g = 8, b = 8, a = 8;
    sNext.glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_BACK,
                                                GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE, &r);
    sNext.glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_BACK,
                                                GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE, &g);
    sNext.glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_BACK,
                                                GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE, &b);
    sNext.glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_BACK,
                                                GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE, &a);
    if (r == 5 && g == 6 && b == 5) return GL_RGB565;
    if (r == 10 && g == 10 && b == 10) return GL_RGB10_A2;
    if (r == 16) return GL_RGBA16F;
    return a > 0 ? GL_RGBA8 : GL_RGB8;
}

// A game-resolution color buffer, at least width x height. Leaves both framebuffer bindings to
// the caller.
bool ensureScratch(ContextState& st, GLsizei width, GLsizei height) {
    if (st.esMajor < 3 || width <= 0 || height <= 0) return false;
    if (st.scratchFb != 0 && width <= st.scratchWidth && height <= st.scratchHeight) return true;

    GLint renderbuffer = 0;
    sNext.glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
    if (st.scratchRb == 0) sNext.glGenRenderbuffers(1, &st.scratchRb);
    if (st.scratchFb == 0) sNext.glGenFramebuffers(1, &st.scratchFb);
    const GLsizei w = std::max(width, st.scratchWidth);
    const GLsizei h = std::max(height, st.scratchHeight);
    sNext.glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    const GLenum format = defaultColorFormat();
    sNext.glBindRenderbuffer(GL_RENDERBUFFER, st.scratchRb);
    sNext.glRenderbufferStorage(GL_RENDERBUFFER, format, w, h);
    sNext.glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
    sNext.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, st.scratchFb);
    sNext.glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                                    st.scratchRb);
    sNext.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, st.drawFb);
    sNext.glBindFramebuffer(GL_READ_FRAMEBUFFER, st.readFb);
    st.scratchWidth = w;
    st.scratchHeight = h;
    return true;
}

void releaseScratch(ContextState& st) {
    if (st.scratchFb != 0) sNext.glDeleteFramebuffers(1, &st.scratchFb);
    if (st.scratchRb != 0) sNext.glDeleteRenderbuffers(1, &st.scratchRb);
    st.scratchFb = st.scratchRb = 0;
    st.scratchWidth = st.scratchHeight = 0;
}

// Blits with the scissor test and rasterizer discard off, as a copy must not be clipped.
void unclippedBlit(GLint s0, GLint s1, GLint s2, GLint s3, GLint d0, GLint d1, GLint d2, GLint d3,
                   GLbitfield mask, GLenum filter) {
    const GLboolean scissor = sNext.glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean discard = sNext.glIsEnabled(GL_RASTERIZER_DISCARD);
    if (scissor) sNext.glDisable(GL_SCISSOR_TEST);
    if (discard) sNext.glDisable(GL_RASTERIZER_DISCARD);
    sNext.glBlitFramebuffer(s0, s1, s2, s3, d0, d1, d2, d3, mask, filter);
    if (scissor) sNext.glEnable(GL_SCISSOR_TEST);
    if (discard) sNext.glEnable(GL_RASTERIZER_DISCARD);
}

// Upscales the default framebuffer's game-pixel rect into the scratch buffer and binds that for
// reading, so a read of (0, 0, width, height) returns what the game expects. Undo with
// endScratchRead.
bool beginScratchRead(ContextState& st, GLint x, GLint y, GLsizei width, GLsizei height) {
    if (!ensureScratch(st, width, height)) return false;
    sNext.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, st.scratchFb);
    unclippedBlit(scaleX(st, x), scaleY(st, y), scaleX(st, x + width), scaleY(st, y + height), 0,
                  0, width, height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    sNext.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, st.drawFb);
    sNext.glBindFramebuffer(GL_READ_FRAMEBUFFER, st.scratchFb);
    return true;
}

void endScratchRead(const ContextState& st) {
    sNext.glBindFramebuffer(GL_READ_FRAMEBUFFER, st.readFb);
}

GLint samplesOf(const ContextState& st, GLuint framebuffer) {
    GLint samples = 0;
    sNext.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
    sNext.glGetIntegerv(GL_SAMPLES, &samples);
    sNext.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, st.drawFb);
    return samples;
}

void logOnce(const char* what) {
    static std::atomic<bool> sLogged{false};
    if (!sLogged.exchange(true)) ALOGW("render scale: %s on the scaled default framebuffer", what);
}

// ---- wrappers; bound only while the current context is scaled ----

void bindFramebuffer(GLenum target, GLuint framebuffer) {
    sNext.glBindFramebuffer(target, framebuffer);
    ContextState* st = current();
    if (st == nullptr) return;
    const bool wasDefault = st->drawFb == 0;
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) st->drawFb = framebuffer;
    if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER) st->readFb = framebuffer;
    if (wasDefault != (st->drawFb == 0)) applyViewportAndScissor(*st);
}

void deleteFramebuffers(GLsizei n, const GLuint* framebuffers) {
    sNext.glDeleteFramebuffers(n, framebuffers);
    ContextState* st = current();
    if (st == nullptr || n <= 0 || framebuffers == nullptr) return;
    const bool wasDefault = st->drawFb == 0;
    for (GLsizei i = 0; i < n; i++) {
        if (framebuffers[i] == 0) continue;
        if (framebuffers[i] == st->drawFb) st->drawFb = 0;
        if (framebuffers[i] == st->readFb) st->readFb = 0;
    }
    if (wasDefault != (st->drawFb == 0)) applyViewportAndScissor(*st);
}

void viewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    ContextState* st = current();
    if (st == nullptr || width < 0 || height < 0) return sNext.glViewport(x, y, width, height);
    st->viewport[0] = x;
    st->viewport[1] = y;
    st->viewport[2] = width;
    st->viewport[3] = height;
    applyBox(*st, st->viewport, st->appliedViewport, sNext.glViewport);
}

void scissor(GLint x, GLint y, GLsizei width, GLsizei height) {
    ContextState* st = current();
    if (st == nullptr || width < 0 || height < 0) return sNext.glScissor(x, y, width, height);
    st->scissor[0] = x;
    st->scissor[1] = y;
    st->scissor[2] = width;
    st->scissor[3] = height;
    applyBox(*st, st->scissor, st->appliedScissor, sNext.glScissor);
}

void getIntegerv(GLenum pname, GLint* data) {
    ContextState* st = current();
    if (st != nullptr && data != nullptr && (pname == GL_VIEWPORT || pname == GL_SCISSOR_BOX)) {
        const GLint* box = pname == GL_VIEWPORT ? st->viewport : st->scissor;
        std::copy(box, box + 4, data);
        return;
    }
    sNext.glGetIntegerv(pname, data);
}

void getInteger64v(GLenum pname, GLint64* data) {
    ContextState* st = current();
    if (st != nullptr && data != nullptr && (pname == GL_VIEWPORT || pname == GL_SCISSOR_BOX)) {
        const GLint* box = pname == GL_VIEWPORT ? st->viewport : st->scissor;
        std::copy(box, box + 4, data);
        return;
    }
    sNext.glGetInteger64v(pname, data);
}

void getFloatv(GLenum pname, GLfloat* data) {
    ContextState* st = current();
    if (st != nullptr && data != nullptr && (pname == GL_VIEWPORT || pname == GL_SCISSOR_BOX)) {
        const GLint* box = pname == GL_VIEWPORT ? st->viewport : st->scissor;
        std::copy(box, box + 4, data);
        return;
    }
    sNext.glGetFloatv(pname, data);
}

void blitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0,
                     GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter) {
    ContextState* st = current();
    if (st == nullptr || (st->readFb != 0 && st->drawFb != 0)) {
        return sNext.glBlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1,
                                       mask, filter);
    }
    if (st->drawFb == 0 && st->readFb != 0 && mask == GL_COLOR_BUFFER_BIT &&
        samplesOf(*st, st->readFb) > 0) {
        // A multisample resolve must be 1:1, so resolve at game size first.
        const GLsizei w = std::abs(srcX1 - srcX0), h = std::abs(srcY1 - srcY0);
        if (ensureScratch(*st, w, h)) {
            sNext.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, st->scratchFb);
            unclippedBlit(std::min(srcX0, srcX1), std::min(srcY0, srcY1),
                          std::min(srcX0, srcX1) + w, std::min(srcY0, srcY1) + h, 0, 0, w, h,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
            sNext.glBindFramebuffer(GL_READ_FRAMEBUFFER, st->scratchFb);
            sNext.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            // Keep the game's orientation of the source rect.
            const GLint s0 = srcX0 <= srcX1 ? 0 : w, s2 = srcX0 <= srcX1 ? w : 0;
            const GLint s1 = srcY0 <= srcY1 ? 0 : h, s3 = srcY0 <= srcY1 ? h : 0;
            sNext.glBlitFramebuffer(s0, s1, s2, s3, scaleX(*st, dstX0), scaleY(*st, dstY0),
                                    scaleX(*st, dstX1), scaleY(*st, dstY1), mask, filter);
            sNext.glBindFramebuffer(GL_READ_FRAMEBUFFER, st->readFb);
            return;
        }
    }
    if (st->readFb == 0) {
        srcX0 = scaleX(*st, srcX0);
        srcY0 = scaleY(*st, srcY0);
        srcX1 = scaleX(*st, srcX1);
        srcY1 = scaleY(*st, srcY1);
    }
    if (st->drawFb == 0) {
        dstX0 = scaleX(*st, dstX0);
        dstY0 = scaleY(*st, dstY0);
        dstX1 = scaleX(*st, dstX1);
        dstY1 = scaleY(*st, dstY1);
    }
    sNext.glBlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
}

void readPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type,
                void* pixels) {
    ContextState* st = current();
    if (st != nullptr && st->readFb == 0 && beginScratchRead(*st, x, y, width, height)) {
        sNext.glReadPixels(0, 0, width, height, format, type, pixels);
        endScratchRead(*st);
        return;
    }
    if (st != nullptr && st->readFb == 0) logOnce("glReadPixels");
    sNext.glReadPixels(x, y, width, height, format, type, pixels);
}

void readnPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type,
                 GLsizei bufSize, void* data) {
    ContextState* st = current();
    if (st != nullptr && st->readFb == 0 && beginScratchRead(*st, x, y, width, height)) {
        sNext.glReadnPixels(0, 0, width, height, format, type, bufSize, data);
        endScratchRead(*st);
        return;
    }
    if (st != nullptr && st->readFb == 0) logOnce("glReadnPixels");
    sNext.glReadnPixels(x, y, width, height, format, type, bufSize, data);
}

void copyTexImage2D(GLenum target, GLint level, GLenum internalformat, GLint x, GLint y,
                    GLsizei width, GLsizei height, GLint border) {
    ContextState* st = current();
    if (st != nullptr && st->readFb == 0 && beginScratchRead(*st, x, y, width, height)) {
        sNext.glCopyTexImage2D(target, level, internalformat, 0, 0, width, height, border);
        endScratchRead(*st);
        return;
    }
    if (st != nullptr && st->readFb == 0) logOnce("glCopyTexImage2D");
    sNext.glCopyTexImage2D(target, level, internalformat, x, y, width, height, border);
}

void copyTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y,
                       GLsizei width, GLsizei height) {
    ContextState* st = current();
    if (st != nullptr && st->readFb == 0 && beginScratchRead(*st, x, y, width, height)) {
        sNext.glCopyTexSubImage2D(target, level, xoffset, yoffset, 0, 0, width, height);
        endScratchRead(*st);
        return;
    }
    if (st != nullptr && st->readFb == 0) logOnce("glCopyTexSubImage2D");
    sNext.glCopyTexSubImage2D(target, level, xoffset, yoffset, x, y, width, height);
}

void copyTexSubImage3D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset,
                       GLint x, GLint y, GLsizei width, GLsizei height) {
    ContextState* st = current();
    if (st != nullptr && st->readFb == 0 && beginScratchRead(*st, x, y, width, height)) {
        sNext.glCopyTexSubImage3D(target, level, xoffset, yoffset, zoffset, 0, 0, width, height);
        endScratchRead(*st);
        return;
    }
    if (st != nullptr && st->readFb == 0) logOnce("glCopyTexSubImage3D");
    sNext.glCopyTexSubImage3D(target, level, xoffset, yoffset, zoffset, x, y, width, height);
}

void invalidateSubFramebuffer(GLenum target, GLsizei numAttachments, const GLenum* attachments,
                              GLint x, GLint y, GLsizei width, GLsizei height) {
    ContextState* st = current();
    const GLuint framebuffer =
            st == nullptr ? 1 : (target == GL_READ_FRAMEBUFFER ? st->readFb : st->drawFb);
    if (framebuffer == 0) {
        const GLint box[4] = {x, y, width, height};
        GLint out[4];
        scaleBox(*st, box, out);
        return sNext.glInvalidateSubFramebuffer(target, numAttachments, attachments, out[0],
                                                out[1], out[2], out[3]);
    }
    sNext.glInvalidateSubFramebuffer(target, numAttachments, attachments, x, y, width, height);
}

gl_hooks_t* scaledHooks(const egl_connection_t* cnx) {
    if (sScaledHooksBuilt.load(std::memory_order_acquire)) return &sScaledHooks;
    std::lock_guard lock(sTableMutex);
    if (sScaledHooksBuilt.load(std::memory_order_relaxed)) return &sScaledHooks;
    const gl_hooks_t* plain = cnx->hooks[egl_connection_t::GLESv2_INDEX];
    memcpy(&sScaledHooks, plain, sizeof(sScaledHooks));
    sNext = plain->gl;
    gl_hooks_t::gl_t& gl = sScaledHooks.gl;
    gl.glBindFramebuffer = bindFramebuffer;
    gl.glDeleteFramebuffers = deleteFramebuffers;
    gl.glViewport = viewport;
    gl.glScissor = scissor;
    gl.glGetIntegerv = getIntegerv;
    gl.glGetInteger64v = getInteger64v;
    gl.glGetFloatv = getFloatv;
    gl.glBlitFramebuffer = blitFramebuffer;
    gl.glReadPixels = readPixels;
    gl.glReadnPixels = readnPixels;
    gl.glCopyTexImage2D = copyTexImage2D;
    gl.glCopyTexSubImage2D = copyTexSubImage2D;
    gl.glCopyTexSubImage3D = copyTexSubImage3D;
    gl.glInvalidateSubFramebuffer = invalidateSubFramebuffer;
    sScaledHooksBuilt.store(true, std::memory_order_release);
    return &sScaledHooks;
}

int esMajorVersion() {
    const char* version = reinterpret_cast<const char*>(sNext.glGetString(GL_VERSION));
    int major = 2, minor = 0;
    if (version != nullptr) sscanf(version, "OpenGL ES %d.%d", &major, &minor);
    return major;
}

// Takes the game's GL state as it is now, in game pixels.
void capture(ContextState& st, int bufferWidth, int bufferHeight, int userWidth, int userHeight) {
    st.esMajor = esMajorVersion();
    GLint draw = 0, read = 0;
    sNext.glGetIntegerv(GL_FRAMEBUFFER_BINDING, &draw);
    if (st.esMajor >= 3) {
        sNext.glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
    } else {
        read = draw;
    }
    st.drawFb = static_cast<GLuint>(draw);
    st.readFb = static_cast<GLuint>(read);
    sNext.glGetIntegerv(GL_VIEWPORT, st.viewport);
    sNext.glGetIntegerv(GL_SCISSOR_BOX, st.scissor);
    std::copy(st.viewport, st.viewport + 4, st.appliedViewport);
    std::copy(st.scissor, st.scissor + 4, st.appliedScissor);
    // A context's first makeCurrent sets both to the buffer: that is the game's full size.
    const GLint buffer[4] = {0, 0, bufferWidth, bufferHeight};
    const GLint user[4] = {0, 0, userWidth, userHeight};
    if (std::equal(buffer, buffer + 4, st.viewport)) std::copy(user, user + 4, st.viewport);
    if (std::equal(buffer, buffer + 4, st.scissor)) std::copy(user, user + 4, st.scissor);
}

// The game's own size and the buffer size it is fitted into, when they differ.
bool fittedSize(ANativeWindow* window, int* userWidth, int* userHeight, int* width, int* height) {
    if (window == nullptr ||
        native_window_get_fitted_user_dimensions(window, userWidth, userHeight, width, height) !=
                0) {
        return false;
    }
    return *userWidth > 0 && *userHeight > 0 && *width > 0 && *height > 0 &&
            (*width != *userWidth || *height != *userHeight);
}

} // namespace

void update(egl_context_t* c) {
    if (c == nullptr) return;
    ContextState& st = c->renderScale;
    int userWidth = 0, userHeight = 0, width = 0, height = 0;
    const bool fitted = c->version == egl_connection_t::GLESv2_INDEX &&
            c->draw != EGL_NO_SURFACE &&
            fittedSize(get_surface(c->draw)->getNativeWindow(), &userWidth, &userHeight, &width,
                       &height);
    if (!fitted && !st.scaled) return;

    gl_hooks_t* scaled = scaledHooks(c->cnx);
    if (fitted && !st.scaled) {
        capture(st, width, height, userWidth, userHeight);
        ALOGI("render scale: %dx%d game frame -> %dx%d buffer", userWidth, userHeight, width,
              height);
    }
    if (fitted) {
        st.sx = static_cast<float>(width) / userWidth;
        st.sy = static_cast<float>(height) / userHeight;
    }
    st.scaled = fitted;
    applyViewportAndScissor(st);
    if (!fitted) {
        releaseScratch(st);
        ALOGI("render scale: game frame back to its own size");
    }
    setGLHooksThreadSpecific(fitted ? scaled : c->cnx->hooks[c->version]);
}

bool querySurfaceSize(ANativeWindow* window, EGLint attribute, EGLint* value) {
    if (attribute != EGL_WIDTH && attribute != EGL_HEIGHT) return false;
    int userWidth, userHeight, width, height;
    if (!fittedSize(window, &userWidth, &userHeight, &width, &height)) return false;
    *value = attribute == EGL_WIDTH ? userWidth : userHeight;
    return true;
}

bool scaleRects(const egl_context_t* c, const EGLint* rects, EGLint count,
                std::vector<EGLint>* out) {
    if (c == nullptr || !c->renderScale.scaled || rects == nullptr || count <= 0) return false;
    out->resize(static_cast<size_t>(count) * 4);
    for (EGLint i = 0; i < count; i++) {
        scaleBox(c->renderScale, rects + i * 4, out->data() + i * 4);
    }
    return true;
}

void onExtensionSlot(int slot, __eglMustCastToProperFunctionPointerType proc) {
    std::lock_guard lock(sTableMutex);
    if (sScaledHooksBuilt.load(std::memory_order_relaxed)) {
        sScaledHooks.ext.extensions[slot] = proc;
    }
}

} // namespace render_scale
} // namespace android
