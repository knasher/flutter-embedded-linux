// Copyright 2023 Sony Corporation. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux_embedded/window/elinux_window_wayland.h"

#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <dconf.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <codecvt>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <locale>
#include <unordered_map>

#include "flutter/shell/platform/linux_embedded/logger.h"
#include "flutter/shell/platform/linux_embedded/surface/context_egl.h"
#include "flutter/shell/platform/linux_embedded/window/qt_window_properties.h"

namespace flutter {

namespace {
constexpr char kZwpTextInputManagerV1[] = "zwp_text_input_manager_v1";
constexpr char kZwpTextInputManagerV3[] = "zwp_text_input_manager_v3";
constexpr char kZxdgDecorationManagerV1[] = "zxdg_decoration_manager_v1";

constexpr char kWlCursorThemeBottomLeftCorner[] = "bottom_left_corner";
constexpr char kWlCursorThemeBottomRightCorner[] = "bottom_right_corner";
constexpr char kWlCursorThemeBottomSide[] = "bottom_side";
constexpr char kWlCursorThemeGrabbing[] = "grabbing";
constexpr char kWlCursorThemeLeftPtr[] = "left_ptr";
constexpr char kWlCursorThemeLeftSide[] = "left_side";
constexpr char kWlCursorThemeRightSide[] = "right_side";
constexpr char kWlCursorThemeTopLeftCorner[] = "top_left_corner";
constexpr char kWlCursorThemeTopRightCorner[] = "top_right_corner";
constexpr char kWlCursorThemeTopSide[] = "top_side";
constexpr char kWlCursorThemeXterm[] = "xterm";
constexpr char kWlCursorThemeHand1[] = "hand1";
constexpr char kWlCursorThemeWatch[] = "watch";
constexpr char kCursorNameNone[] = "none";

constexpr char kClipboardMimeTypeText[] = "text/plain";

double SnapDevicePixelRatio(double dpi) {
  if (dpi <= 0.0) {
    return 0.0;
  }
  double dpr = std::round(dpi / 160.0 * 4.0) / 4.0;
  if (dpr < 1.0) {
    dpr = 1.0;
  } else if (dpr > 4.0) {
    dpr = 4.0;
  }
  return dpr;
}

// Reads a dconf key as the dconf tool prints it, or "" if it isn't set.
// In-process, since a Sailjail sandbox has no dconf tool (or shell) to run.
std::string ReadDconf(const char* key) {
  DConfClient* client = dconf_client_new();
  GVariant* value = dconf_client_read(client, key);
  g_object_unref(client);
  if (!value) {
    return std::string();
  }
  gchar* text = g_variant_print(value, TRUE);
  std::string result(text);
  g_free(text);
  g_variant_unref(value);
  return result;
}

double ReadDconfDouble(const char* key) {
  std::string stripped;
  for (const char c : ReadDconf(key)) {
    if (c != '\'' && c != '"' && c != ' ' && c != '\n' && c != '\r' &&
        c != '\t') {
      stripped += c;
    }
  }
  return std::atof(stripped.c_str());
}

double ReadDevicePixelRatio() {
  if (const char* env = std::getenv("FLUTTER_SCALE")) {
    const double value = std::atof(env);
    if (value > 0.0) {
      return value;
    }
  }
  return SnapDevicePixelRatio(
      ReadDconfDouble("/lipstick/screen/primary/physicalDotsPerInch"));
}

// Reads Silica's display cut-outs, e.g. "[[310, 0, 414, 66]]": rectangles as
// [x, y, width, height] in native panel pixels.
std::vector<PhysicalRect> ReadDisplayCutouts() {
  std::vector<PhysicalRect> cutouts;
  const std::string value = ReadDconf("/desktop/sailfish/silica/cutouts");

  // Each innermost [...] holds one rectangle.
  size_t open = value.find('[');
  while (open != std::string::npos) {
    const size_t next = value.find('[', open + 1);
    const size_t close = value.find(']', open + 1);
    if (close != std::string::npos &&
        (next == std::string::npos || close < next)) {
      PhysicalRect rect;
      if (std::sscanf(value.c_str() + open + 1, "%d , %d , %d , %d",
                      &rect.left, &rect.top, &rect.width,
                      &rect.height) == 4 &&
          rect.width > 0 && rect.height > 0) {
        cutouts.push_back(rect);
      }
    }
    open = next;
  }
  return cutouts;
}
}  // namespace

const wl_registry_listener ELinuxWindowWayland::kWlRegistryListener = {
    .global =
        [](void* data,
           wl_registry* wl_registry,
           uint32_t name,
           const char* interface,
           uint32_t version) {
          ELINUX_LOG(TRACE) << "wl_registry_listener.global";

          auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
          self->WlRegistryHandler(wl_registry, name, interface, version);
        },
    .global_remove =
        [](void* data, wl_registry* wl_registry, uint32_t name) {
          ELINUX_LOG(TRACE) << "wl_registry_listener.global_remove";

          auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
          self->WlUnRegistryHandler(wl_registry, name);
        },
};

const xdg_wm_base_listener ELinuxWindowWayland::kXdgWmBaseListener = {
    .ping =
        [](void* data, xdg_wm_base* xdg_wm_base, uint32_t serial) {
          ELINUX_LOG(TRACE) << "xdg_wm_base_listener.ping";
          xdg_wm_base_pong(xdg_wm_base, serial);
        },
};

const xdg_surface_listener ELinuxWindowWayland::kXdgSurfaceListener = {
    .configure =
        [](void* data, xdg_surface* xdg_surface, uint32_t serial) {
          ELINUX_LOG(TRACE) << "xdg_surface_listener.configure";

          auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
          constexpr int32_t x = 0;
          int32_t y = 0;
          auto width = self->view_properties_.width;
          auto height = self->view_properties_.height;

          if (self->window_decorations_) {
            // Shift the window position to the bottom to show decoration
            // even when the window is displayed in the upper left corner
            // of the screen
            y = -self->window_decorations_->Height();
            if (!self->maximised_) {
              height -= y;
            }

            // clip
            if (self->display_max_height_ > 0) {
              height = std::min(height, self->display_max_height_);
            }
          }
          xdg_surface_set_window_geometry(xdg_surface, x, y, width, height);

          xdg_surface_ack_configure(xdg_surface, serial);
          if (self->wait_for_configure_) {
            self->wait_for_configure_ = false;
          }
          self->request_redraw_ = true;
        },
};

const xdg_toplevel_listener ELinuxWindowWayland::kXdgToplevelListener = {
    .configure =
        [](void* data,
           xdg_toplevel* xdg_toplevel,
           int32_t width,
           int32_t height,
           wl_array* states) {
          ELINUX_LOG(TRACE)
              << "xdg_toplevel_listener.configure: " << width << ", " << height;

          auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
          if (self->current_rotation_ == 90 || self->current_rotation_ == 270) {
            std::swap(width, height);
          }

          if (self->IsCoverConfigure(width, height)) {
            return;
          }

          int32_t next_width_dip = width / self->current_scale_;
          int32_t next_height_dip = height / self->current_scale_;
          if (self->restore_window_required_) {
            self->restore_window_required_ = false;
            next_width_dip = self->restore_window_width_;
            next_height_dip = self->restore_window_height_;
          }

          if (!next_width_dip || !next_height_dip ||
              (self->view_properties_.width == next_width_dip &&
               self->view_properties_.height == next_height_dip)) {
            return;
          }

          ELINUX_LOG(TRACE) << "request redraw: " << next_width_dip << ", "
                            << next_height_dip;
          self->view_properties_.width = next_width_dip;
          self->view_properties_.height = next_height_dip;
          self->request_redraw_ = true;
        },
    .close =
        [](void* data, xdg_toplevel* xdg_toplevel) {
          ELINUX_LOG(TRACE) << "xdg_toplevel_listener.close";

          auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
          self->running_ = false;
        }};

const wl_shell_surface_listener ELinuxWindowWayland::kWlShellSurfaceListener =
    {
        .ping =
            [](void* data, wl_shell_surface* wl_shell_surface,
               uint32_t serial) {
              wl_shell_surface_pong(wl_shell_surface, serial);
            },
        .configure =
            [](void* data,
               wl_shell_surface* wl_shell_surface,
               uint32_t edges,
               int32_t width,
               int32_t height) {
              ELINUX_LOG(TRACE) << "wl_shell_surface_listener.configure: "
                                << width << ", " << height;

              auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
              // Lipstick keeps wl_shell surfaces in the panel's native
              // orientation; the content is rotated inside them.
              if (self->current_rotation_ == 90 ||
                  self->current_rotation_ == 270) {
                std::swap(width, height);
              }
              const int32_t next_width_dip = width / self->current_scale_;
              const int32_t next_height_dip = height / self->current_scale_;
              if (!next_width_dip || !next_height_dip ||
                  (self->view_properties_.width == next_width_dip &&
                   self->view_properties_.height == next_height_dip)) {
                return;
              }
              self->view_properties_.width = next_width_dip;
              self->view_properties_.height = next_height_dip;
              self->request_redraw_ = true;
            },
        .popup_done = [](void* data, wl_shell_surface* wl_shell_surface) {},
};

const qt_extended_surface_listener
    ELinuxWindowWayland::kQtExtendedSurfaceListener = {
        .onscreen_visibility =
            [](void* data,
               qt_extended_surface* qt_extended_surface,
               int32_t visible) {
              ELINUX_LOG(INFO)
                  << "qt_extended_surface.onscreen_visibility: " << visible;
              auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
              // Undocumented; observed on SFOS 5.2: 5 in the foreground, 3
              // while shown as the app's cover (its thumbnail in the tile),
              // 0 when not shown at all (minimised behind a cover window,
              // screen off, home screen off-view, another app in front).
              // Bit 2 is taken to mean "cover".
              const bool cover = (visible & 2) != 0;
              if (self->binding_handler_delegate_) {
                using AppVisibility =
                    WindowBindingHandlerDelegate::AppVisibility;
                self->binding_handler_delegate_->OnAppVisibilityChanged(
                    visible == 0 ? AppVisibility::kBackground
                    : cover      ? AppVisibility::kThumbnail
                                 : AppVisibility::kForeground);
              }
              // Hidden keeps the current orientation, so the window doesn't
              // turn while nobody can see it and then show up turned in its
              // tile.
              if (visible == 0) {
                return;
              }
              self->SetShownAsCover(cover);
            },
        .set_generic_property =
            [](void* data,
               qt_extended_surface* qt_extended_surface,
               const char* name,
               wl_array* value) {
              ELINUX_LOG(TRACE)
                  << "qt_extended_surface.set_generic_property: " << name;
            },
        .close =
            [](void* data, qt_extended_surface* qt_extended_surface) {
              ELINUX_LOG(TRACE) << "qt_extended_surface.close";
              auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
              self->running_ = false;
            },
};

const wl_surface_listener ELinuxWindowWayland::kWlSurfaceListener = {
    .enter =
        [](void* data, wl_surface* wl_surface, wl_output* output) {
          ELINUX_LOG(TRACE) << "wl_surface_listener.enter";

          // The compositor can send a null output: crbug.com/1332540
          if (!output) {
            ELINUX_LOG(ERROR) << "cannot enter a NULL output";
            return;
          }

          const uint32_t output_id =
              wl_proxy_get_id(reinterpret_cast<wl_proxy*>(output));
          ELINUX_LOG(TRACE) << "window entered output " << output_id;
          auto self = reinterpret_cast<ELinuxWindowWayland*>(data);

          self->entered_outputs_.insert(output_id);
          self->UpdateWindowScale();
        },
    .leave =
        [](void* data, wl_surface* wl_surface, wl_output* output) {
          ELINUX_LOG(TRACE) << "wl_surface_listener.leave";

          // The compositor can send a null output: crbug.com/1332540
          if (!output) {
            ELINUX_LOG(ERROR) << "cannot leave a NULL output";
            return;
          }

          const uint32_t output_id =
              wl_proxy_get_id(reinterpret_cast<wl_proxy*>(output));
          ELINUX_LOG(TRACE) << "window left output " << output_id;
          auto self = reinterpret_cast<ELinuxWindowWayland*>(data);

          self->entered_outputs_.erase(output_id);
          self->UpdateWindowScale();
        }};

const wp_presentation_listener ELinuxWindowWayland::kWpPresentationListener = {
    .clock_id =
        [](void* data, wp_presentation* wp_presentation, uint32_t clk_id) {
          ELINUX_LOG(TRACE) << "wp_presentation_listener.clock_id";

          auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
          self->wp_presentation_clk_id_ = clk_id;
          ELINUX_LOG(TRACE) << "clk_id = " << clk_id;
        },
};

const wp_presentation_feedback_listener
    ELinuxWindowWayland::kWpPresentationFeedbackListener = {
        .sync_output =
            [](void* data,
               struct wp_presentation_feedback* wp_presentation_feedback,
               wl_output* output) {
              ELINUX_LOG(TRACE)
                  << "wp_presentation_feedback_listener.sync_output";
            },
        .presented =
            [](void* data,
               struct wp_presentation_feedback* wp_presentation_feedback,
               uint32_t tv_sec_hi,
               uint32_t tv_sec_lo,
               uint32_t tv_nsec,
               uint32_t refresh,
               uint32_t seq_hi,
               uint32_t seq_lo,
               uint32_t flags) {
              ELINUX_LOG(TRACE)
                  << "wp_presentation_feedback_listener.presented";

              auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
              self->last_frame_time_nanos_ =
                  (((static_cast<uint64_t>(tv_sec_hi) << 32) + tv_sec_lo) *
                   1000000000) +
                  tv_nsec;
              self->frame_rate_ =
                  static_cast<int32_t>(std::round(1000000000000.0 / refresh));

              if (self->window_decorations_) {
                self->window_decorations_->Draw();
              }

              wp_presentation_feedback_destroy(wp_presentation_feedback);
              wp_presentation_feedback_add_listener(
                  ::wp_presentation_feedback(self->wp_presentation_,
                                             self->native_window_->Surface()),
                  &kWpPresentationFeedbackListener, data);
            },
        .discarded =
            [](void* data,
               struct wp_presentation_feedback* wp_presentation_feedback) {
              ELINUX_LOG(TRACE)
                  << "wp_presentation_feedback_listener.discarded";

              auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
              if (self->window_decorations_) {
                self->window_decorations_->Draw();
              }

              wp_presentation_feedback_destroy(wp_presentation_feedback);
              wp_presentation_feedback_add_listener(
                  ::wp_presentation_feedback(self->wp_presentation_,
                                             self->native_window_->Surface()),
                  &kWpPresentationFeedbackListener, data);
            },
};

const wl_callback_listener ELinuxWindowWayland::kWlSurfaceFrameListener = {
    .done =
        [](void* data, wl_callback* wl_callback, uint32_t time) {
          ELINUX_LOG(TRACE) << "wl_callback_listener.done";

          // The presentation-time is an extended protocol and isn't supported
          // by all compositors. This path is for when it wasn't supported.
          auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
          if (self->wp_presentation_clk_id_ != UINT32_MAX) {
            wl_callback_destroy(wl_callback);
            return;
          }

          if (self->window_decorations_) {
            self->window_decorations_->Draw();
          }

          self->last_frame_time_nanos_ = static_cast<uint64_t>(time) * 1000000;

          auto callback = wl_surface_frame(self->native_window_->Surface());
          wl_callback_destroy(wl_callback);
          wl_callback_add_listener(callback, &kWlSurfaceFrameListener, data);
        },
};

const wl_seat_listener ELinuxWindowWayland::kWlSeatListener = {
    .capabilities = [](void* data, wl_seat* seat, uint32_t caps) -> void {
      ELINUX_LOG(TRACE) << "wl_seat_listener.capabilities";

      auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
      auto seat_iter = self->seat_inputs_map_.find(seat);
      if (seat_iter == self->seat_inputs_map_.end()) {
        ELINUX_LOG(ERROR) << "Failed to find the seat";
        return;
      }
      auto& inputs = seat_iter->second;

      if ((caps & WL_SEAT_CAPABILITY_POINTER) && !inputs.pointer) {
        inputs.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(inputs.pointer, &kWlPointerListener, self);
        ELINUX_LOG(TRACE) << "Pointer device connected: " << inputs.pointer;
      } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && inputs.pointer) {
        ELINUX_LOG(TRACE) << "Pointer device disconnecting: " << inputs.pointer;
        if (self->cursor_info_.pointer == inputs.pointer) {
          self->cursor_info_.pointer = nullptr;
        }
        wl_pointer_release(inputs.pointer);
        inputs.pointer = nullptr;
      }

      if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !inputs.touch) {
        inputs.touch = wl_seat_get_touch(seat);
        wl_touch_add_listener(inputs.touch, &kWlTouchListener, self);
      } else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && inputs.touch) {
        wl_touch_release(inputs.touch);
        inputs.touch = nullptr;
      }

      if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !inputs.keyboard) {
        inputs.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(inputs.keyboard, &kWlKeyboardListener, self);
      } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && inputs.keyboard) {
        wl_keyboard_release(inputs.keyboard);
        inputs.keyboard = nullptr;
      }
    },
    .name = [](void* data, struct wl_seat* wl_seat, const char* name) -> void {
      ELINUX_LOG(TRACE) << "wl_seat_listener.name";
    },
};

