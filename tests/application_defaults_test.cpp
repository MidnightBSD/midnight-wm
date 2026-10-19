// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "application_defaults.hpp"
#include "browser_launcher.hpp"
#include "terminal_launcher.hpp"
#include <glib.h>
#include <filesystem>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
using namespace midnight;
namespace fs = std::filesystem;
static std::string read(const fs::path& path) {
  std::ifstream in(path);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
int main() {
  auto temp = fs::temp_directory_path() / "midnight-defaults-test-XXXXXX";
  std::string name = temp.string();
  if (!mkdtemp(name.data())) return 1;
  const fs::path root = name;
  const auto config = root / "config";
  const auto apps = root / "data/applications";
  fs::create_directories(config);
  fs::create_directories(apps);
  g_setenv("XDG_CONFIG_HOME", config.c_str(), true);
  g_setenv("XDG_DATA_HOME", (root / "data").c_str(), true);
  g_setenv("XDG_CURRENT_DESKTOP", "MidnightWorkspace", true);
  {
    std::ofstream out(apps / "test-browser.desktop");
    out << "[Desktop Entry]\nType=Application\nName=Test Browser\nExec=/usr/bin/true\n"
           "MimeType=x-scheme-handler/http;x-scheme-handler/https;text/html;\n";
  }
  {
    std::ofstream out(apps / "test-terminal.desktop");
    out << "[Desktop Entry]\nType=Application\nName=Test Terminal\nExec=/usr/bin/true\n"
           "Categories=System;TerminalEmulator;\n";
  }
  {
    std::ofstream out(apps / "test-html-editor.desktop");
    out << "[Desktop Entry]\nType=Application\nName=Test HTML Editor\nExec=/usr/bin/true\n"
           "MimeType=text/html;\n";
  }
  {
    std::ofstream out(apps / "test-calendar.desktop");
    out << "[Desktop Entry]\nType=Application\nName=Test Calendar\nExec=/usr/bin/true\n"
           "MimeType=text/calendar;\n";
  }
  {
    std::ofstream out(apps / "test-text-editor.desktop");
    out << "[Desktop Entry]\nType=Application\nName=Test Text Editor\nExec=/usr/bin/true\n"
           "MimeType=text/plain;\n";
  }
  {
    std::ofstream out(apps / "mimeinfo.cache");
    out << "[MIME Cache]\n"
           "x-scheme-handler/http=test-browser.desktop;\n"
           "x-scheme-handler/https=test-browser.desktop;\n"
           "text/html=test-browser.desktop;test-html-editor.desktop;\n"
           "text/calendar=test-calendar.desktop;\n"
           "text/plain=test-text-editor.desktop;\n";
  }
  {
    std::ofstream out(config / "midnightworkspace-mimeapps.list");
    out << "# preserve this comment\n[Default Applications]\napplication/pdf=other.desktop;\n"
           "[Custom]\nkeep=unchanged\n";
  }
  {
    std::ofstream out(config / "mimeapps.list");
    out << "# generic comment\n[Removed Associations]\nx-scheme-handler/https=test-browser.desktop;\n";
  }
  {
    std::ofstream out(config / "midnightworkspace-xdg-terminals.list");
    out << "# preserve this comment\nold-terminal.desktop\n";
  }
  const auto browser_choices = app_default_choices(AppDefaultRole::browser);
  const auto html_choices = app_default_choices(AppDefaultRole::html);
  const auto calendar_choices = app_default_choices(AppDefaultRole::calendar);
  const auto terminal_choices = app_default_choices(AppDefaultRole::terminal);
  const auto contains = [](const auto& choices, const char* id) {
    return std::ranges::any_of(choices, [id](const auto& item) { return item.id == id; });
  };
  if (!contains(browser_choices, "test-browser.desktop") ||
      contains(browser_choices, "test-html-editor.desktop") ||
      !contains(html_choices, "test-html-editor.desktop") ||
      !contains(calendar_choices, "test-calendar.desktop") ||
      contains(calendar_choices, "test-text-editor.desktop") ||
      !contains(terminal_choices, "test-terminal.desktop")) return 2;
  const auto browser_applied = save_app_defaults({{AppDefaultRole::browser, "test-browser.desktop"}});
  if (!browser_applied || read(config / "midnightworkspace-mimeapps.list").find("text/html=") != std::string::npos)
    return 3;
  const auto applied = save_app_defaults({{AppDefaultRole::html, "test-html-editor.desktop"},
                                          {AppDefaultRole::terminal, "test-terminal.desktop"}});
  if (!applied) { std::cerr << applied.error() << '\n'; return 3; }
  const auto mime = read(config / "midnightworkspace-mimeapps.list");
  const auto associations = read(config / "mimeapps.list");
  const auto terminals = read(config / "midnightworkspace-xdg-terminals.list");
  if (mime.find("# preserve this comment") == std::string::npos ||
      mime.find("keep=unchanged") == std::string::npos ||
      mime.find("application/pdf=other.desktop;") == std::string::npos ||
      mime.find("x-scheme-handler/http=test-browser.desktop;") == std::string::npos ||
      mime.find("x-scheme-handler/https=test-browser.desktop;") == std::string::npos ||
      mime.find("text/html=test-html-editor.desktop;") == std::string::npos ||
      associations.find("# generic comment") == std::string::npos ||
      associations.find("test-browser.desktop") == std::string::npos ||
      associations.find("[Removed Associations]\nx-scheme-handler/https=test-browser.desktop") != std::string::npos ||
      !terminals.starts_with("test-terminal.desktop\n") ||
      terminals.find("# preserve this comment") == std::string::npos ||
      terminals.find("old-terminal.desktop") == std::string::npos) return 4;
  auto* browser = preferred_web_browser();
  auto* html = g_app_info_get_default_for_type("text/html", false);
  auto* terminal = preferred_terminal();
  const bool effective = browser && html && terminal && g_app_info_get_id(browser) &&
      g_app_info_get_id(html) &&
      g_app_info_get_id(terminal) &&
      std::string(g_app_info_get_id(browser)) == "test-browser.desktop" &&
      std::string(g_app_info_get_id(html)) == "test-html-editor.desktop" &&
      std::string(g_app_info_get_id(terminal)) == "test-terminal.desktop";
  if (browser) g_object_unref(browser);
  if (html) g_object_unref(html);
  if (terminal) g_object_unref(terminal);
  if (!effective) return 5;
  const auto before = read(config / "midnightworkspace-mimeapps.list");
  {
    std::ofstream out(config / "midnightworkspace-mimeapps.list");
    out << "[Broken\n";
  }
  if (save_app_defaults({{AppDefaultRole::browser, "test-browser.desktop"}}) ||
      read(config / "midnightworkspace-mimeapps.list") != "[Broken\n") return 6;
  if (save_app_defaults({{AppDefaultRole::browser, "missing.desktop"}})) return 7;
  if (before.empty()) return 8;
  fs::remove_all(root);
  std::cout << "Application choices, XDG writes, preservation, effective defaults and malformed-file recovery passed\n";
}
