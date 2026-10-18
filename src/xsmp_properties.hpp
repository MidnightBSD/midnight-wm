// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include "application_session.hpp"
#include <X11/SM/SMlib.h>

namespace midnight {
// Per-client allowlist for a future SmsSetProperties callback. Caller retains
// ownership of every SmProp. Updates are transactional; deleted properties stop
// being eligible for restart. Values are copied using their explicit lengths.
class XsmpProperties {
public:
  explicit XsmpProperties(std::string client_id);
  std::expected<void, std::string> update(std::span<SmProp* const> properties);
  void remove(std::span<const std::string> names);
  std::optional<ApplicationRestart> restart() const;
private:
  ApplicationRestart properties_;
};
} // namespace midnight
