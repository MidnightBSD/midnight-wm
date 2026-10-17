#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <vector>

struct sqlite3;

namespace midnight {

enum class ObjectKind { file, folder, launcher, shadow, template_object, trash };

struct Object {
  std::string id;
  ObjectKind kind{};
  std::string title;
  std::string target;
  std::string icon;
  std::string parent;
  int x{};
  int y{};
};

class Store {
public:
  explicit Store(std::filesystem::path db_path);
  ~Store();
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  [[nodiscard]] std::expected<void, std::string> open();
  [[nodiscard]] std::expected<void, std::string> put(const Object& object);
  [[nodiscard]] std::expected<void, std::string> erase(const std::string& id);
  [[nodiscard]] std::vector<Object> children(const std::string& parent) const;
  [[nodiscard]] std::expected<Object, std::string> get(const std::string& id) const;
  [[nodiscard]] static std::string new_id();
  [[nodiscard]] static std::filesystem::path data_path();
  [[nodiscard]] static std::filesystem::path desktop_path();

private:
  std::filesystem::path path_;
  sqlite3* db_{};
};

[[nodiscard]] std::expected<void, std::string> copy_file(const std::filesystem::path& from,
                                                           const std::filesystem::path& to);
[[nodiscard]] std::expected<void, std::string> move_file(const std::filesystem::path& from,
                                                           const std::filesystem::path& to);
[[nodiscard]] std::expected<void, std::string> trash_file(const std::filesystem::path& path);
[[nodiscard]] std::string file_uri(const std::filesystem::path& path);
[[nodiscard]] std::expected<std::filesystem::path, std::string> local_path_from_uri(const std::string& uri);

} // namespace midnight
