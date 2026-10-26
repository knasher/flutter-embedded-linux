// Copyright 2026 Dave Nash. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_WINDOW_COVER_WINDOW_WAYLAND_H_
#define FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_WINDOW_COVER_WINDOW_WAYLAND_H_

#include <wayland-client.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "wayland/protocols/surface-extension-client-protocol.h"

namespace flutter {

// A Sailfish app cover: a second wl_shell window that Lipstick shows in the
// app's cover tile on the home screen, instead of a shrunken copy of the main
// window. It is linked to the main window through Qt surface extension window
// properties, as Silica apps do. It shows pictures the app draws.
class CoverWindowWayland {
 public:
  // The size Lipstick shows covers at on the Jolla Phone 2026, used until
  // Lipstick confirms the size with a configure.
  static constexpr int32_t kDefaultWidth = 446;
  static constexpr int32_t kDefaultHeight = 714;

  // Called when Lipstick shows or hides the cover, or changes its size.
  using ChangedCallback = std::function<void()>;

  CoverWindowWayland(wl_compositor* compositor,
                     wl_shm* shm,
                     wl_shell* shell,
                     qt_surface_extension* surface_extension,
                     wl_surface* main_surface,
                     qt_extended_surface* main_extended_surface,
                     const char* app_id,
                     ChangedCallback on_changed);
  ~CoverWindowWayland();

  bool IsValid() const { return surface_ != nullptr; }

  bool visible() const { return visibility_ != 0; }
  int32_t width() const { return width_; }
  int32_t height() const { return height_; }

  // Shows a picture of |width| x |height| pixels, RGBA with premultiplied
  // alpha. If both buffers are still in use, the latest picture is shown as
  // soon as one is released.
  void SetFrame(int32_t width, int32_t height, const uint8_t* rgba);

 private:
  struct Buffer {
    wl_buffer* buffer = nullptr;
    void* data = nullptr;
    size_t size = 0;
    bool busy = false;
  };

  static const wl_shell_surface_listener kShellSurfaceListener;
  static const qt_extended_surface_listener kExtendedSurfaceListener;
  static const wl_buffer_listener kBufferListener;

  bool CreateBuffers(int32_t width, int32_t height);
  void DestroyBuffers();

  // Copies the pending picture into a free buffer and commits it.
  void CommitPending();

  wl_shm* shm_;
  ChangedCallback on_changed_;
  wl_surface* surface_ = nullptr;
  wl_shell_surface* shell_surface_ = nullptr;
  qt_extended_surface* extended_surface_ = nullptr;
  Buffer buffers_[2];
  int32_t buffer_width_ = 0;
  int32_t buffer_height_ = 0;
  int32_t width_ = kDefaultWidth;
  int32_t height_ = kDefaultHeight;
  int32_t visibility_ = 0;
  // The latest picture not yet shown, as RGBA.
  std::vector<uint8_t> pending_;
  int32_t pending_width_ = 0;
  int32_t pending_height_ = 0;
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_WINDOW_COVER_WINDOW_WAYLAND_H_
