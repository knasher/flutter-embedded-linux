// Copyright 2026 Dave Nash. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_WINDOW_COVER_WINDOW_WAYLAND_H_
#define FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_WINDOW_COVER_WINDOW_WAYLAND_H_

#include <wayland-client.h>

#include <cstddef>
#include <cstdint>

#include "wayland/protocols/surface-extension-client-protocol.h"

namespace flutter {

// A Sailfish app cover: a second wl_shell window that Lipstick shows on the
// home screen while the app is in the background, instead of a shrunken copy
// of the main window. It is linked to the main window through Qt surface
// extension window properties, as Silica apps do.
//
// Test version: while shown it draws a moving bar into wl_shm buffers on every
// frame callback, so the callback rate can be measured.
class CoverWindowWayland {
 public:
  CoverWindowWayland(wl_compositor* compositor,
                     wl_shm* shm,
                     wl_shell* shell,
                     qt_surface_extension* surface_extension,
                     wl_surface* main_surface,
                     qt_extended_surface* main_extended_surface,
                     const char* app_id);
  ~CoverWindowWayland();

  bool IsValid() const { return surface_ != nullptr; }

  // Last onscreen_visibility from Lipstick (0 = hidden).
  int32_t visibility() const { return visibility_; }

  // Returns the number of frame callbacks since the previous call.
  uint32_t TakeFrameCount();

 private:
  struct Buffer {
    wl_buffer* buffer = nullptr;
    void* data = nullptr;
    size_t size = 0;
    bool busy = false;
  };

  static const wl_shell_surface_listener kShellSurfaceListener;
  static const qt_extended_surface_listener kExtendedSurfaceListener;
  static const wl_callback_listener kFrameListener;
  static const wl_buffer_listener kBufferListener;

  bool CreateBuffers();
  void DestroyBuffers();

  // Paints the next test frame and commits it. Asks for a frame callback
  // while the cover is shown.
  void Draw();

  wl_shm* shm_;
  wl_surface* surface_ = nullptr;
  wl_shell_surface* shell_surface_ = nullptr;
  qt_extended_surface* extended_surface_ = nullptr;
  wl_callback* frame_callback_ = nullptr;
  Buffer buffers_[2];
  // Lipstick confirms the size with a configure after the first commit.
  int32_t width_ = 446;
  int32_t height_ = 714;
  int32_t visibility_ = 0;
  bool draw_when_released_ = false;
  uint32_t frame_count_ = 0;
  uint32_t step_ = 0;
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_WINDOW_COVER_WINDOW_WAYLAND_H_
