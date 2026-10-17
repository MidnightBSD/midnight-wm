#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

namespace midnight {

// Session D-Bus interface used by applications to reveal files and folders.
// Callbacks keep the protocol independent of the X11 shell implementation.
class FileManager1 {
public:
  using ShowFolder = std::function<bool(const std::filesystem::path&,
                                        const std::optional<std::filesystem::path>&)>;
  using ShowProperties = std::function<bool(const std::filesystem::path&)>;

  FileManager1(ShowFolder show_folder, ShowProperties show_properties);
  ~FileManager1();
  FileManager1(const FileManager1&) = delete;
  FileManager1& operator=(const FileManager1&) = delete;

  [[nodiscard]] bool initialize();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace midnight
