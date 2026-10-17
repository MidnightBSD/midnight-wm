#include "model.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

namespace {
bool check(bool condition, const char* message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

struct TemporaryDirectory {
  fs::path path = fs::temp_directory_path() / ("midnight-model-" + midnight::Store::new_id());
  TemporaryDirectory() { fs::create_directories(path); }
  ~TemporaryDirectory() { std::error_code error; fs::remove_all(path, error); }
};
} // namespace

int main() {
  TemporaryDirectory temp;
  const auto data = temp.path / "data";
  setenv("XDG_DATA_HOME", data.c_str(), 1);
  midnight::Store store(data / "workspace.db");
  if (!check(store.open().has_value(), "store failed to open")) return 1;
  midnight::ShellModel model(store);

  const auto home = temp.path / "home";
  const auto destination = temp.path / "destination";
  fs::create_directories(home);
  fs::create_directories(destination);
  const auto original = home / "note.txt";
  { std::ofstream stream(original); stream << "workspace\n"; }
  midnight::Object object{midnight::Store::new_id(), midnight::ObjectKind::file,
                          "note.txt", original.string(), "", home.string(), 0, 0};
  if (!check(store.put(object).has_value(), "object failed to persist")) return 1;
  auto items = model.items(home, false);
  if (!check(items.size() == 1, "physical object was duplicated")) return 1;
  if (!check(model.add_reference(items[0], home, midnight::ObjectKind::shadow).has_value(), "shadow creation failed")) return 1;
  items = model.items(home, false);
  if (!check(items.size() == 2, "shadow obscured its physical target")) return 1;
  if (!check(model.add_reference(items[0], home, midnight::ObjectKind::template_object).has_value(),
             "template creation failed")) return 1;
  items = model.items(home, false);
  if (!check(items.size() == 3, "template obscured its physical target")) return 1;
  const auto shadow = std::ranges::find_if(items, [](const auto& item) {
    return item.object.kind == midnight::ObjectKind::shadow;
  });
  if (!check(shadow != items.end() && shadow->object.target == "db:" + object.id,
             "shadow does not use stable object ID")) return 1;
  if (!check(model.update_properties(items[0], home, "renamed.txt", "", original.string()).has_value(),
             "rename failed")) return 1;
  items = model.items(home, false);
  const auto renamed = home / "renamed.txt";
  const auto shadow_after = std::ranges::find_if(items, [](const auto& item) {
    return item.object.kind == midnight::ObjectKind::shadow;
  });
  if (!check(fs::exists(renamed) && shadow_after != items.end() && shadow_after->path == renamed,
             "shadow did not follow renamed object")) return 1;

  if (!check(model.transfer({renamed}, destination, false).has_value(), "first copy failed")) return 1;
  if (!check(model.transfer({renamed}, destination, false).has_value(), "collision copy failed")) return 1;
  if (!check(fs::exists(destination / "renamed.txt") && fs::exists(destination / "renamed 2.txt"),
             "copy collision did not create unique name")) return 1;
  if (!check(!model.transfer({destination}, destination / "nested", false),
             "self copy was accepted")) return 1;
  if (!check(model.transfer({renamed}, home, true).has_value() && fs::exists(renamed),
             "same-directory move damaged source")) return 1;

  const auto trash_candidate = home / "trash-me.txt";
  { std::ofstream stream(trash_candidate); stream << "restore me\n"; }
  midnight::ShellItem physical{{"", midnight::ObjectKind::file, "trash-me.txt", trash_candidate.string(), "", home.string(), 0, 0},
                               trash_candidate, true, false, "", {}};
  auto trashed = model.remove(physical);
  if (!trashed && trashed.error().find("Trashing on system internal mounts is not supported") != std::string::npos) {
    std::cerr << "Trash/restore skipped: " << trashed.error() << '\n';
    return 0;
  }
  if (!trashed) { std::cerr << "trash operation failed: " << trashed.error() << '\n'; return 1; }
  if (!check(!fs::exists(trash_candidate), "trashed file remains in place")) return 1;
  const auto trash_items = model.items("trash:///", false);
  const auto in_trash = std::ranges::find_if(trash_items, [&](const auto& item) {
    return item.original_path == trash_candidate;
  });
  if (!check(in_trash != trash_items.end(), "trashed file absent from built-in Trash")) return 1;
  if (!check(model.restore(*in_trash).has_value() && fs::exists(trash_candidate), "restore failed")) return 1;
  return 0;
}