const wl_pointer_listener ELinuxWindowWayland::kWlPointerListener = {
    .enter = [](void* data,
                wl_pointer* wl_pointer,
                uint32_t serial,
                wl_surface* surface,
                wl_fixed_t surface_x,
                wl_fixed_t surface_y) -> void {
      ELINUX_LOG(TRACE) << "wl_pointer_listener.enter";

      auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
      self->wl_current_surface_ = surface;
      self->serial_ = serial;

      if (self->view_properties_.use_mouse_cursor) {
        self->cursor_info_.pointer = wl_pointer;
        self->cursor_info_.serial = serial;
      }

      if (self->binding_handler_delegate_) {
        double x_px = wl_fixed_to_double(surface_x) * self->current_scale_;
        double y_px = wl_fixed_to_double(surface_y) * self->current_scale_;
        self->binding_handler_delegate_->OnPointerMove(x_px, y_px);
        self->pointer_x_ = x_px;
        self->pointer_y_ = y_px;
      }

      // Force redraw even though it is the same cursor name
      auto copy = self->cursor_info_.cursor_name;
      self->cursor_info_.cursor_name.clear();
      self->UpdateFlutterCursor(copy);
    },
    .leave = [](void* data,
                wl_pointer* pointer,
                uint32_t serial,
                wl_surface* surface) -> void {
      ELINUX_LOG(TRACE) << "wl_pointer_listener.leave";

      auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
      self->wl_current_surface_ = surface;
      self->serial_ = serial;

      if (self->view_properties_.use_mouse_cursor) {
        // Clear the cursor name in order to make sure it gets redrawn next time
        // it enters the surface.
        self->cursor_info_.cursor_name.clear();
      }

      if (self->binding_handler_delegate_) {
        self->binding_handler_delegate_->OnPointerLeave();
        self->pointer_x_ = -1;
        self->pointer_y_ = -1;
      }
    },
    .motion = [](void* data,
                 wl_pointer* pointer,
                 uint32_t time,
                 wl_fixed_t surface_x,
                 wl_fixed_t surface_y) -> void {
      ELINUX_LOG(TRACE) << "wl_pointer_listener.motion";

      auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
      if (self->binding_handler_delegate_) {
        double x_px = wl_fixed_to_double(surface_x) * self->current_scale_;
        double y_px = wl_fixed_to_double(surface_y) * self->current_scale_;
        self->binding_handler_delegate_->OnPointerMove(x_px, y_px);
        self->pointer_x_ = x_px;
        self->pointer_y_ = y_px;
      }
    },
    .button = [](void* data,
                 wl_pointer* pointer,
                 uint32_t serial,
                 uint32_t time,
                 uint32_t button,
                 uint32_t status) -> void {
      ELINUX_LOG(TRACE) << "wl_pointer_listener.button";

      auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
      self->serial_ = serial;

      if (button == BTN_LEFT && status == WL_POINTER_BUTTON_STATE_PRESSED) {
        if (self->window_decorations_ &&
            self->window_decorations_->IsMatched(
                self->wl_current_surface_,
                WindowDecoration::DecorationType::TITLE_BAR)) {
          for (auto& [seat, inputs] : self->seat_inputs_map_) {
            if (inputs.pointer == pointer) {
              xdg_toplevel_move(self->xdg_toplevel_, seat, serial);
              return;
            }
          }
          ELINUX_LOG(TRACE)
              << "Failed to find the pointer for moving the window";
          return;
        }

        if (self->window_decorations_ &&
            self->window_decorations_->IsMatched(
                self->wl_current_surface_,
                WindowDecoration::DecorationType::CLOSE_BUTTON)) {
          self->running_ = false;
          return;
        }

        if (self->window_decorations_ &&
            self->window_decorations_->IsMatched(
                self->wl_current_surface_,
                WindowDecoration::DecorationType::MAXIMISE_BUTTON)) {
          if (self->maximised_) {
            xdg_toplevel_unset_maximized(self->xdg_toplevel_);

            // Requests to return to the original window size before maximizing.
            self->restore_window_required_ = true;
          } else {
            // Stores original window size.
            self->restore_window_width_ = self->view_properties_.width;
            self->restore_window_height_ = self->view_properties_.height;
            self->restore_window_required_ = false;

            xdg_toplevel_set_maximized(self->xdg_toplevel_);
          }
          self->maximised_ = !self->maximised_;
          return;
        }

        if (self->window_decorations_ &&
            self->window_decorations_->IsMatched(
                self->wl_current_surface_,
                flutter::WindowDecoration::DecorationType::MINIMISE_BUTTON)) {
          xdg_toplevel_set_minimized(self->xdg_toplevel_);
          return;
        }
      }

      if (self->binding_handler_delegate_) {
        FlutterPointerMouseButtons flutter_button;
        switch (button) {
          case BTN_LEFT:
            flutter_button = kFlutterPointerButtonMousePrimary;
            break;
          case BTN_RIGHT:
            flutter_button = kFlutterPointerButtonMouseSecondary;
            break;
          case BTN_MIDDLE:
            flutter_button = kFlutterPointerButtonMouseMiddle;
            break;
          case BTN_BACK:
            flutter_button = kFlutterPointerButtonMouseBack;
            break;
          case BTN_FORWARD:
            flutter_button = kFlutterPointerButtonMouseForward;
            break;
          default:
            ELINUX_LOG(ERROR) << "Not expected button input: " << button;
            return;
        }

        if (status == WL_POINTER_BUTTON_STATE_PRESSED) {
          self->binding_handler_delegate_->OnPointerDown(
              self->pointer_x_, self->pointer_y_, flutter_button);
        } else {
          self->binding_handler_delegate_->OnPointerUp(
              self->pointer_x_, self->pointer_y_, flutter_button);
        }
      }
    },
    .axis = [](void* data,
               wl_pointer* wl_pointer,
               uint32_t time,
               uint32_t axis,
               wl_fixed_t value) -> void {
      ELINUX_LOG(TRACE) << "wl_pointer_listener.axis";

      auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
      if (self->binding_handler_delegate_) {
        double delta = wl_fixed_to_double(value);
        constexpr int32_t kScrollOffsetMultiplier = 20;
        self->binding_handler_delegate_->OnScroll(
            self->pointer_x_, self->pointer_y_,
            axis == WL_POINTER_AXIS_VERTICAL_SCROLL ? 0 : delta,
            axis == WL_POINTER_AXIS_VERTICAL_SCROLL ? delta : 0,
            kScrollOffsetMultiplier);
      }
    },
};

