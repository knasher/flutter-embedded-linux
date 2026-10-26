// Copyright 2026 Dave Nash. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_PLUGINS_COVER_PLUGIN_H_
#define FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_PLUGINS_COVER_PLUGIN_H_

#include <gio/gio.h>
#include <rapidjson/document.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "flutter/shell/platform/common/client_wrapper/include/flutter/basic_message_channel.h"
#include "flutter/shell/platform/common/client_wrapper/include/flutter/binary_messenger.h"
#include "flutter/shell/platform/linux_embedded/window_binding_handler.h"

namespace flutter {

// The app's cover on Sailfish OS.
//
// On flutter-sfos/cover (JSON) the app is told whether it can have a cover,
// whether Lipstick shows it and its size. It sends the pictures to show on
// flutter-sfos/cover_frame; the first one creates the cover window, which
// Lipstick then shows in the app's cover tile instead of the main window.
//
// The app can also give the cover buttons, which the home screen draws from
// the _APP_COVER_ACTION window property. That names a D-Bus service of ours
// and the method the home screen calls when a button is tapped; the taps are
// passed on to the app.
class CoverPlugin {
 public:
  CoverPlugin(BinaryMessenger* messenger, WindowBindingHandler* delegate);
  ~CoverPlugin();

  void SendCoverState();

  // Sends the cover button taps received since the last call. Must be called
  // on the platform thread.
  void DispatchEvent();

 private:
  void HandleMessage(const rapidjson::Document& message);

  // A picture: width and height (uint32, little-endian), then RGBA pixels.
  void HandleFrame(const uint8_t* message, size_t size);

  // Owns a D-Bus name for the home screen to call; returns false on failure.
  bool RegisterActionService();

  // Catches the home screen's trigger calls. Runs on GDBus' worker thread.
  static GDBusMessage* FilterMessage(GDBusConnection* connection,
                                     GDBusMessage* message,
                                     gboolean incoming,
                                     gpointer user_data);

  BinaryMessenger* messenger_;
  std::unique_ptr<BasicMessageChannel<rapidjson::Document>> channel_;
  WindowBindingHandler* delegate_;
  GDBusConnection* bus_ = nullptr;
  guint filter_id_ = 0;
  std::string service_;
  std::string path_;
  std::mutex mutex_;
  std::vector<int32_t> pending_actions_;
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_PLUGINS_COVER_PLUGIN_H_
