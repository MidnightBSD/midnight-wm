#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace midnight {
inline constexpr auto workspace_file_manager_name = "org.midnightbsd.Workspace.FileManager";
inline constexpr auto workspace_file_manager_path = "/org/midnightbsd/Workspace/FileManager";

// Session D-Bus interface used by applications to reveal files and folders.
// Callbacks keep the protocol independent of the X11 shell implementation.
class FileManager1 {
public:
  using ShowFolder = std::function<bool(const std::filesystem::path&,
                                        const std::optional<std::filesystem::path>&)>;
  using ShowProperties = std::function<bool(const std::filesystem::path&)>;

  FileManager1(ShowFolder show_folder, ShowProperties show_properties,
               std::string service_name = "org.freedesktop.FileManager1",
               std::string object_path = "/org/freedesktop/FileManager1");
  ~FileManager1();
  FileManager1(const FileManager1&) = delete;
  FileManager1& operator=(const FileManager1&) = delete;

  [[nodiscard]] bool initialize();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace midnight
