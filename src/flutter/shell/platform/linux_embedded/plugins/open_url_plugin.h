// Copyright 2026 Dave Nash. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_PLUGINS_OPEN_URL_PLUGIN_H_
#define FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_PLUGINS_OPEN_URL_PLUGIN_H_

#include <rapidjson/document.h>

#include <memory>

#include "flutter/shell/platform/common/client_wrapper/include/flutter/binary_messenger.h"
#include "flutter/shell/platform/common/client_wrapper/include/flutter/method_channel.h"
#include "flutter/shell/platform/linux_embedded/window_binding_handler.h"

namespace flutter {

// Opens URLs in the apps that handle them on Sailfish OS.
//
// On flutter-sfos/open_url (JSON method channel), openUrl with the URL as its
// argument asks the compositor to open it, as Qt apps do; Sailjail's sandbox
// leaves apps no other way. The result is false if the compositor can't be
// asked. Whether an app handles the URL is up to the caller to check.
class OpenUrlPlugin {
 public:
  OpenUrlPlugin(BinaryMessenger* messenger, WindowBindingHandler* delegate);
  ~OpenUrlPlugin() = default;

 private:
  void HandleMethodCall(
      const MethodCall<rapidjson::Document>& method_call,
      std::unique_ptr<MethodResult<rapidjson::Document>> result);

  std::unique_ptr<MethodChannel<rapidjson::Document>> channel_;
  WindowBindingHandler* delegate_;
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_PLUGINS_OPEN_URL_PLUGIN_H_
