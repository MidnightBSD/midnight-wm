// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "xsmp_properties.hpp"

#include <array>
#include <iostream>
#include <utility>

namespace {
struct Property {
  std::string name, type;
  std::vector<std::string> data;
  std::vector<SmPropValue> values;
  SmProp property{};
  Property(std::string n, std::string t, std::vector<std::string> d)
      : name(std::move(n)), type(std::move(t)), data(std::move(d)) {
    for (auto& value : data) values.push_back({static_cast<int>(value.size()), value.data()});
    property = {name.data(), type.data(), static_cast<int>(values.size()), values.data()};
  }
};
}

int main() {
  bool passed = true;
  const auto check = [&](bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; passed = false; }
  };
  using midnight::XsmpProperties;
  using midnight::RestartStyle;
  XsmpProperties client("client-123");
  const auto update = [&](Property& p) {
    const std::array<SmProp*, 1> properties{&p.property};
    return client.update(properties);
  };
  Property program(SmProgram, SmARRAY8, {"application"});
  Property command(SmRestartCommand, SmLISTofARRAY8,
      {"/usr/local/bin/application", "--session-id", "client-123", "a b", "$(touch /tmp/unwanted)", "", "line\nbreak"});
  check(!client.restart(), "unregistered metadata was eligible for restart");
  check(bool(update(program)) && !client.restart(), "incomplete client accepted for restart");
  check(bool(update(command)), "valid restart command rejected");
  const auto baseline = client.restart();
  check(baseline && baseline->arguments == command.data && baseline->program == "application",
        "argument boundaries or literal data were not preserved");
  // SmPropValue data is length-delimited; no terminating NUL is required.
  command.data[0][0] = 'X';
  check(client.restart() == baseline, "properties borrowed client-owned memory");

  for (const auto& style : {std::pair{SmRestartIfRunning, RestartStyle::if_running},
       {SmRestartAnyway, RestartStyle::anyway}, {SmRestartImmediately, RestartStyle::immediately},
       {SmRestartNever, RestartStyle::never}}) {
    Property hint(SmRestartStyleHint, SmCARD8, {std::string(1, static_cast<char>(style.first))});
    check(bool(update(hint)), "valid restart style rejected");
    if (style.second == RestartStyle::never) check(!client.restart(), "RestartNever was eligible for restart");
    else check(client.restart() && client.restart()->style == style.second, "restart style was not preserved");
  }
  const std::array<std::string, 1> hint_name{SmRestartStyleHint};
  client.remove(hint_name);
  check(client.restart() == baseline, "deleting restart style did not restore its default");

  Property environment(SmEnvironment, SmLISTofARRAY8, {"SECRET", "credential", "DBUS_SESSION_BUS_ADDRESS", "private"});
  Property discard(SmDiscardCommand, SmLISTofARRAY8, {"rm", "-rf", "/tmp/something"});
  check(bool(update(environment)) && bool(update(discard)) && client.restart() == baseline,
        "non-allowlisted properties changed restart metadata");
  for (auto bad : {
      Property(SmRestartCommand, SmARRAY8, {"application"}),
      Property(SmProgram, SmLISTofARRAY8, {"application"}),
      Property(SmProgram, SmARRAY8, {""}),
      Property(SmProgram, SmARRAY8, {std::string(4097, 'x')}),
      Property(SmRestartCommand, SmLISTofARRAY8, {std::string("app\0injected", 12)}),
      Property(SmRestartCommand, SmLISTofARRAY8, {}),
      Property(SmRestartCommand, SmLISTofARRAY8, {""}),
      Property(SmRestartCommand, SmLISTofARRAY8, {"./application"}),
      Property(SmRestartCommand, SmLISTofARRAY8, {"-bad"}),
      Property(SmCurrentDirectory, SmARRAY8, {"relative"}),
      Property(SmRestartStyleHint, SmCARD8, {std::string(1, '\x7f')}),
      Property(SmRestartStyleHint, SmCARD8, {""})}) {
    // Reconstruct instead of copying pointers from the aggregate's source.
    Property property(bad.name, bad.type, bad.data);
    check(!update(property) && client.restart() == baseline, "invalid update changed valid client metadata");
  }
  Property oversized(SmRestartCommand, SmLISTofARRAY8, std::vector<std::string>(129, "argument"));
  check(!update(oversized), "unbounded argument count accepted");
  Property large(SmRestartCommand, SmLISTofARRAY8, std::vector<std::string>(16, std::string(8192, 'x')));
  check(!update(large), "aggregate size limit ignored");
  Property bad_value(SmRestartCommand, SmLISTofARRAY8, {"application"});
  bad_value.property.vals[0].length = -1;
  check(!update(bad_value), "negative property value length accepted");
  bad_value.property.vals[0].length = 10;
  bad_value.property.vals[0].value = nullptr;
  check(!update(bad_value), "NULL property data accepted");
  bad_value.property.num_vals = -1;
  check(!update(bad_value), "negative property count accepted");
  const std::array<SmProp*, 1> null_property{nullptr};
  check(!client.update(null_property), "NULL property accepted");
  const std::array<SmProp*, 33> too_many{};
  check(!client.update(too_many), "unbounded property count accepted");
  Property replacement(SmProgram, SmARRAY8, {"replacement"});
  const std::array<SmProp*, 2> batch{&replacement.property, nullptr};
  check(!client.update(batch) && client.restart() == baseline, "batch update was not transactional");
  const std::array<std::string, 1> command_name{SmRestartCommand};
  client.remove(command_name);
  check(!client.restart(), "deleted restart command was retained");
  Property directory(SmCurrentDirectory, SmARRAY8, {"/tmp/a directory"});
  Property simple(SmRestartCommand, SmLISTofARRAY8, {"application", "--restore"});
  check(bool(update(directory)) && bool(update(simple)) && client.restart() &&
        client.restart()->directory == "/tmp/a directory", "working directory was not preserved");
  XsmpProperties invalid_id("bad\nclient");
  const std::array<SmProp*, 1> one_program{&program.property};
  check(!invalid_id.update(one_program), "invalid client ID accepted");

  std::vector<midnight::ApplicationWindow> windows{
      {"client-123", "editor"}, {"client-123", "preview"}, {"other-client", "editor"}, {"single", ""}};
  using midnight::match_application_window;
  check(match_application_window(windows, "client-123", "editor") == 0, "client/role match failed");
  check(match_application_window(windows, "client-123", "preview") == 1, "second role match failed");
  check(match_application_window(windows, "other-client", "editor") == 2, "client IDs were conflated");
  check(match_application_window(windows, "single", "") == 3, "unique role-less window did not match");
  check(!match_application_window(windows, "", "editor") &&
        !match_application_window(windows, "client-123", ""), "unknown client or role guessed a match");
  const std::array<std::size_t, 1> claimed{0};
  check(!match_application_window(windows, "client-123", "editor", claimed), "saved placement was reused");
  windows.push_back(windows.front());
  check(!match_application_window(windows, "client-123", "editor"), "ambiguous role matched a window");
  check(!match_application_window(windows, "client-123", "editor", claimed),
        "claiming an ambiguous candidate made another candidate match");
  windows.push_back({"single", ""});
  check(!match_application_window(windows, "single", ""), "ambiguous role-less window matched");
  if (passed) std::cout << "XSMP property validation, ownership, restart policy, limits and role matching passed\n";
  return passed ? 0 : 1;
}
