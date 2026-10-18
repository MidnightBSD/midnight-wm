// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "xsmp_properties.hpp"

#include <cstring>
#include <utility>

namespace midnight {
XsmpProperties::XsmpProperties(std::string client_id) { properties_.client_id = std::move(client_id); }

std::expected<void, std::string> XsmpProperties::update(std::span<SmProp* const> properties) {
  if (properties.size() > 32) return std::unexpected("Too many XSMP properties");
  auto next = properties_;
  std::size_t bytes = 0;
  for (const auto* property : properties) {
    if (!property || !property->name || !property->type || !*property->name || !*property->type ||
        strnlen(property->name, 129) > 128 || strnlen(property->type, 33) > 32 ||
        property->num_vals < 0 || property->num_vals > 128 ||
        (property->num_vals && !property->vals))
      return std::unexpected("Malformed XSMP property");
    const std::string_view name(property->name), type(property->type);
    // Bound all incoming values, even those outside the retained allowlist.
    for (int i = 0; i < property->num_vals; ++i) {
      const auto& value = property->vals[i];
      if (value.length < 0 || value.length > 8192 || (value.length && !value.value))
        return std::unexpected("Malformed XSMP property value");
      bytes += static_cast<std::size_t>(value.length) + 1;
      if (bytes > 65536) return std::unexpected("XSMP properties exceed the size limit");
    }
    if (name == SmRestartStyleHint) {
      if (type != SmCARD8 || property->num_vals != 1 || property->vals[0].length != 1)
        return std::unexpected("Invalid XSMP restart style property");
      const auto style = *static_cast<const unsigned char*>(property->vals[0].value);
      if (style > SmRestartNever) return std::unexpected("Unknown XSMP restart style");
      switch (style) {
        case SmRestartIfRunning: next.style = RestartStyle::if_running; break;
        case SmRestartAnyway: next.style = RestartStyle::anyway; break;
        case SmRestartImmediately: next.style = RestartStyle::immediately; break;
        case SmRestartNever: next.style = RestartStyle::never; break;
      }
    } else if (name == SmProgram || name == SmCurrentDirectory || name == SmRestartCommand) {
      const bool command = name == SmRestartCommand;
      if (type != (command ? SmLISTofARRAY8 : SmARRAY8) || property->num_vals < 1 ||
          (!command && property->num_vals != 1))
        return std::unexpected("Invalid XSMP restart property type or count");
      std::vector<std::string> values;
      for (int i = 0; i < property->num_vals; ++i) {
        const auto& value = property->vals[i];
        std::string text;
        if (value.length) text.assign(static_cast<const char*>(value.value), static_cast<std::size_t>(value.length));
        if (text.find('\0') != std::string::npos)
          return std::unexpected("NUL in XSMP restart property");
        values.push_back(std::move(text));
      }
      if (!command && values.front().empty()) return std::unexpected("Empty XSMP scalar restart property");
      if (command) next.arguments = std::move(values);
      else if (name == SmProgram) next.program = std::move(values.front());
      else next.directory = std::move(values.front());
    }
    // Environment, DiscardCommand, ShutdownCommand, ResignCommand and other
    // properties are deliberately not copied into restart metadata.
  }
  // Program and RestartCommand can arrive in separate SetProperties messages.
  // Validate the accumulated scalar/argument limits even while incomplete.
  auto complete = next;
  if (complete.program.empty()) complete.program = "pending";
  if (complete.arguments.empty()) complete.arguments = {"pending"};
  if (auto validated = validate_application_restart(complete); !validated) return validated;
  properties_ = std::move(next);
  return {};
}

void XsmpProperties::remove(std::span<const std::string> names) {
  for (const auto& name : names) {
    if (name == SmProgram) properties_.program.clear();
    else if (name == SmRestartCommand) properties_.arguments.clear();
    else if (name == SmCurrentDirectory) properties_.directory.clear();
    else if (name == SmRestartStyleHint) properties_.style = RestartStyle::if_running;
  }
}

std::optional<ApplicationRestart> XsmpProperties::restart() const {
  if (!validate_application_restart(properties_) || properties_.style == RestartStyle::never) return std::nullopt;
  return properties_;
}
} // namespace midnight
