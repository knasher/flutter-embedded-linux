// Copyright 2023 Sony Corporation. All rights reserved.
// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_PLUGINS_DISPLAY_CUTOUT_PLUGIN_H_
#define FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_PLUGINS_DISPLAY_CUTOUT_PLUGIN_H_

#include <rapidjson/document.h>

#include <memory>

#include "flutter/shell/platform/common/client_wrapper/include/flutter/basic_message_channel.h"
#include "flutter/shell/platform/common/client_wrapper/include/flutter/binary_messenger.h"
#include "flutter/shell/platform/linux_embedded/window_binding_handler.h"

namespace flutter {

// Tells Flutter where the display cut-outs are.
//
// The embedder API has no view padding, so this sends the cut-outs and the
// padding that avoids them (physical pixels, current orientation) for the app
// to apply itself, e.g. by wrapping MediaQuery.
class DisplayCutoutPlugin {
 public:
  explicit DisplayCutoutPlugin(BinaryMessenger* messenger,
                               WindowBindingHandler* delegate);
  ~DisplayCutoutPlugin() = default;

  // Sends the cut-outs for the current orientation to the engine.
  void SendDisplayCutouts();

 private:
  std::unique_ptr<BasicMessageChannel<rapidjson::Document>> channel_;
  WindowBindingHandler* delegate_;
  // The last message sent, so repeats can be skipped.
  rapidjson::Document last_message_;
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_PLUGINS_DISPLAY_CUTOUT_PLUGIN_H_
