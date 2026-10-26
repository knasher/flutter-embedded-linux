// Copyright 2026 Dave Nash. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_WINDOW_QT_WINDOW_PROPERTIES_H_
#define FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_WINDOW_QT_WINDOW_PROPERTIES_H_

#include <cstdint>
#include <string>

#include "wayland/protocols/surface-extension-client-protocol.h"

namespace flutter {

// Window properties of the Qt surface extension, which Lipstick reads (e.g.
// CATEGORY, WINID, _APP_COVER_ACTION). The values are QVariants serialised
// by QDataStream (Qt 5).

void SetBoolWindowProperty(qt_extended_surface* surface,
                           const char* name,
                           bool value);

void SetULongLongWindowProperty(qt_extended_surface* surface,
                                const char* name,
                                uint64_t value);

// |value| must be ASCII.
void SetStringWindowProperty(qt_extended_surface* surface,
                             const char* name,
                             const std::string& value);

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_LINUX_EMBEDDED_WINDOW_QT_WINDOW_PROPERTIES_H_
