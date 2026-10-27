// Copyright 2026 Dave Nash. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux_embedded/plugins/open_url_plugin.h"

#include "flutter/shell/platform/common/json_method_codec.h"

// X11's Success macro breaks result->Success (see platform_plugin.cc).
#if defined(DISPLAY_BACKEND_TYPE_X11)
#undef Success
#endif

namespace flutter {

namespace {
constexpr char kChannelName[] = "flutter-sfos/open_url";

constexpr char kOpenUrlMethod[] = "openUrl";

constexpr char kBadArgumentsError[] = "Bad Arguments";
}  // namespace

OpenUrlPlugin::OpenUrlPlugin(BinaryMessenger* messenger,
                             WindowBindingHandler* delegate)
    : channel_(std::make_unique<MethodChannel<rapidjson::Document>>(
          messenger,
          kChannelName,
          &JsonMethodCodec::GetInstance())),
      delegate_(delegate) {
  channel_->SetMethodCallHandler(
      [this](const MethodCall<rapidjson::Document>& call,
             std::unique_ptr<MethodResult<rapidjson::Document>> result) {
        HandleMethodCall(call, std::move(result));
      });
}

void OpenUrlPlugin::HandleMethodCall(
    const MethodCall<rapidjson::Document>& method_call,
    std::unique_ptr<MethodResult<rapidjson::Document>> result) {
  if (method_call.method_name() != kOpenUrlMethod) {
    result->NotImplemented();
    return;
  }
  const rapidjson::Document* url = method_call.arguments();
  if (!url || !url->IsString()) {
    result->Error(kBadArgumentsError, "openUrl expects the URL as a string.");
    return;
  }
  rapidjson::Document opened;
  opened.SetBool(delegate_->OpenUrl(
      std::string(url->GetString(), url->GetStringLength())));
  result->Success(opened);
}

}  // namespace flutter
