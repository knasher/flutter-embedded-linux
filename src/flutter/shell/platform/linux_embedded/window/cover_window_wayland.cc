// Copyright 2026 Dave Nash. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux_embedded/window/cover_window_wayland.h"

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
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
          ELINUX_LOG(INFO) << "Cover configure: " << width << "x" << height;
          auto self = reinterpret_cast<CoverWindowWayland*>(data);
          if (width <= 0 || height <= 0 ||
              (width == self->width_ && height == self->height_)) {
            return;
          }
          self->width_ = width;
          self->height_ = height;
          self->DestroyBuffers();
          if (self->CreateBuffers()) {
            self->Draw();
          }
        },
    .popup_done = [](void* data, wl_shell_surface* wl_shell_surface) {},
};

const qt_extended_surface_listener
    CoverWindowWayland::kExtendedSurfaceListener = {
        .onscreen_visibility =
            [](void* data,
               qt_extended_surface* qt_extended_surface,
               int32_t visible) {
              ELINUX_LOG(INFO) << "Cover onscreen_visibility: " << visible;
              auto self = reinterpret_cast<CoverWindowWayland*>(data);
              self->visibility_ = visible;
              if (visible != 0 && !self->frame_callback_) {
                self->Draw();
              }
            },
        .set_generic_property =
            [](void* data,
               qt_extended_surface* qt_extended_surface,
               const char* name,
               wl_array* value) {
              ELINUX_LOG(INFO) << "Cover set_generic_property: " << name;
            },
        .close =
            [](void* data, qt_extended_surface* qt_extended_surface) {
              ELINUX_LOG(INFO) << "Cover close";
            },
};

const wl_callback_listener CoverWindowWayland::kFrameListener = {
    .done =
        [](void* data, wl_callback* wl_callback, uint32_t time) {
          auto self = reinterpret_cast<CoverWindowWayland*>(data);
          wl_callback_destroy(wl_callback);
          self->frame_callback_ = nullptr;
          self->frame_count_++;
          if (self->visibility_ != 0) {
            self->Draw();
          }
        },
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
          if (self->draw_when_released_) {
            self->Draw();
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
    const char* app_id)
    : shm_(shm) {
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

  if (CreateBuffers()) {
    Draw();
  }
}

CoverWindowWayland::~CoverWindowWayland() {
  if (frame_callback_) {
    wl_callback_destroy(frame_callback_);
  }
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

uint32_t CoverWindowWayland::TakeFrameCount() {
  const uint32_t count = frame_count_;
  frame_count_ = 0;
  return count;
}

bool CoverWindowWayland::CreateBuffers() {
  const int32_t stride = width_ * 4;
  const size_t size = static_cast<size_t>(stride) * height_;
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
    buffer.buffer = wl_shm_pool_create_buffer(pool, 0, width_, height_, stride,
                                              WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    buffer.data = data;
    buffer.size = size;
    buffer.busy = false;
    wl_buffer_add_listener(buffer.buffer, &kBufferListener, this);
  }
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
}

void CoverWindowWayland::Draw() {
  Buffer* buffer = nullptr;
  for (auto& candidate : buffers_) {
    if (candidate.buffer && !candidate.busy) {
      buffer = &candidate;
      break;
    }
  }
  if (!buffer) {
    draw_when_released_ = true;
    return;
  }
  draw_when_released_ = false;

  // Test pattern: an amber strip marks the top, and a white bar moves down a
  // blue background by a few pixels per frame.
  constexpr uint32_t kBlue = 0xff0175c2;
  constexpr uint32_t kAmber = 0xffffc107;
  constexpr uint32_t kWhite = 0xffffffff;
  const int32_t strip = height_ / 20;
  const int32_t bar = height_ / 12;
  const int32_t bar_top =
      strip + static_cast<int32_t>((step_ * 6) % (height_ - strip - bar));
  auto* pixels = static_cast<uint32_t*>(buffer->data);
  for (int32_t y = 0; y < height_; y++) {
    uint32_t color = kBlue;
    if (y < strip) {
      color = kAmber;
    } else if (y >= bar_top && y < bar_top + bar) {
      color = kWhite;
    }
    std::fill(pixels + y * width_, pixels + (y + 1) * width_, color);
  }
  step_++;

  wl_surface_attach(surface_, buffer->buffer, 0, 0);
  wl_surface_damage(surface_, 0, 0, width_, height_);
  if (visibility_ != 0 && !frame_callback_) {
    frame_callback_ = wl_surface_frame(surface_);
    wl_callback_add_listener(frame_callback_, &kFrameListener, this);
  }
  wl_surface_commit(surface_);
  buffer->busy = true;
}

}  // namespace flutter
