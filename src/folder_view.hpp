// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "model.hpp"
#include <algorithm>
#include <cstddef>
#include <vector>

namespace midnight {
enum class FolderView { grid, list };
enum class FolderSort { name_ascending, name_descending, newest, oldest, size, type };
enum class FolderColumn { name, size, type, modified };
struct FolderColumnLayout { FolderColumn column; int x, width; };
constexpr int folder_list_header = 24;
constexpr int folder_list_row = 30;
constexpr int folder_items_top(FolderView view) { return view == FolderView::list ? 55 + folder_list_header : 66; }
constexpr int folder_columns(FolderView view, int width) { return view == FolderView::list ? 1 : std::max(1, width / 90); }
constexpr int folder_visible_rows(FolderView view, int height) {
  return std::max(1, (height - (view == FolderView::list ? folder_list_header : 11)) /
                        (view == FolderView::list ? folder_list_row : 90));
}
[[nodiscard]] int folder_item_at(FolderView view, int x, int y, int width, int height,
                                 std::size_t count, int scroll);
[[nodiscard]] std::vector<FolderColumnLayout> folder_list_columns(int width);
// Read metadata once on listing/refresh, never inside a comparator or draw call.
void read_folder_metadata(ShellItem& item);
void sort_folder_items(std::vector<ShellItem>& items, FolderSort sort);
[[nodiscard]] std::string folder_modified_label(const ShellItem& item);
} // namespace midnight
