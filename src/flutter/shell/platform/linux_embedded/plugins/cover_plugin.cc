// Copyright 2026 Dave Nash. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux_embedded/plugins/cover_plugin.h"

#include <errno.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>

#include "flutter/shell/platform/common/json_message_codec.h"
#include "flutter/shell/platform/linux_embedded/logger.h"

namespace flutter {

namespace {
constexpr char kChannelName[] = "flutter-sfos/cover";
constexpr char kFrameChannelName[] = "flutter-sfos/cover_frame";

// Bumped on incompatible changes to the messages.
constexpr int kMessageVersion = 1;

constexpr char kVersion[] = "version";
constexpr char kAvailable[] = "available";
constexpr char kVisible[] = "visible";
constexpr char kWidth[] = "width";
constexpr char kHeight[] = "height";
constexpr char kActions[] = "actions";
constexpr char kAction[] = "action";
constexpr char kId[] = "id";
constexpr char kIcon[] = "icon";

// The home screen calls this method, with the button's id, on our service.
constexpr char kTriggerMethod[] = "trigger";

// Theme icon names (e.g. "icon-cover-refresh") get this prefix.
constexpr char kThemeIconPrefix[] = "image://theme/";

// Pictures are sent at the cover's size, so anything far bigger is a mistake.
constexpr uint32_t kMaxFrameSide = 4096;

uint32_t ReadU32(const uint8_t* bytes) {
  return bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) |
         (static_cast<uint32_t>(bytes[3]) << 24);
}

// org.freedesktop.DBus RequestName flag and reply.
constexpr guint32 kDoNotQueue = 4;
constexpr guint32 kPrimaryOwner = 1;

// A D-Bus name element: letters, digits and underscores.
std::string NameElement(const char* name) {
  std::string out = name ? name : "app";
  for (auto& c : out) {
    if (!std::isalnum(static_cast<unsigned char>(c))) {
      c = '_';
    }
  }
  if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0]))) {
    out.insert(0, "_");
  }
  return out;
}

// The action list is newline-separated text in an ASCII window property.
bool IsValidIcon(const std::string& icon) {
  return !icon.empty() && std::all_of(icon.begin(), icon.end(), [](char c) {
    return c > ' ' && c < 0x7f;
  });
}
}  // namespace

CoverPlugin::CoverPlugin(BinaryMessenger* messenger,
                         WindowBindingHandler* delegate)
    : messenger_(messenger),
      channel_(std::make_unique<BasicMessageChannel<rapidjson::Document>>(
          messenger,
          kChannelName,
          &JsonMessageCodec::GetInstance())),
      delegate_(delegate) {
  channel_->SetMessageHandler(
      [this](const rapidjson::Document& message,
             const MessageReply<rapidjson::Document>& reply) {
        HandleMessage(message);
        reply(rapidjson::Document());
      });
  messenger_->SetMessageHandler(
      kFrameChannelName,
      [this](const uint8_t* message, size_t size, BinaryReply reply) {
        HandleFrame(message, size);
        if (reply) {
          reply(nullptr, 0);
        }
      });
}

CoverPlugin::~CoverPlugin() {
  messenger_->SetMessageHandler(kFrameChannelName, nullptr);
  if (bus_) {
    if (filter_id_) {
      g_dbus_connection_remove_filter(bus_, filter_id_);
    }
    g_object_unref(bus_);
  }
}

void CoverPlugin::SendCoverState() {
  const CoverState state = delegate_->GetCoverState();
  rapidjson::Document message(rapidjson::kObjectType);
  auto& allocator = message.GetAllocator();
  message.AddMember(kVersion, kMessageVersion, allocator);
  message.AddMember(kAvailable, state.available, allocator);
  message.AddMember(kVisible, state.visible, allocator);
  message.AddMember(kWidth, state.width, allocator);
  message.AddMember(kHeight, state.height, allocator);
  ELINUX_LOG(INFO) << "Cover: " << (state.visible ? "shown" : "hidden") << ", "
                   << state.width << "x" << state.height;
  channel_->Send(message);
}

void CoverPlugin::HandleFrame(const uint8_t* message, size_t size) {
  if (!message || size < 8) {
    ELINUX_LOG(ERROR) << "Cover: picture too short";
    return;
  }
  const uint32_t width = ReadU32(message);
  const uint32_t height = ReadU32(message + 4);
  if (width == 0 || height == 0 || width > kMaxFrameSide ||
      height > kMaxFrameSide ||
      size != 8 + static_cast<size_t>(width) * height * 4) {
    ELINUX_LOG(ERROR) << "Cover: picture size doesn't match";
    return;
  }
  delegate_->SetCoverFrame(static_cast<int32_t>(width),
                           static_cast<int32_t>(height), message + 8);
}

