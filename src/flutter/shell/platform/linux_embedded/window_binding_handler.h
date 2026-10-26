// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_WINDOW_BINDING_HANDLER_H_
#define FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_WINDOW_BINDING_HANDLER_H_

#include <string>
#include <variant>
#include <vector>

#include "flutter/shell/platform/linux_embedded/public/flutter_elinux.h"
#include "flutter/shell/platform/linux_embedded/surface/surface_gl.h"
#include "flutter/shell/platform/linux_embedded/window_binding_handler_delegate.h"

namespace flutter {

class FlutterWindowsView;

// Structure containing physical bounds of a Window
struct PhysicalWindowBounds {
  size_t width;
  size_t height;
};

struct PhysicalViewInsets {
  double top = 0;
  double right = 0;
  double bottom = 0;
  double left = 0;
};

// A rectangle in physical pixels.
struct PhysicalRect {
  int32_t left = 0;
  int32_t top = 0;
  int32_t width = 0;
  int32_t height = 0;
};

// The app's cover on Sailfish OS: whether the window can have one, whether
// Lipstick is showing it, and its size in physical pixels.
struct CoverState {
  bool available = false;
  bool visible = false;
  int32_t width = 0;
  int32_t height = 0;
};

// Display cut-outs (e.g. a camera notch) in view coordinates for the current
// content orientation, and the padding that keeps content clear of them.
struct DisplayCutouts {
  std::vector<PhysicalRect> rects;
  PhysicalViewInsets padding;
};

// Orientations from SystemChrome.setPreferredOrientations.
enum class DeviceOrientation {
  kPortraitUp,
  kLandscapeLeft,
  kPortraitDown,
  kLandscapeRight,
};

using ELinuxRenderSurfaceTarget = SurfaceGl;

// Abstract class for binding Linux embedded platform windows to Flutter views.
class WindowBindingHandler {
 public:
  virtual ~WindowBindingHandler() = default;

  // Dispatches window events such as mouse and keyboard inputs. For Wayland,
  // you have to call this every time in the main loop.
  virtual bool DispatchEvent() = 0;

  // Create a surface.
  // @param[in] width_px         Physical width of the surface.
  // @param[in] height_px        Physical height of the surface.
  // @param[in] enable_impeller  Enable impeller.
  virtual bool CreateRenderSurface(int32_t width_px,
                                   int32_t height_px,
                                   bool enable_impeller) = 0;

  // Destroy a surface which is currently used.
  virtual void DestroyRenderSurface() = 0;

  // Returns a valid ELinuxRenderSurfaceTarget representing the backing
  // window.
  virtual ELinuxRenderSurfaceTarget* GetRenderSurfaceTarget() const = 0;

  // Sets the delegate used to communicate state changes from window to view
  // such as key presses, mouse position updates etc.
  virtual void SetView(WindowBindingHandlerDelegate* view) = 0;

  // Returns the rotation(degree) for the backing window.
  virtual uint16_t GetRotationDegree() const = 0;

  // Returns the scale factor for the backing window.
  virtual double GetDpiScale() = 0;

  // Returns the bounds of the backing window in physical pixels.
  virtual PhysicalWindowBounds GetPhysicalWindowBounds() = 0;

  // Returns the frame rate of the display.
  virtual int32_t GetFrameRate() = 0;

  // Sets the cursor that should be used when the mouse is over the Flutter
  // content. See mouse_cursor.dart for the values and meanings of cursor_name.
  virtual void UpdateFlutterCursor(const std::string& cursor_name) = 0;

  // Sets the virtual keyboard status when the virtual keyboard needs to be
  // shown by Flutter events.
  // @param[in] show         Whether to show or hide the virtual keyboard.
  // @param[in] input_type   The Flutter input type (e.g.,
  // "TextInputType.number").
  virtual void UpdateVirtualKeyboardStatus(
      const bool show,
      const std::string& input_type = "") = 0;

  virtual PhysicalViewInsets GetViewInsets() { return {}; }

  virtual void UpdateVirtualKeyboardArea(int32_t x,
                                         int32_t y,
                                         int32_t width,
                                         int32_t height) {}

  // Returns the angle (0, 90, 180 or 270) of the content orientation relative
  // to the display's native orientation, as Maliit's appOrientation expects.
  virtual int32_t GetContentOrientationAngle() const { return 0; }

  // Sets the orientations the app allows, most preferred first. An empty list
  // allows any orientation.
  virtual void SetPreferredOrientations(
      const std::vector<DeviceOrientation>& orientations) {}

  // Returns the display cut-outs the view is drawn under, if any.
  virtual DisplayCutouts GetDisplayCutouts() const { return {}; }

  // Sets the buttons Sailfish OS shows on the app's cover, as the value of
  // the _APP_COVER_ACTION window property (empty for none).
  virtual void SetCoverActions(const std::string& actions) {}

  // Returns whether the app can have a cover window, whether it's shown and
  // its size.
  virtual CoverState GetCoverState() const { return {}; }

  // Shows a picture in the app's cover window, creating the window the first
  // time: |width| x |height| pixels, RGBA with premultiplied alpha.
  virtual void SetCoverFrame(int32_t width,
                             int32_t height,
                             const uint8_t* rgba) {}

  // Returns the clipboard data.
  virtual std::string GetClipboardData() = 0;

  // Sets the clipboard data.
  virtual void SetClipboardData(const std::string& data) = 0;
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_WINDOW_BINDING_HANDLER_H_