const wl_touch_listener ELinuxWindowWayland::kWlTouchListener = {
    .down = [](void* data,
               wl_touch* wl_touch,
               uint32_t serial,
               uint32_t time,
               wl_surface* surface,
               int32_t id,
               wl_fixed_t surface_x,
               wl_fixed_t surface_y) -> void {
      ELINUX_LOG(TRACE) << "wl_touch_listener.down";

      auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
      self->serial_ = serial;
      if (self->binding_handler_delegate_) {
        double x = wl_fixed_to_double(surface_x);
        double y = wl_fixed_to_double(surface_y);
        self->binding_handler_delegate_->OnTouchDown(time, id, x, y);
      }
    },
    .up = [](void* data,
             wl_touch* wl_touch,
             uint32_t serial,
             uint32_t time,
             int32_t id) -> void {
      ELINUX_LOG(TRACE) << "wl_touch_listener.up";

      auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
      self->serial_ = serial;
      if (self->binding_handler_delegate_) {
        self->binding_handler_delegate_->OnTouchUp(time, id);
      }
    },
    .motion = [](void* data,
                 wl_touch* wl_touch,
                 uint32_t time,
                 int32_t id,
                 wl_fixed_t surface_x,
                 wl_fixed_t surface_y) -> void {
      ELINUX_LOG(TRACE) << "wl_touch_listener.motion";

      auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
      if (self->binding_handler_delegate_) {
        double x = wl_fixed_to_double(surface_x);
        double y = wl_fixed_to_double(surface_y);
        self->binding_handler_delegate_->OnTouchMotion(time, id, x, y);
      }
    },
    .frame = [](void* data, wl_touch* wl_touch) -> void {},
    .cancel = [](void* data, wl_touch* wl_touch) -> void {
      ELINUX_LOG(TRACE) << "wl_touch_listener.cancel";

      auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
      if (self->binding_handler_delegate_) {
        self->binding_handler_delegate_->OnTouchCancel();
      }
    },
};

const wl_keyboard_listener ELinuxWindowWayland::kWlKeyboardListener = {
    .keymap = [](void* data,
                 wl_keyboard* wl_keyboard,
                 uint32_t format,
                 int fd,
                 uint32_t size) -> void {
      ELINUX_LOG(TRACE) << "wl_keyboard_listener.keymap";

      auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
      assert(format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1);
      if (self->binding_handler_delegate_) {
        self->binding_handler_delegate_->OnKeyMap(format, fd, size);
      }
    },
    .enter = [](void* data,
                wl_keyboard* wl_keyboard,
                uint32_t serial,
                wl_surface* surface,
                wl_array* keys) -> void {
      ELINUX_LOG(TRACE) << "wl_keyboard_listener.enter";

      auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
      self->serial_ = serial;
    },
    .leave = [](void* data,
                wl_keyboard* wl_keyboard,
                uint32_t serial,
                wl_surface* surface) -> void {
      ELINUX_LOG(TRACE) << "wl_keyboard_listener.leave";

      auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
      self->serial_ = serial;
    },
    .key = [](void* data,
              wl_keyboard* wl_keyboard,
              uint32_t serial,
              uint32_t time,
              uint32_t key,
              uint32_t state) -> void {
      ELINUX_LOG(TRACE) << "wl_keyboard_listener.key";

      auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
      self->serial_ = serial;
      if (self->binding_handler_delegate_) {
        self->binding_handler_delegate_->OnKey(
            key, state == WL_KEYBOARD_KEY_STATE_PRESSED);
      }
    },
    .modifiers = [](void* data,
                    wl_keyboard* wl_keyboard,
                    uint32_t serial,
                    uint32_t mods_depressed,
                    uint32_t mods_latched,
                    uint32_t mods_locked,
                    uint32_t group) -> void {
      ELINUX_LOG(TRACE) << "wl_keyboard_listener.modifiers";

      auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
      if (self->binding_handler_delegate_) {
        self->binding_handler_delegate_->OnKeyModifiers(
            mods_depressed, mods_latched, mods_locked, group);
      }
    },
    .repeat_info = [](void* data, wl_keyboard* wl_keyboard, int rate, int delay)
        -> void { ELINUX_LOG(TRACE) << "wl_keyboard_listener.repeat_info"; },
};

const wl_output_listener ELinuxWindowWayland::kWlOutputListener = {
    .geometry = [](void* data,
                   wl_output* wl_output,
                   int32_t x,
                   int32_t y,
                   int32_t physical_width,
                   int32_t physical_height,
                   int32_t subpixel,
                   const char* make,
                   const char* model,
                   int32_t output_transform) -> void {
      ELINUX_LOG(TRACE) << "wl_output_listener.geometry";
      auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
      self->transform_ = output_transform;
      self->output_physical_width_mm_ = physical_width;
      self->MaybeUpdateDevicePixelRatio();
    },
    .mode = [](void* data,
               wl_output* wl_output,
               uint32_t flags,
               int32_t width,
               int32_t height,
               int32_t refresh) -> void {
      ELINUX_LOG(TRACE) << "wl_output_listener.mode";

      auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
      if (flags & WL_OUTPUT_MODE_CURRENT) {
        self->output_mode_width_ = width;
        // While following the output transform, the rotation is applied on
        // wl_output.done, so only the current rotation counts here.
        const bool transform_rotated =
            !self->FollowsOutputTransform() &&
            (self->transform_ == WL_OUTPUT_TRANSFORM_90 ||
             self->transform_ == WL_OUTPUT_TRANSFORM_270 ||
             self->transform_ == WL_OUTPUT_TRANSFORM_FLIPPED_90 ||
             self->transform_ == WL_OUTPUT_TRANSFORM_FLIPPED_270);
        if (self->current_rotation_ == 90 || self->current_rotation_ == 270 ||
            transform_rotated) {
          std::swap(width, height);
        }

        ELINUX_LOG(INFO) << "Display output info: width = " << width
                         << ", height = " << height
                         << ", refresh = " << refresh;
        self->display_max_width_ = width;
        self->display_max_height_ = height;
        self->MaybeUpdateDevicePixelRatio();

        // Some composers send 0 for the refresh value.
        if (refresh != 0) {
          self->frame_rate_ = refresh;
        }

        // wl_output reports physical pixels; the rest of the codebase tracks
        // view_properties_ as logical DIP (physical_px == DIP * current_scale_)
        // — convert once so the storage and clipping below share units, and
        // the invariant view_properties_.width * current_scale_ == native_px
        // holds consistently with xdg_toplevel_listener.configure.
        const int32_t width_dip =
            static_cast<int32_t>(width / self->current_scale_);
        const int32_t height_dip =
            static_cast<int32_t>(height / self->current_scale_);

        if (self->view_properties_.view_mode ==
            FlutterDesktopViewMode::kFullscreen) {
          self->view_properties_.width = width_dip;
          self->view_properties_.height = height_dip;
          self->request_redraw_ = true;
        }

        if (self->view_properties_.width > width_dip) {
          ELINUX_LOG(WARNING)
              << "Requested width size(" << self->view_properties_.width << ") "
              << "is larger than display size(" << width_dip
              << ")"
                 ", clipping";
          self->view_properties_.width = width_dip;
          self->request_redraw_ = true;
        }
        if (self->view_properties_.height > height_dip) {
          ELINUX_LOG(WARNING)
              << "Requested height size(" << self->view_properties_.height
              << ") " << "is larger than display size(" << height_dip
              << ")"
                 ", clipping";
          self->view_properties_.height = height_dip;
          self->request_redraw_ = true;
        }
      }
    },
    .done = [](void* data, wl_output* wl_output) -> void {
      ELINUX_LOG(TRACE) << "wl_output_listener.done";
      auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
      self->MaybeApplyOutputTransform();
    },
    .scale = [](void* data, wl_output* wl_output, int32_t scale) -> void {
      ELINUX_LOG(TRACE) << "wl_output_listener.scale";

      auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
      const uint32_t output_id =
          wl_proxy_get_id(reinterpret_cast<wl_proxy*>(wl_output));

      ELINUX_LOG(INFO) << "Display scale for output(" << output_id
                       << "): " << scale;

      self->wl_output_scale_factors_[output_id] = scale;

      self->UpdateWindowScale();
    },
};

const zwp_text_input_v1_listener ELinuxWindowWayland::kZwpTextInputV1Listener =
    {
        .enter = [](void* data,
                    zwp_text_input_v1* zwp_text_input_v1,
                    wl_surface* surface) -> void {
          ELINUX_LOG(TRACE) << "zwp_text_input_v1_listener.enter";

          auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
          // If there is no input data, the backspace key cannot be used,
          // so set dummy data.
          if (self->zwp_text_input_v1_) {
            zwp_text_input_v1_set_surrounding_text(self->zwp_text_input_v1_,
                                                   " ", 1, 1);
          }
        },
        .leave = [](void* data, zwp_text_input_v1* zwp_text_input_v1) -> void {
          ELINUX_LOG(TRACE) << "zwp_text_input_v1_listener.leave";

          auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
          if (self->zwp_text_input_v1_) {
            zwp_text_input_v1_hide_input_panel(self->zwp_text_input_v1_);
          }
        },
        .modifiers_map = [](void* data,
                            zwp_text_input_v1* zwp_text_input_v1,
                            wl_array* map) -> void {
          ELINUX_LOG(TRACE) << "zwp_text_input_v1_listener.modifiers_map";
        },
        .input_panel_state = [](void* data,
                                zwp_text_input_v1* zwp_text_input_v1,
                                uint32_t state) -> void {
          ELINUX_LOG(TRACE) << "zwp_text_input_v1_listener.input_panel_state";
        },
        .preedit_string = [](void* data,
                             zwp_text_input_v1* zwp_text_input_v1,
                             uint32_t serial,
                             const char* text,
                             const char* commit) -> void {
          ELINUX_LOG(TRACE) << "zwp_text_input_v1_listener.preedit_string";

          auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
          if (self->binding_handler_delegate_ && strlen(text)) {
            std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t>
                utf8_converter;
            std::u16string utf16_text = utf8_converter.from_bytes(text);
            self->binding_handler_delegate_->OnVirtualKey(utf16_text[0]);
          }
          if (self->zwp_text_input_v1_) {
            zwp_text_input_v1_reset(self->zwp_text_input_v1_);
            // If there is no input data, the backspace key cannot be used,
            // so set dummy data.
            zwp_text_input_v1_set_surrounding_text(self->zwp_text_input_v1_,
                                                   " ", 1, 1);
          }
        },
        .preedit_styling = [](void* data,
                              zwp_text_input_v1* zwp_text_input_v1,
                              uint32_t index,
                              uint32_t length,
                              uint32_t style) -> void {
          ELINUX_LOG(TRACE) << "zwp_text_input_v1_listener.preedit_styling";
        },
        .preedit_cursor = [](void* data,
                             zwp_text_input_v1* zwp_text_input_v1,
                             int32_t index) -> void {
          ELINUX_LOG(TRACE) << "zwp_text_input_v1_listener.preedit_cursor";
        },
        .commit_string = [](void* data,
                            zwp_text_input_v1* zwp_text_input_v1,
                            uint32_t serial,
                            const char* text) -> void {
          ELINUX_LOG(TRACE) << "zwp_text_input_v1_listener.commit_string";

          // commit_string is notified only when the space key is pressed.
          auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
          if (self->binding_handler_delegate_ && strlen(text)) {
            std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t>
                utf8_converter;
            std::u16string utf16_text = utf8_converter.from_bytes(text);
            self->binding_handler_delegate_->OnVirtualKey(utf16_text[0]);
          }
          // If there is no input data, the backspace key cannot be used,
          // so set dummy data.
          if (self->zwp_text_input_v1_) {
            zwp_text_input_v1_set_surrounding_text(self->zwp_text_input_v1_,
                                                   " ", 1, 1);
          }
        },
        .cursor_position = [](void* data,
                              zwp_text_input_v1* zwp_text_input_v1,
                              int32_t index,
                              int32_t anchor) -> void {
          ELINUX_LOG(TRACE) << "zwp_text_input_v1_listener.cursor_position";
        },
        .delete_surrounding_text = [](void* data,
                                      zwp_text_input_v1* zwp_text_input_v1,
                                      int32_t index,
                                      uint32_t length) -> void {
          ELINUX_LOG(TRACE)
              << "zwp_text_input_v1_listener.delete_surrounding_text";

          auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
          if (self->binding_handler_delegate_) {
            self->binding_handler_delegate_->OnVirtualSpecialKey(KEY_BACKSPACE);
          }
          // If there is no input data, the backspace key cannot be used,
          // so set dummy data.
          if (self->zwp_text_input_v1_) {
            zwp_text_input_v1_set_surrounding_text(self->zwp_text_input_v1_,
                                                   " ", 1, 1);
          }
        },
        .keysym = [](void* data,
                     zwp_text_input_v1* zwp_text_input_v1,
                     uint32_t serial,
                     uint32_t time,
                     uint32_t sym,
                     uint32_t state,
                     uint32_t modifiers) -> void {
          ELINUX_LOG(TRACE) << "zwp_text_input_v1_listener.keysym";

          auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
          if ((state == WL_KEYBOARD_KEY_STATE_PRESSED) &&
              (self->binding_handler_delegate_)) {
            switch (sym) {
              case XKB_KEY_Left:
                self->binding_handler_delegate_->OnVirtualSpecialKey(KEY_LEFT);
                break;
              case XKB_KEY_Right:
                self->binding_handler_delegate_->OnVirtualSpecialKey(KEY_RIGHT);
                break;
              case XKB_KEY_Up:
                self->binding_handler_delegate_->OnVirtualSpecialKey(KEY_UP);
                break;
              case XKB_KEY_Down:
                self->binding_handler_delegate_->OnVirtualSpecialKey(KEY_DOWN);
                break;
              case XKB_KEY_Tab:
                self->binding_handler_delegate_->OnVirtualSpecialKey(KEY_TAB);
                break;
              case XKB_KEY_Return:
                self->binding_handler_delegate_->OnVirtualSpecialKey(KEY_ENTER);
                break;
              default:
                break;
            }
          }
        },
        .language = [](void* data,
                       zwp_text_input_v1* zwp_text_input_v1,
                       uint32_t serial,
                       const char* language) -> void {
          ELINUX_LOG(TRACE) << "zwp_text_input_v1_listener.language";
        },
        .text_direction = [](void* data,
                             zwp_text_input_v1* zwp_text_input_v1,
                             uint32_t serial,
                             uint32_t direction) -> void {
          ELINUX_LOG(TRACE) << "zwp_text_input_v1_listener.text_direction";
        },
};

