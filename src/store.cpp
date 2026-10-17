#include "store.hpp"

#include <gio/gio.h>
#include <glib.h>
#include <sqlite3.h>

#include <array>
#include <cstdlib>
#include <memory>
#include <random>
#include <stdexcept>
#include <system_error>

namespace midnight {
namespace {
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

std::string error_text(GError* error) {
  std::string text = error ? error->message : "unknown GIO error";
  if (error) g_error_free(error);
  return text;
}

std::expected<void, std::string> file_op(const std::filesystem::path& from,
                                         const std::filesystem::path* to, bool move, bool trash) {
  auto* source = g_file_new_for_path(from.c_str());
  GError* error = nullptr;
  gboolean ok = FALSE;
  if (trash) {
    ok = g_file_trash(source, nullptr, &error);
  } else {
    auto* destination = g_file_new_for_path(to->c_str());
    if (move) ok = g_file_move(source, destination, G_FILE_COPY_NOFOLLOW_SYMLINKS, nullptr, nullptr, nullptr, &error);
    else ok = g_file_copy(source, destination, G_FILE_COPY_NOFOLLOW_SYMLINKS, nullptr, nullptr, nullptr, &error);
    g_object_unref(destination);
  }
  g_object_unref(source);
  if (!ok) return std::unexpected(error_text(error));
  return {};
}

Object row_object(sqlite3_stmt* statement) {
  auto str = [statement](int column) -> std::string {
    const auto* value = sqlite3_column_text(statement, column);
    return value ? reinterpret_cast<const char*>(value) : "";
  };
  return {str(0), static_cast<ObjectKind>(sqlite3_column_int(statement, 1)),
          str(2), str(3), str(4), str(5), sqlite3_column_int(statement, 6), sqlite3_column_int(statement, 7)};
}
} // namespace

Store::Store(std::filesystem::path db_path) : path_(std::move(db_path)) {}
Store::~Store() { if (db_) sqlite3_close(db_); }

std::expected<void, std::string> Store::open() {
  std::error_code ec;
  std::filesystem::create_directories(path_.parent_path(), ec);
  if (ec) return std::unexpected(ec.message());
  if (sqlite3_open(path_.c_str(), &db_) != SQLITE_OK) return std::unexpected(sqlite3_errmsg(db_));
  constexpr auto schema =
    "CREATE TABLE IF NOT EXISTS objects ("
    "id TEXT PRIMARY KEY, kind INTEGER NOT NULL, title TEXT NOT NULL, "
    "target TEXT NOT NULL, icon TEXT NOT NULL, parent TEXT NOT NULL, "
    "x INTEGER NOT NULL, y INTEGER NOT NULL);"
    "CREATE INDEX IF NOT EXISTS objects_parent ON objects(parent);";
  char* error = nullptr;
  if (sqlite3_exec(db_, schema, nullptr, nullptr, &error) != SQLITE_OK) {
    std::string message = error ? error : "database initialization failed";
    sqlite3_free(error);
    return std::unexpected(message);
  }
  return {};
}

std::expected<void, std::string> Store::put(const Object& object) {
  if (!db_) return std::unexpected("database is not open");
  sqlite3_stmt* raw{};
  constexpr auto sql = "INSERT INTO objects VALUES(?,?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET "
    "kind=excluded.kind,title=excluded.title,target=excluded.target,icon=excluded.icon,"
    "parent=excluded.parent,x=excluded.x,y=excluded.y";
  if (sqlite3_prepare_v2(db_, sql, -1, &raw, nullptr) != SQLITE_OK)
    return std::unexpected(sqlite3_errmsg(db_));
  Statement statement(raw, sqlite3_finalize);
  const std::array fields{object.id, object.title, object.target, object.icon, object.parent};
  sqlite3_bind_text(raw, 1, fields[0].c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(raw, 2, static_cast<int>(object.kind));
  for (int i = 1; i < 5; ++i) sqlite3_bind_text(raw, i + 2, fields[i].c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(raw, 7, object.x);
  sqlite3_bind_int(raw, 8, object.y);
  if (sqlite3_step(raw) != SQLITE_DONE) return std::unexpected(sqlite3_errmsg(db_));
  return {};
}

std::expected<void, std::string> Store::erase(const std::string& id) {
  if (!db_) return std::unexpected("database is not open");
  sqlite3_stmt* raw{};
  if (sqlite3_prepare_v2(db_, "DELETE FROM objects WHERE id=?", -1, &raw, nullptr) != SQLITE_OK)
    return std::unexpected(sqlite3_errmsg(db_));
  Statement statement(raw, sqlite3_finalize);
  sqlite3_bind_text(raw, 1, id.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(raw) != SQLITE_DONE) return std::unexpected(sqlite3_errmsg(db_));
  return {};
}

std::vector<Object> Store::children(const std::string& parent) const {
  std::vector<Object> objects;
  if (!db_) return objects;
  sqlite3_stmt* raw{};
  if (sqlite3_prepare_v2(db_, "SELECT * FROM objects WHERE parent=? ORDER BY title", -1, &raw, nullptr) != SQLITE_OK)
    return objects;
  Statement statement(raw, sqlite3_finalize);
  sqlite3_bind_text(raw, 1, parent.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(raw) == SQLITE_ROW) objects.push_back(row_object(raw));
  return objects;
}

std::expected<Object, std::string> Store::get(const std::string& id) const {
  if (!db_) return std::unexpected("database is not open");
  sqlite3_stmt* raw{};
  if (sqlite3_prepare_v2(db_, "SELECT * FROM objects WHERE id=?", -1, &raw, nullptr) != SQLITE_OK)
    return std::unexpected(sqlite3_errmsg(db_));
  Statement statement(raw, sqlite3_finalize);
  sqlite3_bind_text(raw, 1, id.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(raw) != SQLITE_ROW) return std::unexpected("object not found");
  return row_object(raw);
}

std::string Store::new_id() {
  gchar* raw = g_uuid_string_random();
  std::string id = raw ? raw : "";
  g_free(raw);
  return id;
}

std::filesystem::path Store::data_path() {
  return std::filesystem::path(g_get_user_data_dir()) / "midnight-wm";
}

std::filesystem::path Store::desktop_path() {
  const char* desktop = g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP);
  return desktop ? std::filesystem::path(desktop) : std::filesystem::path(g_get_home_dir()) / "Desktop";
}

std::expected<void, std::string> copy_file(const std::filesystem::path& from,
                                           const std::filesystem::path& to) {
  return file_op(from, &to, false, false);
}
std::expected<void, std::string> move_file(const std::filesystem::path& from,
                                           const std::filesystem::path& to) {
  return file_op(from, &to, true, false);
}
std::expected<void, std::string> trash_file(const std::filesystem::path& path) {
  return file_op(path, nullptr, false, true);
}

std::string file_uri(const std::filesystem::path& path) {
  auto* file = g_file_new_for_path(path.c_str());
  char* uri = g_file_get_uri(file);
  std::string result = uri ? uri : "";
  g_free(uri);
  g_object_unref(file);
  return result;
}

std::expected<std::filesystem::path, std::string> local_path_from_uri(const std::string& uri) {
  auto* file = g_file_new_for_uri(uri.c_str());
  char* path = g_file_get_path(file);
  g_object_unref(file);
  if (!path) return std::unexpected("URI is not a local file");
  std::filesystem::path result(path);
  g_free(path);
  return result;
}
} // namespace midnight
