#pragma once

#include "store.hpp"

#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace midnight {

struct ShellItem {
  Object object;
  std::filesystem::path path;
  bool physical{};
  bool trashed{};
  std::string uri;
  std::filesystem::path original_path;
};

// Filesystem and object semantics shared by display backends.
class ShellModel {
public:
  explicit ShellModel(Store& store) : store_(store) {}

  [[nodiscard]] std::vector<ShellItem> items(const std::filesystem::path& path, bool desktop) const;
  [[nodiscard]] std::filesystem::path resolve_path(const std::string& target) const;
  [[nodiscard]] static std::filesystem::path unique_path(const std::filesystem::path& desired);
  [[nodiscard]] static std::expected<void, std::string> copy_path(const std::filesystem::path& from,
                                                                  const std::filesystem::path& to);
  [[nodiscard]] std::expected<void, std::string> create_folder(const std::filesystem::path& parent) const;
  [[nodiscard]] static std::expected<void, std::string> transfer(const std::vector<std::filesystem::path>& sources,
                                                                 const std::filesystem::path& destination, bool move);
  [[nodiscard]] std::expected<void, std::string> add_reference(const ShellItem& item,
                                                               const std::filesystem::path& parent,
                                                               ObjectKind kind) const;
  [[nodiscard]] std::expected<void, std::string> update_properties(const ShellItem& item,
                                                                   const std::filesystem::path& parent,
                                                                   const std::string& name,
                                                                   const std::string& icon,
                                                                   const std::string& target) const;
  [[nodiscard]] std::expected<void, std::string> remove(const ShellItem& item) const;
  [[nodiscard]] static std::expected<void, std::string> empty_trash();
  [[nodiscard]] std::expected<void, std::string> restore(const ShellItem& item) const;
  [[nodiscard]] std::expected<void, std::string> delete_permanently(const ShellItem& item) const;

private:
  [[nodiscard]] std::filesystem::path resolve_path(const std::string& target, int depth) const;
  Store& store_;
};

} // namespace midnight
