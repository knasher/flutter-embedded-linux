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

// The app's cover on Sailfish OS, on the flutter-sfos/cover channel.
//
// An app without a cover window is shown on the home screen as a shrunken
// copy of its main window. The app can draw a cover layout while this is the
// case, and its normal layout again when it returns.
//
// Cover buttons are drawn by the home screen from the _APP_COVER_ACTION window
// property, which names a D-Bus service of ours and a method to call when one
// is tapped. Test version: FLUTTER_SFOS_COVER_ACTION_TEST=1 publishes one
// button, and taps are sent to the app.
class CoverPlugin {
 public:
  CoverPlugin(BinaryMessenger* messenger, WindowBindingHandler* delegate);
  ~CoverPlugin();

  void SendShownAsCover(bool shown_as_cover);

  // Sends the cover button taps received since the last call. Must be called
  // on the platform thread.
  void DispatchEvent();

 private:
  // Owns a D-Bus name for the home screen to call; returns false on failure.
  bool RegisterActionService();

  // Catches the home screen's trigger calls. Runs on GDBus' worker thread.
  static GDBusMessage* FilterMessage(GDBusConnection* connection,
                                     GDBusMessage* message,
                                     gboolean incoming,
                                     gpointer user_data);

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
