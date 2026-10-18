// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "folder_view.hpp"
#include <gio/gio.h>
#include <sys/stat.h>
#include <numeric>
#include <tuple>

namespace midnight {
int folder_item_at(FolderView view, int x, int y, int width, int height,
                   std::size_t count, int scroll) {
  const int top = folder_items_top(view);
  if (x < 5 || x >= 5 + width || y < top || y >= 55 + height) return -1;
  const int columns = folder_columns(view, width);
  int column = 0, row = (y - top) / folder_list_row;
  if (view == FolderView::grid) {
    if (x < 12 || (x - 12) % 90 >= 78 || (y - top) % 90 >= 78) return -1;
    column = (x - 12) / 90;
    if (column >= columns) return -1;
    row = (y - top) / 90;
  }
  const auto index = static_cast<std::size_t>(row + std::max(0, scroll)) * columns + column;
  return index < count ? static_cast<int>(index) : -1;
}

std::vector<FolderColumnLayout> folder_list_columns(int width) {
  const bool size = width >= 270, modified = width >= 410, type = width >= 550;
  const int name_width = width - (size ? 82 : 0) - (type ? 115 : 0) - (modified ? 150 : 0);
  std::vector<FolderColumnLayout> result{{FolderColumn::name, 5, std::max(1, name_width)}};
  int x = 5 + name_width;
  if (size) { result.push_back({FolderColumn::size, x, 82}); x += 82; }
  if (type) { result.push_back({FolderColumn::type, x, 115}); x += 115; }
  if (modified) result.push_back({FolderColumn::modified, x, 150});
  return result;
}

void read_folder_metadata(ShellItem& item) {
  item.metadata = {};
  bool link = false;
  if (item.trashed) {
    auto* file = g_file_new_for_uri(item.uri.c_str());
    auto* info = g_file_query_info(file, "standard::type,standard::size,time::modified,time::modified-usec",
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr);
    if (info) {
      link = g_file_info_get_file_type(info) == G_FILE_TYPE_SYMBOLIC_LINK;
      if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) item.object.kind = ObjectKind::folder;
      if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_SIZE)) item.metadata.size = g_file_info_get_size(info);
      if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_TIME_MODIFIED)) {
        const auto modified = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
        if (modified <= static_cast<std::uint64_t>(INT64_MAX)) item.metadata.modified = static_cast<std::int64_t>(modified);
        item.metadata.modified_nanoseconds = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC) * 1000;
      }
      g_object_unref(info);
    }
    g_object_unref(file);
  } else if (!item.path.empty()) {
    struct stat status{};
    if (lstat(item.path.c_str(), &status) == 0) {
      link = S_ISLNK(status.st_mode);
      if (status.st_size >= 0 && (S_ISREG(status.st_mode) || link)) item.metadata.size = status.st_size;
      item.metadata.modified = status.st_mtim.tv_sec;
      item.metadata.modified_nanoseconds = status.st_mtim.tv_nsec;
    }
  }
  if (link) item.metadata.type = "Symbolic link";
  else if (item.object.kind == ObjectKind::folder) item.metadata.type = "Folder";
  else if (item.object.kind == ObjectKind::shadow) item.metadata.type = "Shadow";
  else if (item.object.kind == ObjectKind::launcher) item.metadata.type = "Launcher";
  else if (item.object.kind == ObjectKind::template_object) item.metadata.type = "Template";
  else {
    // Filename-only MIME lookup avoids reading contents, devices or FIFOs.
    const auto name = item.trashed ? item.object.title : item.path.filename().string();
    char* content_type = g_content_type_guess(name.c_str(), nullptr, 0, nullptr);
    char* description = content_type ? g_content_type_get_description(content_type) : nullptr;
    item.metadata.type = description ? description : "File";
    g_free(description);
    g_free(content_type);
  }
}

namespace {
std::string name_key(const std::string& name) {
  char* valid = g_utf8_make_valid(name.data(), name.size());
  char* folded = g_utf8_casefold(valid, -1);
  char* key = g_utf8_collate_key_for_filename(folded, -1);
  std::string result(key);
  g_free(key); g_free(folded); g_free(valid);
  return result;
}
}

void sort_folder_items(std::vector<ShellItem>& items, FolderSort sort) {
  struct Keys { std::string name, type; };
  std::vector<Keys> keys;
  keys.reserve(items.size());
  for (const auto& item : items) keys.push_back({name_key(item.object.title), name_key(item.metadata.type)});
  std::vector<std::size_t> order(items.size());
  std::iota(order.begin(), order.end(), 0);
  std::ranges::sort(order, [&](std::size_t a, std::size_t b) {
    const auto& left = items[a]; const auto& right = items[b];
    const bool left_folder = left.object.kind == ObjectKind::folder;
    const bool right_folder = right.object.kind == ObjectKind::folder;
    if (left_folder != right_folder) return left_folder;
    if (sort == FolderSort::size && !left_folder && left.metadata.size != right.metadata.size) {
      if (!left.metadata.size || !right.metadata.size) return left.metadata.size.has_value();
      return *left.metadata.size > *right.metadata.size;
    }
    if (sort == FolderSort::newest || sort == FolderSort::oldest) {
      if (left.metadata.modified.has_value() != right.metadata.modified.has_value()) return left.metadata.modified.has_value();
      if (left.metadata.modified && right.metadata.modified) {
        const auto lt = std::pair(*left.metadata.modified, left.metadata.modified_nanoseconds);
        const auto rt = std::pair(*right.metadata.modified, right.metadata.modified_nanoseconds);
        if (lt != rt) return sort == FolderSort::newest ? lt > rt : lt < rt;
      }
    }
    if (sort == FolderSort::type && keys[a].type != keys[b].type) return keys[a].type < keys[b].type;
    if (keys[a].name != keys[b].name) return sort == FolderSort::name_descending ? keys[a].name > keys[b].name : keys[a].name < keys[b].name;
    return std::tie(left.object.title, left.path, left.uri, left.object.id) <
           std::tie(right.object.title, right.path, right.uri, right.object.id);
  });
  std::vector<ShellItem> sorted;
  sorted.reserve(items.size());
  for (auto index : order) sorted.push_back(std::move(items[index]));
  items = std::move(sorted);
}

std::string folder_modified_label(const ShellItem& item) {
  if (!item.metadata.modified) return "—";
  auto* date = g_date_time_new_from_unix_local(*item.metadata.modified);
  if (!date) return "—";
  char* label = g_date_time_format(date, "%Y-%m-%d %H:%M");
  std::string result(label ? label : "—");
  g_free(label);
  g_date_time_unref(date);
  return result;
}
} // namespace midnight
