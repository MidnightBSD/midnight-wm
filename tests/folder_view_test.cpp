// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "folder_view.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <filesystem>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
using namespace midnight;

static ShellItem item(const char* name, std::int64_t size, std::int64_t modified,
                      const char* type, ObjectKind kind = ObjectKind::file) {
  ShellItem result;
  result.object.title = name;
  result.object.kind = kind;
  result.path = name;
  result.metadata.size = size;
  result.metadata.modified = modified;
  result.metadata.type = type;
  return result;
}
static void order(std::vector<ShellItem> items, FolderSort sort, const std::vector<std::string>& expected) {
  sort_folder_items(items, sort);
  assert(items.size() == expected.size());
  for (std::size_t i = 0; i < items.size(); ++i) assert(items[i].object.title == expected[i]);
}
int main() {
  const std::vector<ShellItem> files{item("beta.txt", 200, 30, "Text"), item("Alpha.png", 400, 10, "Image"),
                                    item("gamma.txt", 100, 20, "Text"), item("Folder", 0, 0, "Folder", ObjectKind::folder)};
  order(files, FolderSort::name_ascending, {"Folder", "Alpha.png", "beta.txt", "gamma.txt"});
  order(files, FolderSort::name_descending, {"Folder", "gamma.txt", "beta.txt", "Alpha.png"});
  order(files, FolderSort::newest, {"Folder", "beta.txt", "gamma.txt", "Alpha.png"});
  order(files, FolderSort::oldest, {"Folder", "Alpha.png", "gamma.txt", "beta.txt"});
  order(files, FolderSort::size, {"Folder", "Alpha.png", "beta.txt", "gamma.txt"});
  order(files, FolderSort::type, {"Folder", "Alpha.png", "beta.txt", "gamma.txt"});
  auto unknown = item("unknown", 0, 0, "File");
  unknown.metadata.size.reset(); unknown.metadata.modified.reset();
  order({unknown, item("known", 0, 0, "File")}, FolderSort::oldest, {"known", "unknown"});
  order({unknown, item("known", 0, 0, "File")}, FolderSort::size, {"known", "unknown"});
  auto early = item("a", 0, 10, "File"), late = item("z", 0, 10, "File");
  late.metadata.modified_nanoseconds = 99;
  order({early, late}, FolderSort::newest, {"z", "a"});
  order({item("file10", 1, 0, "File"), item("file2", 1, 0, "File")}, FolderSort::name_ascending, {"file2", "file10"});
  auto invalid = item("bad\xff", 1, 0, "File");
  std::vector<ShellItem> malformed{invalid, early};
  sort_folder_items(malformed, FolderSort::name_ascending);
  assert(malformed.size() == 2);

  assert(folder_columns(FolderView::grid, 590) == 6);
  assert(folder_columns(FolderView::list, 590) == 1);
  assert(folder_visible_rows(FolderView::grid, 353) == 3);
  assert(folder_visible_rows(FolderView::list, 353) == 10);
  assert(folder_item_at(FolderView::list, 12, 79, 590, 353, 40, 0) == 0);
  assert(folder_item_at(FolderView::list, 500, 108, 590, 353, 40, 0) == 0);
  assert(folder_item_at(FolderView::list, 500, 109, 590, 353, 40, 3) == 4);
  assert(folder_item_at(FolderView::list, 12, 78, 590, 353, 40, 0) == -1); // header
  assert(folder_item_at(FolderView::list, 4, 79, 590, 353, 40, 0) == -1);
  assert(folder_item_at(FolderView::list, 595, 79, 590, 353, 40, 0) == -1);
  assert(folder_item_at(FolderView::list, 12, 408, 590, 353, 40, 0) == -1); // footer
  assert(folder_item_at(FolderView::list, 12, 109, 590, 353, 1, 0) == -1); // empty row
  assert(folder_item_at(FolderView::grid, 12, 66, 590, 353, 40, 2) == 12);
  assert(folder_item_at(FolderView::grid, 102, 66, 590, 353, 40, 2) == 13);
  assert(folder_item_at(FolderView::grid, 91, 66, 590, 353, 40, 2) == -1); // gap
  for (int width : {1, 225, 269, 270, 409, 410, 549, 550, 590, 2000}) {
    const auto columns = folder_list_columns(width);
    int x = 5;
    for (const auto& column : columns) {
      assert(column.x == x && column.width > 0);
      x += column.width;
    }
    assert(x == 5 + width);
  }

  char temporary[] = "/tmp/midnight-folder-view-XXXXXX";
  const auto* name = mkdtemp(temporary);
  assert(name);
  const std::filesystem::path root(name);
  std::ofstream(root / "example.txt").put('x');
  std::filesystem::resize_file(root / "example.txt", 1048576);
  auto file = item("example.txt", 0, 0, "");
  file.path = root / "example.txt";
  read_folder_metadata(file);
  assert(file.metadata.size == 1048576 && file.metadata.modified && !file.metadata.type.empty());
  assert(folder_modified_label(file) != "—");
  std::filesystem::create_symlink("missing", root / "link");
  file.path = root / "link";
  read_folder_metadata(file);
  assert(file.metadata.size == 7 && file.metadata.type == "Symbolic link");
  assert(mkfifo((root / "pipe").c_str(), 0600) == 0);
  file.path = root / "pipe";
  read_folder_metadata(file);
  assert(!file.metadata.size); // never read a special file
  file.path = root / "absent";
  read_folder_metadata(file);
  assert(!file.metadata.modified && !file.metadata.size);
  assert(folder_modified_label(file) == "—");
  std::filesystem::remove_all(root);
}