const zwp_text_input_v3_listener ELinuxWindowWayland::kZwpTextInputV3Listener =
    {
        .enter = [](void* data,
                    zwp_text_input_v3* zwp_text_input_v3,
                    wl_surface* surface) -> void {
          ELINUX_LOG(TRACE) << "zwp_text_input_v3_listener.enter";

          // To appear the on-screen keyboard when the user returns to a Flutter
          // app which needs to show the on-screen keyboard.
          auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
          if (self->is_requested_show_virtual_keyboard_) {
            self->ShowVirtualKeyboard();
          }
        },
        .leave = [](void* data,
                    zwp_text_input_v3* zwp_text_input_v3,
                    wl_surface* surface) -> void {
          ELINUX_LOG(TRACE) << "zwp_text_input_v3_listener.leave";
        },
        .preedit_string = [](void* data,
                             zwp_text_input_v3* zwp_text_input_v3,
                             const char* text,
                             int32_t cursor_begin,
                             int32_t cursor_end) -> void {
          ELINUX_LOG(TRACE) << "zwp_text_input_v3_listener.preedit_string";
        },
        .commit_string = [](void* data,
                            zwp_text_input_v3* zwp_text_input_v3,
                            const char* text) -> void {
          ELINUX_LOG(TRACE) << "zwp_text_input_v3_listener.commit_string";

          auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
          if (self->binding_handler_delegate_ && strlen(text)) {
            std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t>
                utf8_converter;
            std::u16string utf16_text = utf8_converter.from_bytes(text);
            self->binding_handler_delegate_->OnVirtualKey(utf16_text[0]);
          }
        },
        .delete_surrounding_text = [](void* data,
                                      zwp_text_input_v3* zwp_text_input_v3,
                                      uint32_t before_length,
                                      uint32_t after_length) -> void {
          ELINUX_LOG(TRACE)
              << "zwp_text_input_v3_listener.delete_surrounding_text";
        },
        .done = [](void* data,
                   zwp_text_input_v3* zwp_text_input_v3,
                   uint32_t serial) -> void {
          ELINUX_LOG(TRACE) << "zwp_text_input_v3_listener.done";
        },
};

const wl_data_device_listener ELinuxWindowWayland::kWlDataDeviceListener = {
    .data_offer = [](void* data,
                     wl_data_device* wl_data_device,
                     wl_data_offer* offer) -> void {
      ELINUX_LOG(TRACE) << "wl_data_device_listener.data_offer";
    },
    .enter = [](void* data,
                wl_data_device* wl_data_device,
                uint32_t serial,
                wl_surface* surface,
                wl_fixed_t x,
                wl_fixed_t y,
                wl_data_offer* offer) -> void {
      ELINUX_LOG(TRACE) << "wl_data_device_listener.enter";
    },
    .leave = [](void* data, wl_data_device* wl_data_device) -> void {
      ELINUX_LOG(TRACE) << "wl_data_device_listener.leave";
    },
    .motion = [](void* data,
                 wl_data_device* wl_data_device,
                 uint32_t time,
                 wl_fixed_t x,
                 wl_fixed_t y) -> void {
      ELINUX_LOG(TRACE) << "wl_data_device_listener.motion";
    },
    .drop = [](void* data, wl_data_device* wl_data_device) -> void {
      ELINUX_LOG(TRACE) << "wl_data_device_listener.drop";
    },
    .selection = [](void* data,
                    wl_data_device* wl_data_device,
                    wl_data_offer* offer) -> void {
      ELINUX_LOG(TRACE) << "wl_data_device_listener.selection";

      auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
      if (self->wl_data_offer_) {
        wl_data_offer_destroy(self->wl_data_offer_);
      }
      self->wl_data_offer_ = offer;
    },
};

const wl_data_source_listener ELinuxWindowWayland::kWlDataSourceListener = {
    .target = [](void* data,
                 wl_data_source* wl_data_source,
                 const char* mime_type) -> void {
      ELINUX_LOG(TRACE) << "wl_data_device_listener.target";
    },
    .send = [](void* data,
               wl_data_source* wl_data_source,
               const char* mime_type,
               int32_t fd) -> void {
      ELINUX_LOG(TRACE) << "wl_data_device_listener.send";

      if (strcmp(mime_type, kClipboardMimeTypeText)) {
        ELINUX_LOG(ERROR) << "Not expected mime_type: " << mime_type;
        return;
      }
      auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
      // Write the copied data to the clipboard.
      write(fd, self->clipboard_data_.c_str(),
            strlen(self->clipboard_data_.c_str()));
      close(fd);
    },
    .cancelled = [](void* data, wl_data_source* wl_data_source) -> void {
      ELINUX_LOG(TRACE) << "wl_data_device_listener.cancelled";

      auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
      self->clipboard_data_ = "";
      if (self->wl_data_source_) {
        wl_data_source_destroy(self->wl_data_source_);
        self->wl_data_source_ = nullptr;
      }
    },
    .dnd_drop_performed = [](void* data,
                             wl_data_source* wl_data_source) -> void {
      ELINUX_LOG(TRACE) << "wl_data_device_listener.dnd_drop_performed";
    },
    .dnd_finished = [](void* data, wl_data_source* wl_data_source) -> void {
      ELINUX_LOG(TRACE) << "wl_data_device_listener.dnd_finished";
    },
    .action = [](void* data,
                 wl_data_source* wl_data_source,
                 uint32_t dnd_action) -> void {
      ELINUX_LOG(TRACE) << "wl_data_device_listener.action";
    },
};

const zxdg_toplevel_decoration_v1_listener
    ELinuxWindowWayland::kZxdgToplevelDecorationV1Listener = {
        .configure =
            [](void* data,
               struct zxdg_toplevel_decoration_v1* zxdg_toplevel_decoration_v1,
               uint32_t mode) -> void {
          ELINUX_LOG(INFO)
              << "zxdg_toplevel_decoration_v1_listener.configure: mode is "
              << ((mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE)
                      ? "server-side"
                      : "client-side");
          auto self = reinterpret_cast<ELinuxWindowWayland*>(data);
          if (self->view_properties_.use_window_decoration &&
              mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE &&
              !self->window_decorations_) {
            int32_t width_dip = self->view_properties_.width;
            int32_t height_dip = self->view_properties_.height;
            self->CreateDecoration(width_dip, height_dip);
          }
        },
};

ELinuxWindowWayland::ELinuxWindowWayland(
    FlutterDesktopViewProperties view_properties)
    : cursor_info_({"", 0, nullptr}),
      display_valid_(false),
      running_(false),
      maximised_(false),
      is_requested_show_virtual_keyboard_(false),
      xdg_toplevel_(nullptr),
      wl_compositor_(nullptr),
      wl_subcompositor_(nullptr),
      wl_current_surface_(nullptr),
      wl_shm_(nullptr),
      seat_inputs_map_(),
      wl_data_device_manager_(nullptr),
      wl_data_device_(nullptr),
      wl_data_offer_(nullptr),
      wl_data_source_(nullptr),
      serial_(0),
      text_input_serial_(0),
      zwp_text_input_manager_v1_(nullptr),
      zwp_text_input_manager_v3_(nullptr),
      zwp_text_input_v1_(nullptr),
      zwp_text_input_v3_(nullptr),
      wp_presentation_(nullptr),
      wp_presentation_clk_id_(UINT32_MAX),
      window_decorations_(nullptr) {
  view_properties_ = view_properties;
  current_scale_ = 1.0;
  device_pixel_ratio_ = 1.0;
  device_pixel_ratio_locked_ = false;
  if (view_properties.force_scale_factor &&
      view_properties.scale_factor > 0) {
    device_pixel_ratio_ = view_properties.scale_factor;
    device_pixel_ratio_locked_ = true;
  } else if (const double ratio = ReadDevicePixelRatio(); ratio > 0.0) {
    device_pixel_ratio_ = ratio;
    device_pixel_ratio_locked_ = true;
  }
  display_cutouts_ = ReadDisplayCutouts();
  for (const auto& c : display_cutouts_) {
    ELINUX_LOG(INFO) << "Display cut-out: " << c.width << "x" << c.height
                     << " at " << c.left << "," << c.top;
  }
  SetRotation(view_properties_.view_rotation);

  // Lipstick reports rotation only through the wl_output transform. Follow it
  // unless a fixed rotation was requested.
  // FLUTTER_SFOS_ROTATION=0 turns this off, and so does FLUTTER_SFOS_SHELL=xdg,
  // which keeps xdg_shell and the upstream behaviour.
  {
    const char* env = std::getenv("FLUTTER_SFOS_SHELL");
    force_xdg_shell_ = env && std::strcmp(env, "xdg") == 0;
  }
  if (view_properties_.view_rotation == FlutterDesktopViewRotation::kRotation_0 &&
      !force_xdg_shell_) {
    const char* env = std::getenv("FLUTTER_SFOS_ROTATION");
    follow_output_transform_ = !(env && std::strcmp(env, "0") == 0);
  }

  auto xcursor_size_string = std::getenv(kXcursorSizeEnvironmentKey);
  cursor_size_ =
      xcursor_size_string ? atoi(xcursor_size_string) : kDefaultPointerSize;
  if (cursor_size_ <= 0) {
    cursor_size_ = kDefaultPointerSize;
  }

  wl_display_ = wl_display_connect(nullptr);
  if (!wl_display_) {
    ELINUX_LOG(ERROR) << "Failed to connect to the Wayland display.";
    return;
  }

  wl_registry_ = wl_display_get_registry(wl_display_);
  if (!wl_registry_) {
    ELINUX_LOG(ERROR) << "Failed to get the wayland registry.";
    return;
  }

  wl_registry_add_listener(wl_registry_, &kWlRegistryListener, this);
  wl_display_roundtrip(wl_display_);

  for (auto& [seat, _] : seat_inputs_map_) {
    if (wl_data_device_manager_ && seat) {
      wl_data_device_ =
          wl_data_device_manager_get_data_device(wl_data_device_manager_, seat);
      wl_data_device_add_listener(wl_data_device_, &kWlDataDeviceListener,
                                  this);
    }

    // Setup text-input protocol for onscreen keyboard inputs.
    {
      if (zwp_text_input_manager_v3_ && seat) {
        zwp_text_input_v3_ = zwp_text_input_manager_v3_get_text_input(
            zwp_text_input_manager_v3_, seat);
        if (!zwp_text_input_v3_) {
          ELINUX_LOG(ERROR) << "Failed to create the text input manager v3.";
          return;
        }
        zwp_text_input_v3_add_listener(zwp_text_input_v3_,
                                       &kZwpTextInputV3Listener, this);
      } else if (zwp_text_input_manager_v1_) {
        zwp_text_input_v1_ = zwp_text_input_manager_v1_create_text_input(
            zwp_text_input_manager_v1_);
        if (!zwp_text_input_v1_) {
          ELINUX_LOG(ERROR) << "Failed to create text input manager v1.";
          return;
        }
        zwp_text_input_v1_add_listener(zwp_text_input_v1_,
                                       &kZwpTextInputV1Listener, this);
      } else {
        // do nothing.
      }
    }
  }

  display_valid_ = true;
  running_ = true;
}

