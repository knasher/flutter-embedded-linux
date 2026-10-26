// Copyright 2026 Dave Nash. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux_embedded/plugins/cover_plugin.h"

#include <errno.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

#include "flutter/shell/platform/common/json_message_codec.h"
#include "flutter/shell/platform/linux_embedded/logger.h"

namespace flutter {

namespace {
constexpr char kChannelName[] = "flutter-sfos/cover";

// Bumped on incompatible changes to the message.
constexpr int kMessageVersion = 1;

constexpr char kVersion[] = "version";
constexpr char kShownAsCover[] = "shownAsCover";
constexpr char kAction[] = "action";

// The home screen calls this method, with the button's id, on our service.
constexpr char kTriggerMethod[] = "trigger";

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
}  // namespace

CoverPlugin::CoverPlugin(BinaryMessenger* messenger,
                         WindowBindingHandler* delegate)
    : channel_(std::make_unique<BasicMessageChannel<rapidjson::Document>>(
          messenger,
          kChannelName,
          &JsonMessageCodec::GetInstance())),
      delegate_(delegate) {
  const char* env = std::getenv("FLUTTER_SFOS_COVER_ACTION_TEST");
  if (env && std::strcmp(env, "1") == 0 && RegisterActionService()) {
    // One action per "<service>\n<method>\n<id>\n<icon>", joined by
    // newlines, then a trailing "\n0" (as other apps send it).
    const std::string actions = service_ + "\n" + kTriggerMethod +
                                "\n0\nimage://theme/icon-cover-refresh\n0";
    delegate_->SetCoverActions(actions);
    ELINUX_LOG(INFO) << "Cover action test: " << service_;
  }
}

CoverPlugin::~CoverPlugin() {
  if (bus_) {
    if (filter_id_) {
      g_dbus_connection_remove_filter(bus_, filter_id_);
    }
    g_object_unref(bus_);
  }
}

void CoverPlugin::SendShownAsCover(bool shown_as_cover) {
  rapidjson::Document message(rapidjson::kObjectType);
  auto& allocator = message.GetAllocator();
  message.AddMember(kVersion, kMessageVersion, allocator);
  message.AddMember(kShownAsCover, shown_as_cover, allocator);
  ELINUX_LOG(INFO) << "Shown as cover: " << shown_as_cover;
  channel_->Send(message);
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

bool CoverPlugin::RegisterActionService() {
  GError* error = nullptr;
  bus_ = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
  if (!bus_) {
    ELINUX_LOG(ERROR) << "Cover actions: no session bus: " << error->message;
    g_clear_error(&error);
    return false;
  }

  // The pattern other Sailfish apps use for their cover action service.
  service_ = std::string("org.sailfishos.coveraction.") +
             NameElement(program_invocation_short_name) + ".pid" +
             std::to_string(getpid()) + ".id1";
  path_ = "/" + service_;
  std::replace(path_.begin(), path_.end(), '.', '/');

  GVariant* reply = g_dbus_connection_call_sync(
      bus_, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "RequestName",
      g_variant_new("(su)", service_.c_str(), kDoNotQueue),
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
    ELINUX_LOG(ERROR) << "Cover actions: could not own " << service_;
    return false;
  }

  // GDBus only routes calls that name an interface to registered objects,
  // and the home screen's call may not, so catch it with a filter instead.
  filter_id_ = g_dbus_connection_add_filter(bus_, FilterMessage, this, nullptr);
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

  const char* member = g_dbus_message_get_member(message);
  const char* signature = g_dbus_message_get_signature(message);
  ELINUX_LOG(INFO) << "Cover action call: " << (member ? member : "")
                   << " (" << (signature ? signature : "") << ")";

  GDBusMessage* reply;
  if (g_strcmp0(member, kTriggerMethod) == 0) {
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
