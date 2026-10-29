// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_PLUGINS_TEXT_INPUT_PLUGIN_H_
#define FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_PLUGINS_TEXT_INPUT_PLUGIN_H_

#include <rapidjson/document.h>

#include <memory>

#include <glib.h>
#include <maliit-glib/maliitbus.h>
#include <maliit-glib/maliitcontext.h>
#include <maliit-glib/maliitserver.h>

#include "flutter/shell/platform/common/client_wrapper/include/flutter/binary_messenger.h"
#include "flutter/shell/platform/common/client_wrapper/include/flutter/method_channel.h"
#include "flutter/shell/platform/common/text_input_model.h"
#include "flutter/shell/platform/linux_embedded/window_binding_handler.h"

namespace flutter {

class TextInputPlugin {
 public:
  TextInputPlugin(BinaryMessenger* messenger, WindowBindingHandler* delegate);
  ~TextInputPlugin() = default;

  void OnKeyPressed(uint32_t keycode, uint32_t code_point);
  void DispatchEvent();
  void OnContentOrientationChanged();

 private:
  void InitMaliitConnection();
  void MaliitShowInputMethod();
  void MaliitHideInputMethod();
  void MaliitSendAppOrientation();

  // Tells the keyboard about the client: its field type, its hints and the
  // text around the cursor.
  void MaliitUpdateWidgetInformation(bool focus_changed);

  // Sets the enter key's icon for the client.
  void MaliitSetEnterKey();

  static gboolean MaliitHandleIMInitiatedHide(MaliitContext* obj,
                                              GDBusMethodInvocation* invocation,
                                              gpointer user_data);
  static gboolean MaliitHandleCommitString(MaliitContext* obj,
                                           GDBusMethodInvocation* invocation,
                                           const gchar* string,
                                           int replacement_start,
                                           int replacement_length,
                                           int cursor_pos,
                                           gpointer user_data);
  static gboolean MaliitHandleUpdatePreedit(MaliitContext* obj,
                                            GDBusMethodInvocation* invocation,
                                            const gchar* string,
                                            GVariant* format_list_data,
                                            gint replace_start,
                                            gint replace_length,
                                            gint cursor_pos,
                                            gpointer user_data);
  static gboolean MaliitHandleKeyEvent(MaliitContext* obj,
                                       GDBusMethodInvocation* invocation,
                                       gint type,
                                       gint key,
                                       gint modifiers,
                                       const gchar* text,
                                       gboolean auto_repeat,
                                       int count,
                                       guchar request_type,
                                       gpointer user_data);
  static gboolean MaliitHandleUpdateInputMethodArea(
      MaliitContext* obj,
      GDBusMethodInvocation* invocation,
      gint x,
      gint y,
      gint width,
      gint height,
      gpointer user_data);

  // Sends the current state of the given model to the Flutter engine.
  void SendStateUpdate(const TextInputModel& model);

  // Sends an action triggered by the Enter key to the Flutter engine.
  void EnterPressed(TextInputModel* model);

  // Called when a method is called on |channel_|;
  void HandleMethodCall(
      const flutter::MethodCall<rapidjson::Document>& method_call,
      std::unique_ptr<flutter::MethodResult<rapidjson::Document>> result);

  // The MethodChannel used for communication with the Flutter engine.
  std::unique_ptr<flutter::MethodChannel<rapidjson::Document>> channel_;

  // The active client id.
  int client_id_ = 0;

  // The active model. nullptr if not set.
  std::unique_ptr<TextInputModel> active_model_;

  // Keyboard type of the client. See available options:
  // https://docs.flutter.io/flutter/services/TextInputType-class.html
  std::string input_type_;

  // An action requested by the user on the input client. See available options:
  // https://docs.flutter.io/flutter/services/TextInputAction-class.html
  std::string input_action_;

  // The delegate for virtual keyboard updates.
  WindowBindingHandler* delegate_;

  // What the keyboard is told about the client, from its configuration.
  int content_type_ = 0;
  bool hidden_text_ = false;
  bool prediction_enabled_ = true;
  bool correction_enabled_ = true;
  bool autocapitalization_enabled_ = false;
  std::string enter_key_icon_;

  // Whether the keyboard has yet to be shown for the client.
  bool focus_changed_ = false;

  GMainContext* glib_ctx_ = nullptr;
  GMainLoop* glib_loop_ = nullptr;
  MaliitServer* maliit_server_ = nullptr;
  MaliitContext* maliit_context_ = nullptr;
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_PLUGINS_TEXT_INPUT_PLUGIN_H_