ELinuxWindowWayland::~ELinuxWindowWayland() {
  display_valid_ = false;
  running_ = false;

  for (auto theme : wl_cursor_themes_) {
    wl_cursor_theme_destroy(theme.second);
  }
  wl_cursor_themes_.clear();

  {
    if (zwp_text_input_v1_) {
      zwp_text_input_v1_destroy(zwp_text_input_v1_);
      zwp_text_input_v1_ = nullptr;
    }

    if (zwp_text_input_manager_v1_) {
      zwp_text_input_manager_v1_destroy(zwp_text_input_manager_v1_);
      zwp_text_input_manager_v1_ = nullptr;
    }

    if (zwp_text_input_v3_) {
      zwp_text_input_v3_destroy(zwp_text_input_v3_);
      zwp_text_input_v3_ = nullptr;
    }

    if (zwp_text_input_manager_v3_) {
      zwp_text_input_manager_v3_destroy(zwp_text_input_manager_v3_);
      zwp_text_input_manager_v3_ = nullptr;
    }
  }

  {
    if (zxdg_decoration_manager_v1_) {
      zxdg_decoration_manager_v1_destroy(zxdg_decoration_manager_v1_);
      zxdg_decoration_manager_v1_ = nullptr;
    }

    if (zxdg_toplevel_decoration_v1_) {
      zxdg_toplevel_decoration_v1_destroy(zxdg_toplevel_decoration_v1_);
      zxdg_toplevel_decoration_v1_ = nullptr;
    }
  }

  if (wl_data_offer_) {
    wl_data_offer_destroy(wl_data_offer_);
    wl_data_offer_ = nullptr;
  }

  if (wl_data_source_) {
    wl_data_source_destroy(wl_data_source_);
    wl_data_source_ = nullptr;
  }

  if (wl_data_device_) {
    if (wl_data_device_manager_version_ >=
        WL_DATA_DEVICE_RELEASE_SINCE_VERSION) {
      wl_data_device_release(wl_data_device_);
    } else {
      wl_data_device_destroy(wl_data_device_);
    }
    wl_data_device_ = nullptr;
  }

  if (wl_data_device_manager_) {
    wl_data_device_manager_destroy(wl_data_device_manager_);
    wl_data_device_manager_ = nullptr;
  }

  for (auto& [seat, inputs] : seat_inputs_map_) {
    if (inputs.pointer) {
      wl_pointer_destroy(inputs.pointer);
      inputs.pointer = nullptr;
    }

    if (inputs.touch) {
      wl_touch_destroy(inputs.touch);
      inputs.touch = nullptr;
    }

    if (inputs.keyboard) {
      wl_keyboard_destroy(inputs.keyboard);
      inputs.keyboard = nullptr;
    }

    if (seat) {
      wl_seat_destroy(seat);
    }
  }

  seat_inputs_map_.clear();

  if (wl_output_) {
    wl_output_destroy(wl_output_);
    wl_output_ = nullptr;
  }

  if (wl_shm_) {
    wl_shm_destroy(wl_shm_);
    wl_shm_ = nullptr;
  }

  if (xdg_toplevel_) {
    xdg_toplevel_destroy(xdg_toplevel_);
    xdg_toplevel_ = nullptr;
  }

  if (xdg_wm_base_) {
    xdg_wm_base_destroy(xdg_wm_base_);
    xdg_wm_base_ = nullptr;
  }

  if (qt_surface_extension_) {
    qt_surface_extension_destroy(qt_surface_extension_);
    qt_surface_extension_ = nullptr;
  }

  if (wl_shell_) {
    wl_shell_destroy(wl_shell_);
    wl_shell_ = nullptr;
  }

  if (wl_compositor_) {
    wl_compositor_destroy(wl_compositor_);
    wl_compositor_ = nullptr;
  }

  if (wl_subcompositor_) {
    wl_subcompositor_destroy(wl_subcompositor_);
    wl_subcompositor_ = nullptr;
  }

  if (wl_registry_) {
    wl_registry_destroy(wl_registry_);
    wl_registry_ = nullptr;
  }

  if (wl_display_) {
    wl_display_flush(wl_display_);
    wl_display_disconnect(wl_display_);
    wl_display_ = nullptr;
  }
}

void ELinuxWindowWayland::SetView(WindowBindingHandlerDelegate* window) {
  binding_handler_delegate_ = window;
}

ELinuxRenderSurfaceTarget* ELinuxWindowWayland::GetRenderSurfaceTarget() const {
  return render_surface_.get();
}

uint16_t ELinuxWindowWayland::GetRotationDegree() const {
  return current_rotation_;
}

double ELinuxWindowWayland::GetDpiScale() {
  return device_pixel_ratio_;
}

PhysicalWindowBounds ELinuxWindowWayland::GetPhysicalWindowBounds() {
  return {GetCurrentWidth(), GetCurrentHeight()};
}

int32_t ELinuxWindowWayland::GetFrameRate() {
  return frame_rate_;
}

bool ELinuxWindowWayland::DispatchEvent() {
  if (!IsValid()) {
    ELINUX_LOG(ERROR) << "Wayland display is invalid.";
    return false;
  }

  if (!running_) {
    return false;
  }

  if (request_redraw_) {
    request_redraw_ = false;
    if (window_decorations_) {
      window_decorations_->Resize(view_properties_.width,
                                  view_properties_.height, current_scale_);
    }

    if (binding_handler_delegate_) {
      binding_handler_delegate_->OnWindowSizeChanged(
          view_properties_.width * current_scale_,
          view_properties_.height * current_scale_ -
              WindowDecorationsPhysicalHeight());
    }
    NotifyDisplayInfoUpdates();
  }

  // Prepare to call wl_display_read_events.
  while (wl_display_prepare_read(wl_display_) != 0) {
    // If Wayland compositor terminates, -1 is returned.
    auto result = wl_display_dispatch_pending(wl_display_);
    if (result == -1) {
      return false;
    }
  }
  wl_display_flush(wl_display_);

  // Handle Vsync.
  if (binding_handler_delegate_) {
    const uint64_t vsync_interval_time_nanos = 1000000000000 / frame_rate_;
    binding_handler_delegate_->OnVsync(last_frame_time_nanos_,
                                       vsync_interval_time_nanos);
  }

  // Handle Wayland events.
  pollfd fds[] = {
      {wl_display_get_fd(wl_display_), POLLIN},
  };
  if (poll(fds, 1, 0) > 0) {
    auto result = wl_display_read_events(wl_display_);
    if (result == -1) {
      return false;
    }

    result = wl_display_dispatch_pending(wl_display_);
    if (result == -1) {
      return false;
    }
  } else {
    wl_display_cancel_read(wl_display_);
  }

  return true;
}

