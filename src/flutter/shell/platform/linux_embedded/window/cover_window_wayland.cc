// Copyright 2026 Dave Nash. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux_embedded/window/cover_window_wayland.h"

#include <sys/mman.h>
#include <unistd.h>

#include <string>

#include "flutter/shell/platform/linux_embedded/logger.h"
#include "flutter/shell/platform/linux_embedded/window/qt_window_properties.h"

namespace flutter {

namespace {

// Window ids for linking the cover to the main window. Lipstick matches them
// only between windows of the same process, so small fixed numbers will do.
constexpr uint64_t kMainWindowId = 1;
constexpr uint64_t kCoverWindowId = 2;

}  // namespace

const wl_shell_surface_listener CoverWindowWayland::kShellSurfaceListener = {
    .ping =
        [](void* data, wl_shell_surface* wl_shell_surface, uint32_t serial) {
          wl_shell_surface_pong(wl_shell_surface, serial);
        },
    .configure =
        [](void* data,
           wl_shell_surface* wl_shell_surface,
           uint32_t edges,
           int32_t width,
           int32_t height) {
          auto self = reinterpret_cast<CoverWindowWayland*>(data);
          if (width <= 0 || height <= 0 ||
              (width == self->width_ && height == self->height_)) {
            return;
          }
          ELINUX_LOG(INFO) << "Cover size: " << width << "x" << height;
          self->width_ = width;
          self->height_ = height;
          self->on_changed_();
        },
    .popup_done = [](void* data, wl_shell_surface* wl_shell_surface) {},
};

const qt_extended_surface_listener
    CoverWindowWayland::kExtendedSurfaceListener = {
        .onscreen_visibility =
            [](void* data,
               qt_extended_surface* qt_extended_surface,
               int32_t visible) {
              auto self = reinterpret_cast<CoverWindowWayland*>(data);
              if ((visible != 0) == self->visible()) {
                self->visibility_ = visible;
                return;
              }
              self->visibility_ = visible;
              self->on_changed_();
            },
        .set_generic_property =
            [](void* data,
               qt_extended_surface* qt_extended_surface,
               const char* name,
               wl_array* value) {},
        .close = [](void* data, qt_extended_surface* qt_extended_surface) {},
};

const wl_buffer_listener CoverWindowWayland::kBufferListener = {
    .release =
        [](void* data, wl_buffer* wl_buffer) {
          auto self = reinterpret_cast<CoverWindowWayland*>(data);
          for (auto& buffer : self->buffers_) {
            if (buffer.buffer == wl_buffer) {
              buffer.busy = false;
            }
          }
          if (!self->pending_.empty()) {
            self->CommitPending();
          }
        },
};

CoverWindowWayland::CoverWindowWayland(
    wl_compositor* compositor,
    wl_shm* shm,
    wl_shell* shell,
    qt_surface_extension* surface_extension,
    wl_surface* main_surface,
    qt_extended_surface* main_extended_surface,
    const char* app_id,
    ChangedCallback on_changed)
    : shm_(shm), on_changed_(std::move(on_changed)) {
  surface_ = wl_compositor_create_surface(compositor);
  if (!surface_) {
    ELINUX_LOG(ERROR) << "Failed to create the cover surface.";
    return;
  }

  // Set up like a Silica cover window: a wl_shell surface transient for the
  // main window, with the app's class.
  shell_surface_ = wl_shell_get_shell_surface(shell, surface_);
  wl_shell_surface_add_listener(shell_surface_, &kShellSurfaceListener, this);
  extended_surface_ =
      qt_surface_extension_get_extended_surface(surface_extension, surface_);
  qt_extended_surface_add_listener(extended_surface_, &kExtendedSurfaceListener,
                                   this);
  wl_shell_surface_set_title(shell_surface_, "_CoverWindow");
  if (app_id != nullptr) {
    wl_shell_surface_set_class(shell_surface_, app_id);
  }
  wl_shell_surface_set_transient(shell_surface_, main_surface, 0, 0, 0);
  qt_extended_surface_set_content_orientation_mask(extended_surface_, 15);
  qt_extended_surface_set_window_flags(extended_surface_, 0);

  SetBoolWindowProperty(extended_surface_, "TRANSPARENT", false);
  SetULongLongWindowProperty(extended_surface_, "WINID", kCoverWindowId);
  SetStringWindowProperty(extended_surface_, "CATEGORY", "cover");

  SetULongLongWindowProperty(main_extended_surface, "WINID", kMainWindowId);
  SetStringWindowProperty(main_extended_surface, "SAILFISH_COVER_WINDOW",
                          "__winref:" + std::to_string(kCoverWindowId));
  SetBoolWindowProperty(main_extended_surface, "SAILFISH_HAVE_COVER", true);
}

CoverWindowWayland::~CoverWindowWayland() {
  if (extended_surface_) {
    qt_extended_surface_destroy(extended_surface_);
  }
  if (shell_surface_) {
    wl_shell_surface_destroy(shell_surface_);
  }
  if (surface_) {
    wl_surface_destroy(surface_);
  }
  DestroyBuffers();
}

void CoverWindowWayland::SetFrame(int32_t width,
                                  int32_t height,
                                  const uint8_t* rgba) {
  pending_.assign(rgba, rgba + static_cast<size_t>(width) * height * 4);
  pending_width_ = width;
  pending_height_ = height;
  CommitPending();
}

bool CoverWindowWayland::CreateBuffers(int32_t width, int32_t height) {
  const int32_t stride = width * 4;
  const size_t size = static_cast<size_t>(stride) * height;
  for (auto& buffer : buffers_) {
    const int fd = memfd_create("flutter-sfos-cover", MFD_CLOEXEC);
    if (fd < 0) {
      ELINUX_LOG(ERROR) << "Failed to create the cover buffer.";
      return false;
    }
    if (ftruncate(fd, size) != 0) {
      close(fd);
      ELINUX_LOG(ERROR) << "Failed to size the cover buffer.";
      return false;
    }
    void* data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
      close(fd);
      ELINUX_LOG(ERROR) << "Failed to map the cover buffer.";
      return false;
    }
    wl_shm_pool* pool = wl_shm_create_pool(shm_, fd, size);
    buffer.buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
                                              WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    buffer.data = data;
    buffer.size = size;
    buffer.busy = false;
    wl_buffer_add_listener(buffer.buffer, &kBufferListener, this);
  }
  buffer_width_ = width;
  buffer_height_ = height;
  return true;
}

