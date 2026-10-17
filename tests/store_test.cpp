#include "store.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

int main() {
  char directory[] = "/tmp/midnight-wm-test-XXXXXX";
  if (!mkdtemp(directory)) return 1;
  const std::filesystem::path root(directory);
  {
    midnight::Store store(root / "workspace.db");
    assert(store.open());
    midnight::Object folder{midnight::Store::new_id(), midnight::ObjectKind::folder,
                            "Projects", (root / "Projects").string(), "folder", "desktop", 50, 80};
    assert(store.put(folder));
    auto result = store.get(folder.id);
    assert(result && result->title == "Projects" && result->x == 50);
    folder.title = "Work";
    assert(store.put(folder));
    assert(store.children("desktop").size() == 1);
    assert(store.get(folder.id)->title == "Work");
    assert(store.erase(folder.id));
    assert(store.children("desktop").empty());
  }
  {
    midnight::Store reopened(root / "workspace.db");
    assert(reopened.open());
    assert(reopened.children("desktop").empty());
  }
  {
    std::ofstream(root / "source.txt") << "clipboard data";
    assert(midnight::copy_file(root / "source.txt", root / "copy.txt"));
    assert(midnight::move_file(root / "copy.txt", root / "moved.txt"));
    assert(!std::filesystem::exists(root / "copy.txt"));
    assert(std::filesystem::exists(root / "moved.txt"));
    const auto uri = midnight::file_uri(root / "moved.txt");
    auto path = midnight::local_path_from_uri(uri);
    assert(path && *path == root / "moved.txt");
  }
  std::filesystem::remove_all(root);
  std::cout << "store tests passed\n";
}
