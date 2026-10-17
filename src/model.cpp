#include "model.hpp"

#include <gio/gio.h>

#include <algorithm>
#include <ranges>
#include <system_error>

namespace midnight {
namespace {
std::string basename(const std::filesystem::path& path) {
  auto name = path.filename().string();
  return name.empty() ? path.string() : name;
}

std::string error_text(GError* error) {
  std::string message = error ? error->message : "unknown GIO error";
  if (error) g_error_free(error);
  return message;
}
} // namespace

std::filesystem::path ShellModel::resolve_path(const std::string& target) const {
  return resolve_path(target, 0);
}

std::filesystem::path ShellModel::resolve_path(const std::string& target, int depth) const {
  if (target.starts_with("db:")) {
    if (depth >= 8) return {};
    auto object = store_.get(target.substr(3));
    return object ? resolve_path(object->target, depth + 1) : std::filesystem::path{};
  }
  return target;
}

std::vector<ShellItem> ShellModel::items(const std::filesystem::path& path, bool desktop) const {
  std::vector<ShellItem> out;
  if (path == "trash:///") {
    auto* root = g_file_new_for_uri("trash:///");
    GError* error = nullptr;
    auto* enumerator = g_file_enumerate_children(root,
        "standard::name,standard::display-name,trash::orig-path", G_FILE_QUERY_INFO_NONE, nullptr, &error);
    if (enumerator) {
      while (auto* info = g_file_enumerator_next_file(enumerator, nullptr, &error)) {
        const char* name = g_file_info_get_name(info);
        const char* display = g_file_info_get_display_name(info);
        const char* original = g_file_info_get_attribute_byte_string(info, G_FILE_ATTRIBUTE_TRASH_ORIG_PATH);
        auto* child = g_file_get_child(root, name);
        char* uri = g_file_get_uri(child);
        std::filesystem::path original_path(original ? original : "");
        out.push_back({{"", ObjectKind::file, display ? display : name, original_path.string(), "", "trash:///", 0, 0},
                       original_path, false, true, uri ? uri : "", original_path});
        g_free(uri);
        g_object_unref(child);
        g_object_unref(info);
      }
      g_object_unref(enumerator);
    }
    if (error) g_error_free(error);
    g_object_unref(root);
  } else {
    const auto parent = desktop ? std::string{} : path.string();
    for (auto& object : store_.children(parent)) {
      const bool physical = object.id != "home" && object.id != "downloads" &&
        object.id != "templates" &&
        (object.kind == ObjectKind::file || object.kind == ObjectKind::folder);
      out.push_back({object, resolve_path(object.target), physical, false, "", {}});
    }
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
      for (const auto& entry : std::filesystem::directory_iterator(path,
             std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        const auto target = entry.path();
        if (std::ranges::any_of(out, [&](const ShellItem& item) {
              return item.physical && item.object.target == target.string();
            })) continue;
        out.push_back({{"", entry.is_directory(ec) ? ObjectKind::folder : ObjectKind::file,
                        basename(target), target.string(), "", parent, 0, 0}, target, true, false, "", {}});
      }
    }
  }
  std::ranges::sort(out, [](const ShellItem& a, const ShellItem& b) { return a.object.title < b.object.title; });
  return out;
}

std::filesystem::path ShellModel::unique_path(const std::filesystem::path& desired) {
  std::error_code ec;
  if (!std::filesystem::exists(desired, ec)) return desired;
  for (int n = 2; n < 10000; ++n) {
    auto candidate = desired.parent_path() / (desired.stem().string() + " " + std::to_string(n) + desired.extension().string());
    if (!std::filesystem::exists(candidate, ec)) return candidate;
  }
  return desired;
}

std::expected<void, std::string> ShellModel::copy_path(const std::filesystem::path& from,
                                                        const std::filesystem::path& to) {
  std::error_code ec;
  if (!std::filesystem::is_symlink(from, ec) && std::filesystem::is_directory(from, ec)) {
    std::filesystem::create_directory(to, ec);
    if (ec) return std::unexpected(ec.message());
    for (const auto& entry : std::filesystem::directory_iterator(from, ec)) {
      if (ec) return std::unexpected(ec.message());
      auto result = copy_path(entry.path(), to / entry.path().filename());
      if (!result) return result;
    }
    return {};
  }
  if (ec) return std::unexpected(ec.message());
  return midnight::copy_file(from, to);
}

std::expected<void, std::string> ShellModel::create_folder(const std::filesystem::path& parent) const {
  if (parent == "trash:///") return std::unexpected("Cannot create folders in Trash");
  std::error_code ec;
  std::filesystem::create_directory(unique_path(parent / "New Folder"), ec);
  if (ec) return std::unexpected(ec.message());
  return {};
}

std::expected<void, std::string> ShellModel::transfer(const std::vector<std::filesystem::path>& sources,
                                                       const std::filesystem::path& destination, bool move) {
  if (destination == "trash:///") {
    for (const auto& source : sources) {
      auto result = trash_file(source);
      if (!result) return result;
    }
    return {};
  }
  for (const auto& source : sources) {
    if (source.empty()) continue;
    if (move && source.parent_path() == destination) continue;
    std::error_code ec;
    if (std::filesystem::is_directory(source, ec)) {
      const auto canonical_source = std::filesystem::weakly_canonical(source, ec);
      const auto canonical_dest = std::filesystem::weakly_canonical(destination, ec);
      if (!ec) {
        auto relative = canonical_dest.lexically_relative(canonical_source);
        if (relative.empty() || (*relative.begin() != ".." && relative != "."))
          return std::unexpected("Cannot place a folder inside itself");
      }
    }
    const auto output = unique_path(destination / basename(source));
    auto result = move ? move_file(source, output) : copy_path(source, output);
    if (!result) return result;
  }
  return {};
}

std::expected<void, std::string> ShellModel::add_reference(const ShellItem& item,
                                                            const std::filesystem::path& parent,
                                                            ObjectKind kind) const {
  if (item.path.empty()) return std::unexpected("Target does not exist");
  const auto target = kind == ObjectKind::shadow && !item.object.id.empty()
    ? "db:" + item.object.id : item.path.string();
  Object object{Store::new_id(), kind, item.object.title + (kind == ObjectKind::shadow ? " shadow" : " template"),
                target, "", parent == Store::desktop_path() ? "" : parent.string(), 0, 0};
  return store_.put(object);
}

std::expected<void, std::string> ShellModel::update_properties(const ShellItem& item,
                                                                const std::filesystem::path& parent,
                                                                const std::string& name,
                                                                const std::string& icon,
                                                                const std::string& target) const {
  if (name.empty()) return std::unexpected("Name cannot be empty");
  Object object = item.object;
  if (item.physical) {
    auto path = item.path;
    if (name != basename(path)) {
      auto renamed = path.parent_path() / name;
      if (renamed.filename() != name || std::filesystem::exists(renamed))
        return std::unexpected("Invalid or existing file name");
      if (auto result = move_file(path, renamed); !result) return result;
      path = std::move(renamed);
    }
    if (object.id.empty()) object.id = Store::new_id();
    object.target = path.string();
    object.parent = parent == Store::desktop_path() ? "" : parent.string();
  } else object.target = target;
  object.title = name;
  object.icon = icon;
  return store_.put(object);
}

std::expected<void, std::string> ShellModel::remove(const ShellItem& item) const {
  if (item.physical) {
    auto result = trash_file(item.path);
    if (!result) return result;
    if (!item.object.id.empty()) return store_.erase(item.object.id);
    return {};
  }
  if (item.object.id.empty()) return std::unexpected("Object has no ID");
  return store_.erase(item.object.id);
}

std::expected<void, std::string> ShellModel::restore(const ShellItem& item) const {
  if (!item.trashed || item.original_path.empty()) return std::unexpected("Original location unavailable");
  auto* source = g_file_new_for_uri(item.uri.c_str());
  auto* target = g_file_new_for_path(item.original_path.c_str());
  GError* error = nullptr;
  const bool okay = g_file_move(source, target, G_FILE_COPY_NONE, nullptr, nullptr, nullptr, &error);
  g_object_unref(source);
  g_object_unref(target);
  if (!okay) return std::unexpected(error_text(error));
  return {};
}

std::expected<void, std::string> ShellModel::delete_permanently(const ShellItem& item) const {
  if (!item.trashed) return std::unexpected("Object is not in Trash");
  auto* source = g_file_new_for_uri(item.uri.c_str());
  GError* error = nullptr;
  const bool okay = g_file_delete(source, nullptr, &error);
  g_object_unref(source);
  if (!okay) return std::unexpected(error_text(error));
  return {};
}

} // namespace midnight