void CoverWindowWayland::DestroyBuffers() {
  for (auto& buffer : buffers_) {
    if (buffer.buffer) {
      wl_buffer_destroy(buffer.buffer);
    }
    if (buffer.data) {
      munmap(buffer.data, buffer.size);
    }
    buffer = Buffer();
  }
  buffer_width_ = 0;
  buffer_height_ = 0;
}

void CoverWindowWayland::CommitPending() {
  if (pending_width_ != buffer_width_ || pending_height_ != buffer_height_) {
    DestroyBuffers();
    if (!CreateBuffers(pending_width_, pending_height_)) {
      DestroyBuffers();
      pending_.clear();
      return;
    }
  }
  Buffer* buffer = nullptr;
  for (auto& candidate : buffers_) {
    if (!candidate.busy) {
      buffer = &candidate;
      break;
    }
  }
  if (!buffer) {
    // Shown once a buffer is released.
    return;
  }

  // RGBA bytes to WL_SHM_FORMAT_ARGB8888, which is little-endian (B, G, R, A
  // in memory). Both are premultiplied.
  const uint8_t* src = pending_.data();
  auto* dst = static_cast<uint8_t*>(buffer->data);
  const size_t pixels = static_cast<size_t>(pending_width_) * pending_height_;
  for (size_t i = 0; i < pixels; i++, src += 4, dst += 4) {
    dst[0] = src[2];
    dst[1] = src[1];
    dst[2] = src[0];
    dst[3] = src[3];
  }
  pending_.clear();

  wl_surface_attach(surface_, buffer->buffer, 0, 0);
  wl_surface_damage(surface_, 0, 0, buffer_width_, buffer_height_);
  wl_surface_commit(surface_);
  buffer->busy = true;
}

}  // namespace flutter
