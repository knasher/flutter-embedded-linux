// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux_embedded/plugins/platform_plugin.h"

#include "flutter/shell/platform/common/json_method_codec.h"

// Avoids the following build error:
// ----------------------------------------------------------------
//  error: expected unqualified-id
//    result->Success(document);
//            ^
// /usr/include/X11/X.h:350:21: note: expanded from macro 'Success'
// #define Success            0    /* everything's okay */
// ----------------------------------------------------------------
#if defined(DISPLAY_BACKEND_TYPE_X11)
#undef Success
#endif

namespace flutter {

namespace {
constexpr char kChannelName[] = "flutter/platform";

constexpr char kGetClipboardDataMethod[] = "Clipboard.getData";
constexpr char kSetClipboardDataMethod[] = "Clipboard.setData";
constexpr char kSystemNavigatorPopMethod[] = "SystemNavigator.pop";
constexpr char kSetPreferredOrientationsMethod[] =
    "SystemChrome.setPreferredOrientations";

constexpr char kTextPlainFormat[] = "text/plain";
constexpr char kTextKey[] = "text";

constexpr char kUnknownClipboardFormatError[] =
    "Unknown clipboard format error";

bool ParseDeviceOrientation(const std::string& name,
                            DeviceOrientation* orientation) {
  if (name == "DeviceOrientation.portraitUp") {
    *orientation = DeviceOrientation::kPortraitUp;
  } else if (name == "DeviceOrientation.landscapeLeft") {
    *orientation = DeviceOrientation::kLandscapeLeft;
  } else if (name == "DeviceOrientation.portraitDown") {
    *orientation = DeviceOrientation::kPortraitDown;
  } else if (name == "DeviceOrientation.landscapeRight") {
    *orientation = DeviceOrientation::kLandscapeRight;
  } else {
    return false;
  }
  return true;
}
}  // namespace

PlatformPlugin::PlatformPlugin(BinaryMessenger* messenger,
                               WindowBindingHandler* delegate)
    : channel_(std::make_unique<MethodChannel<rapidjson::Document>>(
          messenger,
          kChannelName,
          &flutter::JsonMethodCodec::GetInstance())),
      delegate_(delegate) {
  channel_->SetMethodCallHandler(
      [this](
          const flutter::MethodCall<rapidjson::Document>& call,
          std::unique_ptr<flutter::MethodResult<rapidjson::Document>> result) {
        HandleMethodCall(call, std::move(result));
      });
}

void PlatformPlugin::HandleMethodCall(
    const flutter::MethodCall<rapidjson::Document>& method_call,
    std::unique_ptr<flutter::MethodResult<rapidjson::Document>> result) {
  const std::string& method = method_call.method_name();

  if (method.compare(kGetClipboardDataMethod) == 0) {
    // Only one string argument is expected.
    const rapidjson::Value& format = method_call.arguments()[0];
    if (strcmp(format.GetString(), kTextPlainFormat) != 0) {
      result->Error(kUnknownClipboardFormatError,
                    "Clipboard API only supports text.");
      return;
    }

    auto clipboardData = delegate_->GetClipboardData();
    rapidjson::Document document;
    document.SetObject();
    rapidjson::Document::AllocatorType& allocator = document.GetAllocator();
    document.AddMember(rapidjson::Value(kTextKey, allocator),
                       rapidjson::Value(clipboardData.c_str(), allocator),
                       allocator);
    result->Success(document);
  } else if (method.compare(kSetClipboardDataMethod) == 0) {
    const rapidjson::Value& document = *method_call.arguments();
    rapidjson::Value::ConstMemberIterator itr = document.FindMember(kTextKey);
    if (itr == document.MemberEnd()) {
      result->Error(kUnknownClipboardFormatError,
                    "Missing text to store on clipboard.");
      return;
    }
    delegate_->SetClipboardData(itr->value.GetString());
    result->Success();
  } else if (method.compare(kSetPreferredOrientationsMethod) == 0) {
    // A list of DeviceOrientation names, most preferred first.
    std::vector<DeviceOrientation> orientations;
    const rapidjson::Value* args = method_call.arguments();
    if (args && args->IsArray()) {
      for (const auto& value : args->GetArray()) {
        DeviceOrientation orientation;
        if (value.IsString() &&
            ParseDeviceOrientation(value.GetString(), &orientation)) {
          orientations.push_back(orientation);
        }
      }
    }
    delegate_->SetPreferredOrientations(orientations);
    result->Success();
  } else if (method.compare(kSystemNavigatorPopMethod) == 0) {
    // todo: it is necessary to consider whether exit() is okay
    exit(EXIT_SUCCESS);
    result->Success();
  } else {
    result->NotImplemented();
  }
}

}  // namespace flutter
