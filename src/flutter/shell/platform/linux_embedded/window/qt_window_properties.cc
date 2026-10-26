// Copyright 2026 Dave Nash. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/linux_embedded/window/qt_window_properties.h"

#include <cstring>
#include <vector>

namespace flutter {

namespace {

// QDataStream (Qt 5) writes a QVariant as its big-endian type id, a null flag
// byte, then the value.
constexpr uint32_t kQMetaTypeBool = 1;
constexpr uint32_t kQMetaTypeULongLong = 5;
constexpr uint32_t kQMetaTypeQString = 10;

void AppendU32(std::vector<uint8_t>& out, uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    out.push_back(static_cast<uint8_t>(value >> shift));
  }
}

std::vector<uint8_t> VariantHeader(uint32_t type) {
  std::vector<uint8_t> out;
  AppendU32(out, type);
  out.push_back(0);  // Not null.
  return out;
}

void Send(qt_extended_surface* surface,
          const char* name,
          const std::vector<uint8_t>& value) {
  wl_array array;
  wl_array_init(&array);
  std::memcpy(wl_array_add(&array, value.size()), value.data(), value.size());
  qt_extended_surface_update_generic_property(surface, name, &array);
  wl_array_release(&array);
}

}  // namespace

void SetBoolWindowProperty(qt_extended_surface* surface,
                           const char* name,
                           bool value) {
  auto out = VariantHeader(kQMetaTypeBool);
  out.push_back(value ? 1 : 0);
  Send(surface, name, out);
}

void SetULongLongWindowProperty(qt_extended_surface* surface,
                                const char* name,
                                uint64_t value) {
  auto out = VariantHeader(kQMetaTypeULongLong);
  AppendU32(out, static_cast<uint32_t>(value >> 32));
  AppendU32(out, static_cast<uint32_t>(value));
  Send(surface, name, out);
}

void SetStringWindowProperty(qt_extended_surface* surface,
                             const char* name,
                             const std::string& value) {
  // A QString is its UTF-16 byte length, then UTF-16BE; ASCII maps directly.
  auto out = VariantHeader(kQMetaTypeQString);
  AppendU32(out, static_cast<uint32_t>(value.size() * 2));
  for (char c : value) {
    out.push_back(0);
    out.push_back(static_cast<uint8_t>(c));
  }
  Send(surface, name, out);
}

}  // namespace flutter