bool ELinuxWindowWayland::CreateRenderSurface(int32_t width_px,
                                              int32_t height_px,
                                              bool enable_impeller) {
  enable_impeller_ = enable_impeller;

  if (!display_valid_) {
    ELINUX_LOG(ERROR) << "Wayland display is invalid.";
    return false;
  }

  if (!wl_compositor_) {
    ELINUX_LOG(ERROR) << "Wl_compositor is invalid";
    return false;
  }

  // Sailfish: native apps use wl_shell with the Qt surface extension, and
  // Lipstick only treats set_buffer_transform as the content orientation for
  // those surfaces.
  use_wl_shell_ =
      !force_xdg_shell_ && wl_shell_ && qt_surface_extension_ &&
      view_properties_.view_mode == FlutterDesktopViewMode::kFullscreen;
  ELINUX_LOG(INFO) << "Shell: " << (use_wl_shell_ ? "wl_shell" : "xdg_shell");

  if (!use_wl_shell_ && !xdg_wm_base_) {
    ELINUX_LOG(ERROR) << "Xdg-shell is invalid";
    return false;
  }

  if (view_properties_.view_mode == FlutterDesktopViewMode::kFullscreen) {
    if (view_properties_.force_scale_factor && display_max_width_ > 0 &&
        display_max_height_ > 0) {
      // When force_scale_factor is active, the surface must be the native
      // display resolution (display_max). Using display_max directly avoids
      // floating-point rounding when current_scale_ is non-integer (e.g. 1.3).
      width_px = display_max_width_;
      height_px = display_max_height_;
    } else {
      width_px = view_properties_.width * current_scale_;
      height_px = view_properties_.height * current_scale_;
    }
  }

  ELINUX_LOG(TRACE) << "Created the Wayland surface: " << width_px << "x"
                    << height_px;
  if (view_properties_.use_mouse_cursor) {
    wl_cursor_surface_ = wl_compositor_create_surface(wl_compositor_);
    if (!wl_cursor_surface_) {
      ELINUX_LOG(ERROR)
          << "Failed to create the compositor surface for cursor.";
      return false;
    }
  }

  if (current_rotation_ == 90 || current_rotation_ == 270) {
    std::swap(width_px, height_px);
  }
  native_window_ = std::make_unique<NativeWindowWayland>(
      wl_compositor_, width_px, height_px, view_properties_.enable_vsync);

  wl_surface_add_listener(native_window_->Surface(), &kWlSurfaceListener, this);
  if (use_wl_shell_ && applied_transform_ != WL_OUTPUT_TRANSFORM_NORMAL) {
    wl_surface_set_buffer_transform(native_window_->Surface(),
                                    applied_transform_);
  }

  if (use_wl_shell_) {
    wl_shell_surface_ =
        wl_shell_get_shell_surface(wl_shell_, native_window_->Surface());
    if (!wl_shell_surface_) {
      ELINUX_LOG(ERROR) << "Failed to get the wl_shell surface.";
      return false;
    }
    wl_shell_surface_add_listener(wl_shell_surface_, &kWlShellSurfaceListener,
                                  this);
    wl_shell_surface_set_toplevel(wl_shell_surface_);
    if (view_properties_.title != nullptr) {
      wl_shell_surface_set_title(wl_shell_surface_, view_properties_.title);
    }
    if (view_properties_.app_id != nullptr) {
      wl_shell_surface_set_class(wl_shell_surface_, view_properties_.app_id);
    }

    qt_extended_surface_ = qt_surface_extension_get_extended_surface(
        qt_surface_extension_, native_window_->Surface());
    qt_extended_surface_add_listener(qt_extended_surface_,
                                     &kQtExtendedSurfaceListener, this);
    UpdateContentOrientationMask();
    qt_extended_surface_set_window_flags(qt_extended_surface_, 0);
    if (!cover_actions_.empty()) {
      SetStringWindowProperty(qt_extended_surface_, "_APP_COVER_ACTION",
                              cover_actions_);
    }
  } else {
    xdg_surface_ =
        xdg_wm_base_get_xdg_surface(xdg_wm_base_, native_window_->Surface());
    if (!xdg_surface_) {
      ELINUX_LOG(ERROR) << "Failed to get the xdg surface.";
      return false;
    }
    xdg_surface_add_listener(xdg_surface_, &kXdgSurfaceListener, this);

    xdg_toplevel_ = xdg_surface_get_toplevel(xdg_surface_);
    if (view_properties_.title != nullptr) {
      xdg_toplevel_set_title(xdg_toplevel_, view_properties_.title);
    }
    if (view_properties_.app_id != nullptr) {
      xdg_toplevel_set_app_id(xdg_toplevel_, view_properties_.app_id);
    }
    xdg_toplevel_add_listener(xdg_toplevel_, &kXdgToplevelListener, this);
  }
  wl_surface_set_buffer_scale(native_window_->Surface(), current_scale_);

  {
    auto* callback = wl_surface_frame(native_window_->Surface());
    wl_callback_add_listener(callback, &kWlSurfaceFrameListener, this);

    if (wp_presentation_) {
      wp_presentation_feedback_add_listener(
          ::wp_presentation_feedback(wp_presentation_,
                                     native_window_->Surface()),
          &kWpPresentationFeedbackListener, this);
    }
  }

  if (view_properties_.view_mode == FlutterDesktopViewMode::kFullscreen) {
    if (use_wl_shell_) {
      wl_shell_surface_set_fullscreen(
          wl_shell_surface_, WL_SHELL_SURFACE_FULLSCREEN_METHOD_DEFAULT, 0,
          nullptr);
    } else {
      xdg_toplevel_set_fullscreen(xdg_toplevel_, NULL);
    }
  }

  // wl_shell surfaces are configured after their first buffer, so only an
  // xdg_surface needs waiting for.
  wait_for_configure_ = !use_wl_shell_;
  wl_surface_commit(native_window_->Surface());

  render_surface_ = std::make_unique<SurfaceGl>(std::make_unique<ContextEgl>(
      std::make_unique<EnvironmentEgl>(EGL_PLATFORM_WAYLAND_KHR, wl_display_),
      enable_impeller));
  render_surface_->SetNativeWindow(native_window_.get());

  if (view_properties_.use_window_decoration && !use_wl_shell_) {
    if (zxdg_decoration_manager_v1_) {
      ELINUX_LOG(INFO) << "Use server-side xdg-decoration mode";
      zxdg_toplevel_decoration_v1_ =
          zxdg_decoration_manager_v1_get_toplevel_decoration(
              zxdg_decoration_manager_v1_, xdg_toplevel_);
      zxdg_toplevel_decoration_v1_set_mode(
          zxdg_toplevel_decoration_v1_,
          ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
      zxdg_toplevel_decoration_v1_add_listener(
          zxdg_toplevel_decoration_v1_, &kZxdgToplevelDecorationV1Listener,
          this);
    } else {
      int32_t width_dip = width_px / current_scale_;
      int32_t height_dip = height_px / current_scale_;
      CreateDecoration(width_dip, height_dip);
    }
  }

  // Wait for making sure that xdg_surface has been configured.
  while (wait_for_configure_) {
    wl_display_dispatch(wl_display_);
  }

  return true;
}

void ELinuxWindowWayland::CreateDecoration(int32_t width_dip,
                                           int32_t height_dip) {
  if (window_decorations_) {
    ELINUX_LOG(WARNING) << "Window decoration has already created";
    return;
  }

  window_decorations_ = std::make_unique<WindowDecorationsWayland>(
      wl_display_, wl_compositor_, wl_subcompositor_, native_window_->Surface(),
      width_dip, height_dip, current_scale_, enable_impeller_,
      view_properties_.enable_vsync);
}

void ELinuxWindowWayland::DestroyRenderSurface() {
  // The cover is transient for the main surface, so it goes first.
  cover_window_ = nullptr;

  // destroy the main surface before destroying the client window on Wayland.
  if (window_decorations_) {
    window_decorations_ = nullptr;
  }
  render_surface_ = nullptr;
  native_window_ = nullptr;

  if (xdg_surface_) {
    xdg_surface_destroy(xdg_surface_);
    xdg_surface_ = nullptr;
  }

  if (qt_extended_surface_) {
    qt_extended_surface_destroy(qt_extended_surface_);
    qt_extended_surface_ = nullptr;
  }

  if (wl_shell_surface_) {
    wl_shell_surface_destroy(wl_shell_surface_);
    wl_shell_surface_ = nullptr;
  }

  if (wl_cursor_surface_) {
    wl_surface_destroy(wl_cursor_surface_);
    wl_cursor_surface_ = nullptr;
  }
}

PhysicalViewInsets ELinuxWindowWayland::GetViewInsets() {
  return {.bottom = keyboard_inset_bottom_};
}

void ELinuxWindowWayland::UpdateVirtualKeyboardArea(int32_t x,
                                                    int32_t y,
                                                    int32_t width,
                                                    int32_t height) {
  ELINUX_LOG(TRACE) << "Input method area: " << x << "," << y << " " << width
                    << "x" << height << " (rotation " << current_rotation_
                    << ")";
  if (width > 0 && height > 0 &&
      (current_rotation_ == 90 || current_rotation_ == 180 ||
       current_rotation_ == 270)) {
    // Maliit reports the area in the display's native orientation; map it
    // into the rotated view the same way touch points are mapped.
    const double view_w = GetCurrentWidth();
    const double view_h = GetCurrentHeight();
    const double x0 = x;
    const double y0 = y;
    const double x1 = x0 + width;
    const double y1 = y0 + height;
    double left = 0;
    double top = 0;
    double right = 0;
    double bottom = 0;
    if (current_rotation_ == 90) {
      left = y0;
      right = y1;
      top = view_h - x1;
      bottom = view_h - x0;
    } else if (current_rotation_ == 180) {
      left = view_w - x1;
      right = view_w - x0;
      top = view_h - y1;
      bottom = view_h - y0;
    } else {
      left = view_w - y1;
      right = view_w - y0;
      top = x0;
      bottom = x1;
    }
    x = static_cast<int32_t>(left);
    y = static_cast<int32_t>(top);
    width = static_cast<int32_t>(right - left);
    height = static_cast<int32_t>(bottom - top);
    ELINUX_LOG(TRACE) << "Input method area in view: " << x << "," << y << " "
                      << width << "x" << height;
  }
  double inset = 0.0;
  const int32_t win_h = static_cast<int32_t>(GetCurrentHeight());
  if (width > 0 && height > 0 && win_h > 0) {
    if (y > 0 && y < win_h) {
      inset = static_cast<double>(win_h - y);
    } else {
      inset = static_cast<double>(height);
    }
    // Extra space for rounded-corner / home-indicator overlap.
    inset += 32.0;
    if (inset > win_h) {
      inset = win_h;
    }
  }
  if (inset == keyboard_inset_bottom_) {
    return;
  }
  keyboard_inset_bottom_ = inset;
  if (binding_handler_delegate_) {
    binding_handler_delegate_->OnWindowMetricsChanged();
  }
}

void ELinuxWindowWayland::UpdateVirtualKeyboardStatus(
    const bool show,
    const std::string& input_type) {
  // Not supported virtual keyboard.
  if (!(zwp_text_input_v1_ || zwp_text_input_v3_) || seat_inputs_map_.empty()) {
    return;
  }

  text_input_type_ = input_type;
  is_requested_show_virtual_keyboard_ = show;
  if (is_requested_show_virtual_keyboard_) {
    ShowVirtualKeyboard();
  } else {
    DismissVirtualKeybaord();
  }
}

void ELinuxWindowWayland::UpdateFlutterCursor(const std::string& cursor_name) {
  if (view_properties_.use_mouse_cursor) {
    if (!cursor_info_.pointer) {
      return;
    }
    if (cursor_name.compare(cursor_info_.cursor_name) == 0) {
      return;
    }
    cursor_info_.cursor_name = cursor_name;

    if (cursor_name.compare(kCursorNameNone) == 0) {
      // Turn off the cursor.
      wl_pointer_set_cursor(cursor_info_.pointer, cursor_info_.serial,
                            wl_cursor_surface_, 0, 0);
      wl_surface_attach(wl_cursor_surface_, nullptr, 0, 0);
      wl_surface_damage(wl_cursor_surface_, 0, 0, 0, 0);
      wl_surface_commit(wl_cursor_surface_);
      return;
    }

    auto wl_cursor = GetWlCursor(cursor_name, cursor_size_ * current_scale_);
    if (!wl_cursor) {
      return;
    }
    auto image = wl_cursor->images[0];
    auto buffer = wl_cursor_image_get_buffer(image);
    if (buffer) {
      wl_pointer_set_cursor(
          cursor_info_.pointer, cursor_info_.serial, wl_cursor_surface_,
          image->hotspot_x / current_scale_, image->hotspot_y / current_scale_);
      wl_surface_attach(wl_cursor_surface_, buffer, 0, 0);
      wl_surface_damage(wl_cursor_surface_, 0, 0, image->width, image->height);
      wl_surface_set_buffer_scale(wl_cursor_surface_, current_scale_);
      wl_surface_commit(wl_cursor_surface_);
    }
  }
}

std::string ELinuxWindowWayland::GetClipboardData() {
  // If we own the data source, we can return the local data directly.
  // This avoids a synchronous read() and prevents a deadlock when
  // pasting data copied from this app.
  if (wl_data_source_) {
    return clipboard_data_;
  }

  std::string str = "";

  if (wl_data_offer_) {
    int fd[2];
    if (pipe2(fd, O_CLOEXEC) == -1) {
      return str;
    }

    wl_data_offer_receive(wl_data_offer_, kClipboardMimeTypeText, fd[1]);
    close(fd[1]);
    wl_display_dispatch(wl_display_);

    char buf[256];
    int len;
    // Read data form the clipboard.
    while ((len = read(fd[0], buf, sizeof(buf))) > 0) {
      str.append(buf, len);
    }
    close(fd[0]);
    return str;
  }

  return str;
}

void ELinuxWindowWayland::SetClipboardData(const std::string& data) {
  clipboard_data_ = data;
  if (wl_data_device_manager_) {
    if (wl_data_source_) {
      wl_data_source_destroy(wl_data_source_);
      wl_data_source_ = nullptr;
    }

    wl_data_source_ =
        wl_data_device_manager_create_data_source(wl_data_device_manager_);
    if (!wl_data_source_) {
      return;
    }

    wl_data_source_offer(wl_data_source_, kClipboardMimeTypeText);
    wl_data_source_add_listener(wl_data_source_, &kWlDataSourceListener, this);
    wl_data_device_set_selection(wl_data_device_, wl_data_source_, serial_);
  }
}

bool ELinuxWindowWayland::IsValid() const {
  if (!display_valid_ || !native_window_ || !render_surface_ ||
      !native_window_->IsValid() || !render_surface_->IsValid()) {
    return false;
  }
  return true;
}

void ELinuxWindowWayland::WlRegistryHandler(wl_registry* wl_registry,
                                            uint32_t name,
                                            const char* interface,
                                            uint32_t version) {
  if (!strcmp(interface, wl_compositor_interface.name)) {
    constexpr uint32_t kMaxVersion = 5;
    wl_compositor_ = static_cast<decltype(wl_compositor_)>(
        wl_registry_bind(wl_registry, name, &wl_compositor_interface,
                         std::min(kMaxVersion, version)));
    return;
  }

  if (!strcmp(interface, wl_subcompositor_interface.name)) {
    constexpr uint32_t kMaxVersion = 1;
    wl_subcompositor_ = static_cast<wl_subcompositor*>(wl_registry_bind(
        wl_registry, name, &wl_subcompositor_interface, kMaxVersion));
  }

  if (!strcmp(interface, xdg_wm_base_interface.name)) {
    constexpr uint32_t kMaxVersion = 3;
    xdg_wm_base_ = static_cast<decltype(xdg_wm_base_)>(
        wl_registry_bind(wl_registry, name, &xdg_wm_base_interface,
                         std::min(kMaxVersion, version)));
    xdg_wm_base_add_listener(xdg_wm_base_, &kXdgWmBaseListener, this);
    return;
  }

  if (!strcmp(interface, wl_shell_interface.name)) {
    wl_shell_ = static_cast<decltype(wl_shell_)>(
        wl_registry_bind(wl_registry, name, &wl_shell_interface, 1));
    return;
  }

  if (!strcmp(interface, qt_surface_extension_interface.name)) {
    qt_surface_extension_ = static_cast<decltype(qt_surface_extension_)>(
        wl_registry_bind(wl_registry, name, &qt_surface_extension_interface,
                         1));
    return;
  }

  if (!strcmp(interface, wl_seat_interface.name)) {
    constexpr uint32_t kMaxVersion = 4;
    auto [inserted, _] =
        seat_inputs_map_.emplace(static_cast<wl_seat*>(wl_registry_bind(
                                     wl_registry, name, &wl_seat_interface,
                                     std::min(kMaxVersion, version))),
                                 seat_inputs());
    registry_names_to_seat_ptr_.emplace(name, inserted->first);
    wl_seat_add_listener(inserted->first, &kWlSeatListener, this);
    return;
  }

  if (!strcmp(interface, wl_output_interface.name)) {
    constexpr uint32_t kMaxVersion = 2;
    wl_output_ = static_cast<decltype(wl_output_)>(
        wl_registry_bind(wl_registry, name, &wl_output_interface,
                         std::min(kMaxVersion, version)));
    wl_output_add_listener(wl_output_, &kWlOutputListener, this);
    return;
  }

  if (!strcmp(interface, wl_shm_interface.name)) {
    if (view_properties_.use_mouse_cursor) {
      constexpr uint32_t kMaxVersion = 1;
      wl_shm_ = static_cast<decltype(wl_shm_)>(
          wl_registry_bind(wl_registry, name, &wl_shm_interface, kMaxVersion));
    }
    return;
  }

  if (!strcmp(interface, kZwpTextInputManagerV1)) {
    if (view_properties_.use_onscreen_keyboard) {
      constexpr uint32_t kMaxVersion = 1;
      zwp_text_input_manager_v1_ =
          static_cast<decltype(zwp_text_input_manager_v1_)>(wl_registry_bind(
              wl_registry, name, &zwp_text_input_manager_v1_interface,
              kMaxVersion));
    }
    return;
  }

  if (!strcmp(interface, kZwpTextInputManagerV3)) {
    if (view_properties_.use_onscreen_keyboard) {
      constexpr uint32_t kMaxVersion = 1;
      zwp_text_input_manager_v3_ =
          static_cast<decltype(zwp_text_input_manager_v3_)>(wl_registry_bind(
              wl_registry, name, &zwp_text_input_manager_v3_interface,
              std::min(kMaxVersion, version)));
    }
    return;
  }

  if (!strcmp(interface, wl_data_device_manager_interface.name)) {
    // Save the version of wl_data_device_manager because the release method of
    // wl_data_device differs depending on it. Since wl_data_device_manager has
    // been released up to version 3, set the upper limit to 3.
    constexpr uint32_t kMaxVersion = 3;
    wl_data_device_manager_version_ = std::min(kMaxVersion, version);
    wl_data_device_manager_ = static_cast<decltype(wl_data_device_manager_)>(
        wl_registry_bind(wl_registry, name, &wl_data_device_manager_interface,
                         wl_data_device_manager_version_));
    return;
  }

  if (!strcmp(interface, wp_presentation_interface.name)) {
    constexpr uint32_t kMaxVersion = 1;
    wp_presentation_ = static_cast<decltype(wp_presentation_)>(wl_registry_bind(
        wl_registry, name, &wp_presentation_interface, kMaxVersion));
    wp_presentation_add_listener(wp_presentation_, &kWpPresentationListener,
                                 this);
    return;
  }

  if (!strcmp(interface, kZxdgDecorationManagerV1)) {
    constexpr uint32_t kMaxVersion = 1;
    zxdg_decoration_manager_v1_ =
        static_cast<decltype(zxdg_decoration_manager_v1_)>(wl_registry_bind(
            wl_registry, name, &zxdg_decoration_manager_v1_interface,
            std::min(kMaxVersion, version)));
    return;
  }
}

void ELinuxWindowWayland::WlUnRegistryHandler(wl_registry* wl_registry,
                                              uint32_t name) {
  // Just remove seats for now, as we don't need to do anything else.
  // But there is also no interface name here.
  auto seat_iter = registry_names_to_seat_ptr_.find(name);
  if (seat_iter != registry_names_to_seat_ptr_.end()) {
    auto seat = seat_iter->second;
    auto seat_inputs_iter = seat_inputs_map_.find(seat);
    if (seat_inputs_iter != seat_inputs_map_.end()) {
      auto& inputs = seat_inputs_iter->second;
      if (inputs.pointer) {
        if (cursor_info_.pointer == inputs.pointer) {
          cursor_info_.pointer = nullptr;
        }
        wl_pointer_release(inputs.pointer);
        inputs.pointer = nullptr;
      }
      if (inputs.touch) {
        wl_touch_release(inputs.touch);
        inputs.touch = nullptr;
      }
      if (inputs.keyboard) {
        wl_keyboard_release(inputs.keyboard);
        inputs.keyboard = nullptr;
      }
      seat_inputs_map_.erase(seat_inputs_iter);
    }
    if (seat) {
      wl_seat_destroy(seat);
    }
    registry_names_to_seat_ptr_.erase(name);
  }
}

bool ELinuxWindowWayland::LoadCursorTheme(uint32_t size) {
  if (!wl_shm_) {
    ELINUX_LOG(ERROR) << "Failed to load cursor theme because shared memory "
                         "buffers are not available.";
    return false;
  }

  auto theme = wl_cursor_theme_load(nullptr, size, wl_shm_);
  if (!theme) {
    ELINUX_LOG(ERROR) << "Failed to load cursor theme for size: " << size;
    return false;
  }

  wl_cursor_themes_[size] = theme;

  std::vector<std::string> wl_cursor_names{
      kWlCursorThemeLeftPtr,
      kWlCursorThemeBottomLeftCorner,
      kWlCursorThemeBottomRightCorner,
      kWlCursorThemeBottomSide,
      kWlCursorThemeGrabbing,
      kWlCursorThemeLeftSide,
      kWlCursorThemeRightSide,
      kWlCursorThemeTopLeftCorner,
      kWlCursorThemeTopRightCorner,
      kWlCursorThemeTopSide,
      kWlCursorThemeXterm,
      kWlCursorThemeHand1,
      kWlCursorThemeWatch,
  };

  std::unordered_map<std::string, wl_cursor*> cursor_list;

  for (const auto& cursor_name : wl_cursor_names) {
    auto wl_cursor = wl_cursor_theme_get_cursor(theme, cursor_name.c_str());
    if (!wl_cursor) {
      ELINUX_LOG(ERROR) << "Unsupported cursor theme: " << cursor_name.c_str();
      continue;
    }
    cursor_list[cursor_name] = wl_cursor;
  }

  supported_wl_cursor_list_.insert(std::make_pair(size, cursor_list));

  return true;
}

wl_cursor* ELinuxWindowWayland::GetWlCursor(const std::string& cursor_name,
                                            uint32_t size) {
  // Convert the cursor theme name from Flutter's cursor value to Wayland's one.
  // However, Wayland has not all cursor themes corresponding to Flutter.
  // If there is no Wayland's cursor theme corresponding to the Flutter's cursor
  // name, it is defined as empty.
  // If empty, the default cursor theme(left_ptr) will be displayed.
  static const std::unordered_map<std::string, std::string>
      flutter_to_wayland_cursor_map = {
          {"alias", ""},
          {"allScroll", ""},
          {"basic", kWlCursorThemeLeftPtr},
          {"cell", ""},
          {"click", kWlCursorThemeHand1},
          {"contextMenu", ""},
          {"copy", ""},
          {"forbidden", ""},
          {"grab", ""},
          {"grabbing", kWlCursorThemeGrabbing},
          {"help", ""},
          {"move", ""},
          {"noDrop", ""},
          {"precise", ""},
          {"progress", ""},
          {"text", kWlCursorThemeXterm},
          {"resizeColumn", ""},
          {"resizeDown", kWlCursorThemeBottomSide},
          {"resizeDownLeft", kWlCursorThemeBottomLeftCorner},
          {"resizeDownRight", kWlCursorThemeBottomRightCorner},
          {"resizeLeft", kWlCursorThemeLeftSide},
          {"resizeLeftRight", ""},
          {"resizeRight", kWlCursorThemeRightSide},
          {"resizeRow", ""},
          {"resizeUp", kWlCursorThemeTopSide},
          {"resizeUpDown", ""},
          {"resizeUpLeft", kWlCursorThemeTopLeftCorner},
          {"resizeUpRight", kWlCursorThemeTopRightCorner},
          {"resizeUpLeftDownRight", ""},
          {"resizeUpRightDownLeft", ""},
          {"verticalText", ""},
          {"wait", kWlCursorThemeWatch},
          {"zoomIn", ""},
          {"zoomOut", ""},
      };

  if (supported_wl_cursor_list_.find(size) == supported_wl_cursor_list_.end()) {
    if (!LoadCursorTheme(size)) {
      return nullptr;
    }
  }

  auto cursor_list = supported_wl_cursor_list_.at(size);

  if (flutter_to_wayland_cursor_map.find(cursor_name) !=
      flutter_to_wayland_cursor_map.end()) {
    auto theme = flutter_to_wayland_cursor_map.at(cursor_name);
    if (!theme.empty() && cursor_list.find(theme) != cursor_list.end()) {
      return cursor_list[theme];
    }
  }

  ELINUX_LOG(ERROR) << "Unsupported cursor: " << cursor_name.c_str();
  return cursor_list[kWlCursorThemeLeftPtr];
}

void ELinuxWindowWayland::ShowVirtualKeyboard() {
  if (zwp_text_input_v3_) {
    // I'm not sure the reason, but enable needs to be called twice.
    zwp_text_input_v3_enable(zwp_text_input_v3_);
    zwp_text_input_v3_commit(zwp_text_input_v3_);
    zwp_text_input_v3_enable(zwp_text_input_v3_);
    zwp_text_input_v3_commit(zwp_text_input_v3_);

    // Map Flutter input types to Wayland content purposes.
    uint32_t hint = ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE;
    uint32_t purpose = ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL;

    if (text_input_type_ == "TextInputType.number") {
      purpose = ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NUMBER;
    } else if (text_input_type_ == "TextInputType.phone") {
      purpose = ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PHONE;
    } else if (text_input_type_ == "TextInputType.emailAddress") {
      purpose = ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_EMAIL;
    } else if (text_input_type_ == "TextInputType.url") {
      purpose = ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_URL;
    } else if (text_input_type_ == "TextInputType.visiblePassword") {
      purpose = ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PASSWORD;
    } else if (text_input_type_ == "TextInputType.multiline") {
      hint |= ZWP_TEXT_INPUT_V3_CONTENT_HINT_MULTILINE;
    }

    zwp_text_input_v3_set_content_type(zwp_text_input_v3_, hint,
                                       purpose);  // Untested code path
    zwp_text_input_v3_commit(zwp_text_input_v3_);
  } else {
    if (native_window_) {
      // Map Flutter input types to Wayland v1 content purposes.
      uint32_t hint = ZWP_TEXT_INPUT_V1_CONTENT_HINT_NONE;
      uint32_t purpose = ZWP_TEXT_INPUT_V1_CONTENT_PURPOSE_NORMAL;

      if (text_input_type_ == "TextInputType.number") {
        purpose = ZWP_TEXT_INPUT_V1_CONTENT_PURPOSE_NUMBER;
      } else if (text_input_type_ == "TextInputType.phone") {
        purpose = ZWP_TEXT_INPUT_V1_CONTENT_PURPOSE_PHONE;
      } else if (text_input_type_ == "TextInputType.emailAddress") {
        purpose = ZWP_TEXT_INPUT_V1_CONTENT_PURPOSE_EMAIL;
      } else if (text_input_type_ == "TextInputType.url") {
        purpose = ZWP_TEXT_INPUT_V1_CONTENT_PURPOSE_URL;
      } else if (text_input_type_ == "TextInputType.visiblePassword") {
        purpose = ZWP_TEXT_INPUT_V1_CONTENT_PURPOSE_PASSWORD;
      } else if (text_input_type_ == "TextInputType.multiline") {
        hint |= ZWP_TEXT_INPUT_V1_CONTENT_HINT_MULTILINE;
      }

      zwp_text_input_v1_activate(zwp_text_input_v1_,
                                 seat_inputs_map_.begin()->first,
                                 native_window_->Surface());
      zwp_text_input_v1_set_content_type(zwp_text_input_v1_, hint, purpose);
      zwp_text_input_v1_commit_state(zwp_text_input_v1_, ++text_input_serial_);
      zwp_text_input_v1_show_input_panel(zwp_text_input_v1_);
    }
  }
}

void ELinuxWindowWayland::DismissVirtualKeybaord() {
  if (zwp_text_input_v3_) {
    zwp_text_input_v3_disable(zwp_text_input_v3_);
    zwp_text_input_v3_commit(zwp_text_input_v3_);
  } else {
    zwp_text_input_v1_deactivate(zwp_text_input_v1_,
                                 seat_inputs_map_.begin()->first);
  }
}

bool ELinuxWindowWayland::IsCoverConfigure(int32_t width,
                                           int32_t height) const {
  if (view_properties_.view_mode != FlutterDesktopViewMode::kFullscreen) {
    return false;
  }
  if (display_max_width_ <= 0 || display_max_height_ <= 0 || width <= 0 ||
      height <= 0) {
    return false;
  }
  const int32_t conf_long = std::max(width, height);
  const int32_t disp_long = std::max(display_max_width_, display_max_height_);
  return conf_long * 5 < disp_long * 4;
}

void ELinuxWindowWayland::SetShownAsCover(bool shown_as_cover) {
  if (shown_as_cover == shown_as_cover_) {
    return;
  }
  shown_as_cover_ = shown_as_cover;
  MaybeApplyOutputTransform();
}

void ELinuxWindowWayland::MaybeApplyOutputTransform() {
  // While shown as the app's cover, keep the current orientation: Lipstick
  // shows the window upright in the cover tile as it is, and would animate
  // a turn there.
  int32_t target = shown_as_cover_ ? applied_transform_ : transform_;
  // Respect the app's preferred orientations: stay put when the new one isn't
  // allowed, or move to the most preferred one if the current one isn't.
  if (!IsTransformAllowed(target)) {
    target = IsTransformAllowed(applied_transform_)
                 ? applied_transform_
                 : preferred_transforms_.front();
  }
  if (!FollowsOutputTransform() || target == applied_transform_) {
    return;
  }

  // The buffer keeps the panel's native orientation and the content is drawn
  // pre-rotated into it. On a portrait panel Lipstick sends 270 for
  // Landscape and 90 for InvertedLandscape.
  uint16_t degree = 0;
  switch (target) {
    case WL_OUTPUT_TRANSFORM_NORMAL:
      degree = 0;
      break;
    case WL_OUTPUT_TRANSFORM_90:
      degree = 270;
      break;
    case WL_OUTPUT_TRANSFORM_180:
      degree = 180;
      break;
    case WL_OUTPUT_TRANSFORM_270:
      degree = 90;
      break;
    default:
      ELINUX_LOG(WARNING) << "Ignoring output transform " << target;
      return;
  }

  ELINUX_LOG(INFO) << "Output transform " << target << ": rotating by "
                   << degree << (shown_as_cover_ ? " (cover)" : "");
  const bool was_sideways = current_rotation_ == 90 || current_rotation_ == 270;
  const bool is_sideways = degree == 90 || degree == 270;
  applied_transform_ = target;
  current_rotation_ = degree;

  // Tells Lipstick which orientation the content is drawn in, so its edge
  // gestures follow.
  if (native_window_ && use_wl_shell_) {
    wl_surface_set_buffer_transform(native_window_->Surface(), target);
  }

  // The buffer keeps the panel's native orientation; only the logical size
  // turns.
  if (was_sideways != is_sideways) {
    std::swap(view_properties_.width, view_properties_.height);
  }
  request_redraw_ = true;

  if (binding_handler_delegate_) {
    binding_handler_delegate_->OnContentOrientationChanged();
  }
}

bool ELinuxWindowWayland::FollowsOutputTransform() const {
  // Only the wl_shell path can tell Lipstick the content orientation. The
  // globals are bound before any wl_output event arrives, so this also holds
  // for the first transform during start-up.
  return follow_output_transform_ && wl_shell_ && qt_surface_extension_ &&
         view_properties_.view_mode == FlutterDesktopViewMode::kFullscreen;
}

int32_t ELinuxWindowWayland::GetContentOrientationAngle() const {
  switch (applied_transform_) {
    case WL_OUTPUT_TRANSFORM_90:
      return 270;
    case WL_OUTPUT_TRANSFORM_180:
      return 180;
    case WL_OUTPUT_TRANSFORM_270:
      return 90;
    default:
      return 0;
  }
}

void ELinuxWindowWayland::SetPreferredOrientations(
    const std::vector<DeviceOrientation>& orientations) {
  // In wl_output transform terms, as Lipstick sends them for a portrait panel.
  preferred_transforms_.clear();
  for (const auto orientation : orientations) {
    switch (orientation) {
      case DeviceOrientation::kPortraitUp:
        preferred_transforms_.push_back(WL_OUTPUT_TRANSFORM_NORMAL);
        break;
      case DeviceOrientation::kLandscapeLeft:
        preferred_transforms_.push_back(WL_OUTPUT_TRANSFORM_270);
        break;
      case DeviceOrientation::kPortraitDown:
        preferred_transforms_.push_back(WL_OUTPUT_TRANSFORM_180);
        break;
      case DeviceOrientation::kLandscapeRight:
        preferred_transforms_.push_back(WL_OUTPUT_TRANSFORM_90);
        break;
    }
  }
  UpdateContentOrientationMask();
  MaybeApplyOutputTransform();
}

DisplayCutouts ELinuxWindowWayland::GetDisplayCutouts() const {
  DisplayCutouts cutouts;
  // Only fullscreen wl_shell windows cover the whole panel. Lipstick keeps
  // xdg_shell windows clear of the cut-outs itself.
  if (!use_wl_shell_) {
    return cutouts;
  }
  const int32_t view_width = GetCurrentWidth();
  const int32_t view_height = GetCurrentHeight();
  const bool sideways = current_rotation_ == 90 || current_rotation_ == 270;
  const int32_t native_width = sideways ? view_height : view_width;
  const int32_t native_height = sideways ? view_width : view_height;
  for (const auto& c : display_cutouts_) {
    // Native panel to view coordinates, as for touch input.
    PhysicalRect r = c;
    switch (current_rotation_) {
      case 90:
        r = {c.top, native_width - c.left - c.width, c.height, c.width};
        break;
      case 180:
        r = {native_width - c.left - c.width, native_height - c.top - c.height,
             c.width, c.height};
        break;
      case 270:
        r = {native_height - c.top - c.height, c.left, c.height, c.width};
        break;
      default:
        break;
    }
    cutouts.rects.push_back(r);

    // Pad the view edge the cut-out is nearest to, up to its far side.
    const int32_t to_top = r.top;
    const int32_t to_right = view_width - r.left - r.width;
    const int32_t to_bottom = view_height - r.top - r.height;
    const int32_t to_left = r.left;
    const int32_t nearest = std::min({to_top, to_right, to_bottom, to_left});
    auto& padding = cutouts.padding;
    if (nearest == to_top) {
      padding.top = std::max<double>(padding.top, r.top + r.height);
    } else if (nearest == to_bottom) {
      padding.bottom = std::max<double>(padding.bottom, view_height - r.top);
    } else if (nearest == to_left) {
      padding.left = std::max<double>(padding.left, r.left + r.width);
    } else {
      padding.right = std::max<double>(padding.right, view_width - r.left);
    }
  }
  return cutouts;
}

CoverState ELinuxWindowWayland::GetCoverState() const {
  CoverState state;
  // Cover windows need the Qt surface extension, which only the wl_shell
  // path uses.
  state.available = use_wl_shell_ && wl_shm_ && qt_extended_surface_;
  if (!state.available) {
    return state;
  }
  if (cover_window_) {
    state.visible = cover_window_->visible();
    state.width = cover_window_->width();
    state.height = cover_window_->height();
  } else {
    state.width = CoverWindowWayland::kDefaultWidth;
    state.height = CoverWindowWayland::kDefaultHeight;
  }
  return state;
}

void ELinuxWindowWayland::SetCoverFrame(int32_t width,
                                        int32_t height,
                                        const uint8_t* rgba) {
  if (!cover_window_) {
    if (!GetCoverState().available) {
      return;
    }
    cover_window_ = std::make_unique<CoverWindowWayland>(
        wl_compositor_, wl_shm_, wl_shell_, qt_surface_extension_,
        native_window_->Surface(), qt_extended_surface_,
        view_properties_.app_id, [this]() {
          if (binding_handler_delegate_) {
            binding_handler_delegate_->OnCoverChanged();
          }
        });
    if (!cover_window_->IsValid()) {
      cover_window_ = nullptr;
      return;
    }
  }
  cover_window_->SetFrame(width, height, rgba);
}

void ELinuxWindowWayland::SetCoverActions(const std::string& actions) {
  cover_actions_ = actions;
  if (qt_extended_surface_) {
    SetStringWindowProperty(qt_extended_surface_, "_APP_COVER_ACTION", actions);
  }
}

bool ELinuxWindowWayland::IsTransformAllowed(int32_t transform) const {
  return preferred_transforms_.empty() ||
         std::find(preferred_transforms_.begin(), preferred_transforms_.end(),
                   transform) != preferred_transforms_.end();
}

void ELinuxWindowWayland::UpdateContentOrientationMask() {
  if (!qt_extended_surface_) {
    return;
  }
  int32_t mask = 0;
  if (IsTransformAllowed(WL_OUTPUT_TRANSFORM_NORMAL)) {
    mask |= QT_EXTENDED_SURFACE_ORIENTATION_PORTRAITORIENTATION;
  }
  if (IsTransformAllowed(WL_OUTPUT_TRANSFORM_270)) {
    mask |= QT_EXTENDED_SURFACE_ORIENTATION_LANDSCAPEORIENTATION;
  }
  if (IsTransformAllowed(WL_OUTPUT_TRANSFORM_180)) {
    mask |= QT_EXTENDED_SURFACE_ORIENTATION_INVERTEDPORTRAITORIENTATION;
  }
  if (IsTransformAllowed(WL_OUTPUT_TRANSFORM_90)) {
    mask |= QT_EXTENDED_SURFACE_ORIENTATION_INVERTEDLANDSCAPEORIENTATION;
  }
  qt_extended_surface_set_content_orientation_mask(qt_extended_surface_, mask);
}

void ELinuxWindowWayland::MaybeUpdateDevicePixelRatio() {
  if (device_pixel_ratio_locked_ || output_physical_width_mm_ <= 0 ||
      output_mode_width_ <= 0) {
    return;
  }
  const double inches =
      static_cast<double>(output_physical_width_mm_) / 25.4;
  const double dpr = SnapDevicePixelRatio(
      static_cast<double>(output_mode_width_) / inches);
  if (dpr <= 0.0 || dpr == device_pixel_ratio_) {
    return;
  }
  device_pixel_ratio_ = dpr;
  request_redraw_ = true;
}

void ELinuxWindowWayland::UpdateWindowScale() {
  double scale_factor = 1.0;
  for (auto output_id : entered_outputs_) {
    if (wl_output_scale_factors_.find(output_id) ==
        wl_output_scale_factors_.end())
      continue;

    auto output_scale_factor = wl_output_scale_factors_.at(output_id);
    if (output_scale_factor > scale_factor)
      scale_factor = output_scale_factor;
  }

  if (current_scale_ == scale_factor) {
    return;
  }

  ELINUX_LOG(TRACE) << "Window scale has changed: " << scale_factor;
  current_scale_ = scale_factor;

  wl_surface_set_buffer_scale(native_window_->Surface(), current_scale_);
  request_redraw_ = true;
}

uint32_t ELinuxWindowWayland::WindowDecorationsPhysicalHeight() const {
  if (!window_decorations_) {
    return 0;
  }

  return window_decorations_->Height() * current_scale_;
}

}  // namespace flutter
