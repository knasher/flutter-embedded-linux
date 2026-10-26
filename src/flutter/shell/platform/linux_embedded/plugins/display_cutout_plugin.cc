// Copyright 2023 Sony Corporation. All rights reserved.
// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux_embedded/plugins/display_cutout_plugin.h"

#include "flutter/shell/platform/common/json_message_codec.h"
#include "flutter/shell/platform/linux_embedded/logger.h"

namespace flutter {

namespace {
constexpr char kChannelName[] = "flutter-sfos/display_cutouts";

// Bumped on incompatible changes to the message.
constexpr int kMessageVersion = 1;

constexpr char kVersion[] = "version";
constexpr char kPadding[] = "padding";
constexpr char kCutouts[] = "cutouts";
constexpr char kTop[] = "top";
constexpr char kRight[] = "right";
constexpr char kBottom[] = "bottom";
constexpr char kLeft[] = "left";
constexpr char kWidth[] = "width";
constexpr char kHeight[] = "height";
}  // namespace

DisplayCutoutPlugin::DisplayCutoutPlugin(BinaryMessenger* messenger,
                                         WindowBindingHandler* delegate)
    : channel_(std::make_unique<BasicMessageChannel<rapidjson::Document>>(
          messenger,
          kChannelName,
          &JsonMessageCodec::GetInstance())),
      delegate_(delegate) {}

void DisplayCutoutPlugin::SendDisplayCutouts() {
  const DisplayCutouts cutouts = delegate_->GetDisplayCutouts();

  rapidjson::Document message(rapidjson::kObjectType);
  auto& allocator = message.GetAllocator();
  message.AddMember(kVersion, kMessageVersion, allocator);

  rapidjson::Value padding(rapidjson::kObjectType);
  padding.AddMember(kTop, cutouts.padding.top, allocator);
  padding.AddMember(kRight, cutouts.padding.right, allocator);
  padding.AddMember(kBottom, cutouts.padding.bottom, allocator);
  padding.AddMember(kLeft, cutouts.padding.left, allocator);
  message.AddMember(kPadding, padding, allocator);

  rapidjson::Value rects(rapidjson::kArrayType);
  for (const auto& r : cutouts.rects) {
    rapidjson::Value rect(rapidjson::kObjectType);
    rect.AddMember(kLeft, r.left, allocator);
    rect.AddMember(kTop, r.top, allocator);
    rect.AddMember(kWidth, r.width, allocator);
    rect.AddMember(kHeight, r.height, allocator);
    rects.PushBack(rect, allocator);
  }
  message.AddMember(kCutouts, rects, allocator);

  // A rotation and the resize that follows it both ask; send each state once.
  if (message == last_message_) {
    return;
  }
  last_message_.CopyFrom(message, last_message_.GetAllocator());

  ELINUX_LOG(INFO) << "Display cut-out padding: top " << cutouts.padding.top
                   << ", right " << cutouts.padding.right << ", bottom "
                   << cutouts.padding.bottom << ", left "
                   << cutouts.padding.left;
  channel_->Send(message);
}

}  // namespace flutter