void CoverPlugin::DispatchEvent() {
  std::vector<int32_t> actions;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    actions.swap(pending_actions_);
  }
  for (const int32_t id : actions) {
    rapidjson::Document message(rapidjson::kObjectType);
    auto& allocator = message.GetAllocator();
    message.AddMember(kVersion, kMessageVersion, allocator);
    message.AddMember(kAction, id, allocator);
    ELINUX_LOG(INFO) << "Cover action: " << id;
    channel_->Send(message);
  }
}

void CoverPlugin::HandleMessage(const rapidjson::Document& message) {
  if (!message.IsObject() || !message.HasMember(kActions) ||
      !message[kActions].IsArray()) {
    ELINUX_LOG(ERROR) << "Cover: unexpected message";
    return;
  }

  // Each button is "<service>\n<method>\n<id>\n<icon>", joined by newlines,
  // then a trailing "\n0" (as other apps send it).
  std::string actions;
  for (const auto& action : message[kActions].GetArray()) {
    if (!action.IsObject() || !action.HasMember(kId) ||
        !action[kId].IsInt() || action[kId].GetInt() < 0 ||
        !action.HasMember(kIcon) || !action[kIcon].IsString()) {
      ELINUX_LOG(ERROR) << "Cover: each action needs an id and an icon";
      return;
    }
    std::string icon = action[kIcon].GetString();
    if (!IsValidIcon(icon)) {
      ELINUX_LOG(ERROR) << "Cover: invalid icon name";
      return;
    }
    if (icon.find("://") == std::string::npos && icon[0] != '/') {
      icon.insert(0, kThemeIconPrefix);
    }
    if (service_.empty() && !RegisterActionService()) {
      return;
    }
    if (!actions.empty()) {
      actions += "\n";
    }
    actions += service_ + "\n" + kTriggerMethod + "\n" +
               std::to_string(action[kId].GetInt()) + "\n" + icon;
  }
  if (!actions.empty()) {
    actions += "\n0";
  }
  delegate_->SetCoverActions(actions);
}

bool CoverPlugin::RegisterActionService() {
  GError* error = nullptr;
  bus_ = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
  if (!bus_) {
    ELINUX_LOG(ERROR) << "Cover actions: no session bus: " << error->message;
    g_clear_error(&error);
    return false;
  }

  // The pattern other Sailfish apps use for their cover action service.
  const std::string service = std::string("org.sailfishos.coveraction.") +
                              NameElement(program_invocation_short_name) +
                              ".pid" + std::to_string(getpid()) + ".id1";
  GVariant* reply = g_dbus_connection_call_sync(
      bus_, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "RequestName",
      g_variant_new("(su)", service.c_str(), kDoNotQueue),
      G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
  if (!reply) {
    ELINUX_LOG(ERROR) << "Cover actions: RequestName failed: "
                      << error->message;
    g_clear_error(&error);
    return false;
  }
  guint32 result = 0;
  g_variant_get(reply, "(u)", &result);
  g_variant_unref(reply);
  if (result != kPrimaryOwner) {
    ELINUX_LOG(ERROR) << "Cover actions: could not own " << service;
    return false;
  }

  service_ = service;
  path_ = "/" + service_;
  std::replace(path_.begin(), path_.end(), '.', '/');
  // GDBus only routes calls that name an interface to registered objects,
  // and the home screen's call may not, so catch it with a filter instead.
  filter_id_ = g_dbus_connection_add_filter(bus_, FilterMessage, this, nullptr);
  ELINUX_LOG(INFO) << "Cover actions: " << service_;
  return true;
}

GDBusMessage* CoverPlugin::FilterMessage(GDBusConnection* connection,
                                         GDBusMessage* message,
                                         gboolean incoming,
                                         gpointer user_data) {
  auto self = static_cast<CoverPlugin*>(user_data);
  if (!incoming ||
      g_dbus_message_get_message_type(message) !=
          G_DBUS_MESSAGE_TYPE_METHOD_CALL ||
      g_strcmp0(g_dbus_message_get_path(message), self->path_.c_str()) != 0) {
    return message;
  }

  GDBusMessage* reply;
  if (g_strcmp0(g_dbus_message_get_member(message), kTriggerMethod) == 0) {
    int32_t id = -1;
    GVariant* body = g_dbus_message_get_body(message);
    if (body && g_variant_is_of_type(body, G_VARIANT_TYPE("(i)"))) {
      g_variant_get(body, "(i)", &id);
    }
    {
      std::lock_guard<std::mutex> lock(self->mutex_);
      self->pending_actions_.push_back(id);
    }
    reply = g_dbus_message_new_method_reply(message);
  } else {
    reply = g_dbus_message_new_method_error(
        message, "org.freedesktop.DBus.Error.UnknownMethod", "Unknown method");
  }
  g_dbus_connection_send_message(connection, reply,
                                 G_DBUS_SEND_MESSAGE_FLAGS_NONE, nullptr,
                                 nullptr);
  g_object_unref(reply);
  // Handled here: returning null drops the message, which we own.
  g_object_unref(message);
  return nullptr;
}

}  // namespace flutter
