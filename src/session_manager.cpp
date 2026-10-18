// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "session_manager.hpp"
#include "session_windows.hpp"
#include "config.hpp"
#include "xsmp_properties.hpp"

#include <X11/ICE/ICEutil.h>
#include <glib.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <iostream>
#include <mutex>
#include <optional>
#include <set>
#include <utility>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

// Xtrans exposes this transport selector through libICE on MidnightBSD/FreeBSD.
// Also verify every resulting listener's address family before advertising it.
extern "C" int _IceTransNoListen(const char* protocol);

#ifndef MIDNIGHT_WM_VERSION
#define MIDNIGHT_WM_VERSION "dev"
#endif

namespace midnight {
namespace {
using Clock = std::chrono::steady_clock;
constexpr auto timeout = std::chrono::seconds(3);
constexpr std::size_t connection_limit = 128;
constexpr std::size_t property_limit = 32;
constexpr std::size_t property_bytes = 65536;
Bool deny_host(char*) { return False; }
void ignore_io_error(IceConn) {}
struct PropertyFree { void operator()(SmProp* property) const { SmFreeProperty(property); } };
using Property = std::unique_ptr<SmProp, PropertyFree>;

// Socket timeouts restart after each successful partial read. A trickling peer
// therefore also needs a wall-clock deadline for the entire libICE call. This
// worker only shuts down the active socket; all SM/ICE calls stay on one thread.
class MessageDeadline {
public:
  MessageDeadline() : worker_([this] {
    std::unique_lock lock(mutex_);
    while (!stopping_) {
      if (fd_ < 0) { condition_.wait(lock, [&] { return stopping_ || fd_ >= 0; }); continue; }
      const auto generation = generation_;
      if (condition_.wait_until(lock, deadline_, [&] { return stopping_ || generation_ != generation; })) continue;
      // Keep the lock through shutdown so the owner cannot clear/close/reuse
      // this descriptor between the deadline check and the system call.
      if (fd_ >= 0) { (void)shutdown(fd_, SHUT_RDWR); fd_ = -1; ++generation_; }
    }
  }) {}
  ~MessageDeadline() {
    { std::lock_guard lock(mutex_); stopping_ = true; }
    condition_.notify_one();
    worker_.join();
  }
  IceProcessMessagesStatus process(IceConn connection) {
    {
      std::lock_guard lock(mutex_);
      fd_ = IceConnectionNumber(connection);
      deadline_ = Clock::now() + std::chrono::milliseconds(250);
      ++generation_;
    }
    condition_.notify_one();
    const auto result = IceProcessMessages(connection, nullptr, nullptr);
    { std::lock_guard lock(mutex_); fd_ = -1; ++generation_; }
    condition_.notify_one();
    return result;
  }
private:
  std::mutex mutex_;
  std::condition_variable condition_;
  int fd_{-1};
  bool stopping_{};
  unsigned long generation_{};
  Clock::time_point deadline_{};
  std::thread worker_;
};

std::optional<std::size_t> size_of(const SmProp* property) {
  if (!property || !property->name || !property->type || !*property->name || !*property->type ||
      strnlen(property->name, 129) > 128 || strnlen(property->type, 33) > 32 ||
      property->num_vals < 0 || property->num_vals > 128 || (property->num_vals && !property->vals))
    return std::nullopt;
  std::size_t size = std::strlen(property->name) + std::strlen(property->type) + 2;
  for (int i = 0; i < property->num_vals; ++i) {
    const auto& value = property->vals[i];
    if (value.length < 0 || value.length > 8192 || (value.length && !value.value)) return std::nullopt;
    size += static_cast<std::size_t>(value.length) + 1;
    if (size > property_bytes) return std::nullopt;
  }
  return size;
}
} // namespace

struct SessionManager::Impl {
  struct Peer {
    IceConn ice{};
    Clock::time_point connected{Clock::now()};
    bool close{};
  };
  struct Client {
    explicit Client(SmsConn c, std::string id) : connection(c), properties(std::move(id)) {}
    SmsConn connection;
    XsmpProperties properties;
    // XSMP requires unknown properties to be returned to the client. Retain
    // these only in bounded memory; the restart allowlist is separate.
    std::map<std::string, Property> raw;
    std::optional<ApplicationRestart> completed;
    Clock::time_point deadline{}, last_request{};
    bool logout_participant{}, interaction_requested{};
    bool registered{}, closing{}, waiting{}, phase2{}, phase2_sent{}, needs_complete{}, in_checkpoint{}, timed_out{};
  };
  struct Auth {
    std::string protocol, network;
    std::array<char, 32> cookie{};
  };
  std::vector<Peer> peers;
  std::map<SmsConn, std::unique_ptr<Client>> clients;
  int listener_count{};
  IceListenObj* listeners{};
  std::vector<Auth> authentication;
  std::filesystem::path directory, authority, restore_plan;
  std::string old_authority;
  bool had_authority{}, published{};
  Clock::time_point last_checkpoint{};
  IceIOErrorHandler old_io_handler{};
  MessageDeadline message_deadline;
  std::size_t next_peer{};
  ApplicationSessionStore store;
  std::unique_ptr<SessionWindowChannel> window_channel;
  std::map<std::string, ApplicationRestart> retired;
  enum class Logout { idle, preparing, saving, capture, dying, complete };
  Logout logout{Logout::idle};
  SmsConn interacting{};
  std::deque<SmsConn> interaction_queue;
  Clock::time_point logout_deadline{};
  bool saving{}, checkpoint_ok{true};
  Clock::time_point checkpoint_deadline{}, restore_deadline{};
  std::set<std::string> restore_ids;

  ~Impl() {
    for (auto& [connection, client] : clients) SmsCleanUp(connection);
    clients.clear();
    for (const auto& peer : peers) IceCloseConnection(peer.ice);
    if (listeners) IceFreeListenObjs(listener_count, listeners);
    if (published) {
      g_unsetenv("SESSION_MANAGER");
      g_unsetenv("MIDNIGHT_WM_RESTORE_PLAN");
      g_unsetenv("MIDNIGHT_WM_RESTORE_MANAGER");
      if (had_authority) g_setenv("ICEAUTHORITY", old_authority.c_str(), true);
      else g_unsetenv("ICEAUTHORITY");
    }
    if (old_io_handler) IceSetIOErrorHandler(old_io_handler);
    window_channel.reset();
    if (!restore_plan.empty()) unlink(restore_plan.c_str());
    if (!authority.empty()) unlink(authority.c_str());
    if (!directory.empty()) rmdir(directory.c_str());
  }

  Client* find(SmsConn connection) {
    const auto found = clients.find(connection);
    return found == clients.end() ? nullptr : found->second.get();
  }

  void begin(Client& client, int save_type = SmSaveLocal) {
    if (!client.registered || client.closing || client.waiting) return;
    client.waiting = true;
    client.phase2 = client.phase2_sent = false;
    client.needs_complete = true;
    client.deadline = Clock::now() + timeout;
    const bool shutdown = logout == Logout::saving;
    SmsSaveYourself(client.connection, save_type, shutdown,
                   shutdown ? SmInteractStyleAny : SmInteractStyleNone, !shutdown);
  }

  static Status registered(SmsConn connection, SmPointer data, char* previous_id) {
    auto& self = *static_cast<Impl*>(data);
    auto* client = self.find(connection);
    if (!client || client->registered) { std::free(previous_id); return 0; }
    if (self.logout != Logout::idle) {
      client->closing = true;
      std::free(previous_id);
      return 0;
    }
    std::string assigned;
    if (previous_id) {
      if (strnlen(previous_id, 257) <= 256 && Clock::now() < self.restore_deadline &&
          self.restore_ids.erase(previous_id)) assigned = previous_id;
      std::free(previous_id);
      // Unknown, duplicate or expired IDs retry as fresh clients via SMlib.
      if (assigned.empty()) return 0;
    }
    if (assigned.empty()) {
      char* generated = g_uuid_string_random();
      assigned = generated;
      g_free(generated);
    }
    client->properties = XsmpProperties(assigned);
    const auto result = SmsRegisterClientReply(connection, assigned.data());
    if (!result) { client->closing = true; return 0; }
    client->registered = true;
    self.begin(*client);
    return 1;
  }

  static void save_request(SmsConn connection, SmPointer data, int save_type,
      Bool shutdown, int, Bool, Bool global) {
    auto& self = *static_cast<Impl*>(data);
    auto* client = self.find(connection);
    if (!client || !client->registered || client->closing) return;
    // Only the owned WM's confirmed exit action may initiate shutdown.
    // Application requests cannot terminate the desktop or bypass confirmation.
    if (self.logout != Logout::idle || shutdown || (save_type != SmSaveLocal && save_type != SmSaveGlobal && save_type != SmSaveBoth)) return;
    const auto now = Clock::now();
    if (now - client->last_request < std::chrono::seconds(1)) return;
    client->last_request = now;
    if (global) self.checkpoint();
    else self.begin(*client, save_type);
  }

  static void phase2_request(SmsConn connection, SmPointer data) {
    if (auto* client = static_cast<Impl*>(data)->find(connection); client && client->waiting)
      client->phase2 = true;
  }

  static void save_done(SmsConn connection, SmPointer data, Bool success) {
    if (auto* client = static_cast<Impl*>(data)->find(connection); client && client->waiting) {
      auto& self = *static_cast<Impl*>(data);
      if (Clock::now() >= client->deadline ||
          (client->logout_participant && Clock::now() >= self.logout_deadline)) {
        client->waiting = false;
        client->timed_out = true;
        client->completed.reset();
        if (client->in_checkpoint) self.checkpoint_ok = false;
        if (client->logout_participant) self.cancel_logout("Application save reply arrived after its deadline");
        return;
      }
      client->waiting = false;
      client->completed = success ? client->properties.restart() : std::nullopt;
      if (!success && client->in_checkpoint) self.checkpoint_ok = false;
      if (!success && client->logout_participant) self.cancel_logout("Application could not save");
    }
  }

  static void interact_request(SmsConn connection, SmPointer data, int dialog) {
    auto& self = *static_cast<Impl*>(data);
    auto* client = self.find(connection);
    if (self.logout != Logout::saving || !client || !client->waiting ||
        !client->logout_participant || Clock::now() >= client->deadline ||
        client->interaction_requested ||
        (dialog != SmDialogNormal && dialog != SmDialogError)) return;
    self.interaction_queue.push_back(connection);
    client->interaction_requested = true;
    client->deadline = self.logout_deadline;
  }

  static void interact_done(SmsConn connection, SmPointer data, Bool cancel) {
    auto& self = *static_cast<Impl*>(data);
    if (self.logout != Logout::saving || self.interacting != connection) return;
    if (Clock::now() >= self.logout_deadline) { self.cancel_logout("Application interaction timed out"); return; }
    self.interacting = nullptr;
    if (auto* client = self.find(connection)) {
      client->interaction_requested = false;
      client->deadline = Clock::now() + timeout;
    }
    if (cancel) self.cancel_logout("Application cancelled logout");
  }

  void cancel_logout(const char* reason) {
    if (logout == Logout::idle || logout == Logout::complete || logout == Logout::dying) return;
    std::cerr << "Logout cancelled: " << reason << '\n';
    for (auto& [connection, client] : clients) {
      if (client->logout_participant) SmsShutdownCancelled(connection);
      client->logout_participant = client->interaction_requested = client->in_checkpoint = false;
      // Preserve outstanding waits: XSMP has no request IDs. Do not let a late
      // SaveYourselfDone satisfy a later save or shutdown transaction.
      if (client->waiting) client->deadline = Clock::now() + timeout;
    }
    interacting = nullptr;
    interaction_queue.clear();
    saving = false;
    logout = Logout::idle;
    window_channel->logout_reply(false);
  }

  void advance_logout() {
    const auto now = Clock::now();
    if (window_channel->take_logout_request() && logout == Logout::idle) {
      logout = Logout::preparing;
      logout_deadline = now + std::chrono::seconds(60);
    }
    if (logout == Logout::idle || logout == Logout::complete) return;
    if (logout != Logout::dying && (window_channel->logout_aborted() || now >= logout_deadline)) {
      cancel_logout("Session request expired or disconnected");
      return;
    }
    if (logout == Logout::preparing) {
      if (pending() || saving) return;
      if (std::ranges::any_of(clients, [](const auto& item) { return item.second->timed_out; })) {
        cancel_logout("A timed-out application must reconnect before logout");
        return;
      }
      logout = Logout::saving;
      checkpoint_ok = true;
      saving = restore_ids.empty() && load_config().save_application_checkpoints;
      for (auto& [connection, client] : clients) {
        if (!client->registered || client->closing) continue;
        client->logout_participant = true;
        client->in_checkpoint = saving;
        client->completed.reset();
        begin(*client, SmSaveBoth);
      }
    }
    if (logout == Logout::saving) {
      while (!interacting && !interaction_queue.empty()) {
        const auto connection = interaction_queue.front();
        interaction_queue.pop_front();
        auto* client = find(connection);
        if (client && client->interaction_requested && client->waiting && !client->closing) {
          interacting = connection;
          SmsInteract(connection);
        }
      }
      if (pending() || interacting) return;
      logout = Logout::capture;
      checkpoint_deadline = now + timeout;
      if (saving) window_channel->request();
    }
    if (logout == Logout::capture) {
      finish_checkpoint();
      if (saving) return;
      if (!checkpoint_ok) { cancel_logout("Final checkpoint failed"); return; }
      // Only successful saves reach Die. Never signal registered processes.
      for (auto& [connection, client] : clients) {
        if (client->logout_participant && !client->closing) SmsDie(connection);
        client->needs_complete = false;
      }
      logout = Logout::dying;
      logout_deadline = now + timeout;
    }
    if (logout == Logout::dying) {
      const bool remaining = std::ranges::any_of(clients, [](const auto& item) {
        return item.second->logout_participant && !item.second->closing;
      });
      if (remaining && now < logout_deadline) return;
      logout = Logout::complete;
      window_channel->logout_reply(true);
    }
  }

  static void close_client(SmsConn connection, SmPointer data, int count, char** reasons) {
    SmFreeReasons(count, reasons);
    if (auto* client = static_cast<Impl*>(data)->find(connection)) client->closing = true;
  }

  static void set_properties(SmsConn connection, SmPointer data, int count, SmProp** properties) {
    auto* client = static_cast<Impl*>(data)->find(connection);
    bool valid = client && client->registered && !client->closing && count >= 0 &&
                 count <= static_cast<int>(property_limit) && (!count || properties);
    std::map<std::string, std::size_t> sizes;
    if (valid) {
      for (const auto& [name, property] : client->raw) sizes[name] = *size_of(property.get());
      for (int i = 0; i < count; ++i) {
        const auto size = size_of(properties[i]);
        if (!size) { valid = false; break; }
        sizes[properties[i]->name] = *size;
      }
      std::size_t bytes = 0;
      for (const auto& [name, size] : sizes) bytes += size;
      valid = valid && sizes.size() <= property_limit && bytes <= property_bytes;
      if (valid) valid = bool(client->properties.update({properties, static_cast<std::size_t>(count)}));
    }
    // Library allocations are released on both rejection and acceptance. Move
    // accepted properties into the bounded protocol store without borrowing.
    if (properties) {
      for (int i = 0; i < count; ++i) {
        if (valid) client->raw[properties[i]->name].reset(properties[i]);
        else if (properties[i]) SmFreeProperty(properties[i]);
      }
      std::free(properties);
    }
  }

  static void delete_properties(SmsConn connection, SmPointer data, int count, char** names) {
    auto* client = static_cast<Impl*>(data)->find(connection);
    std::vector<std::string> removed;
    bool valid = client && count >= 0 && count <= static_cast<int>(property_limit) && (!count || names);
    if (names) {
      for (int i = 0; i < count; ++i) {
        if (!names[i] || strnlen(names[i], 129) > 128) valid = false;
        else if (valid) removed.emplace_back(names[i]);
        std::free(names[i]);
      }
      std::free(names);
    }
    if (valid) {
      client->properties.remove(removed);
      for (const auto& name : removed) client->raw.erase(name);
      // Never retain a now-deleted command as a completed checkpoint.
      client->completed.reset();
      if (client->in_checkpoint) static_cast<Impl*>(data)->checkpoint_ok = false;
    }
  }

  static void get_properties(SmsConn connection, SmPointer data) {
    auto* client = static_cast<Impl*>(data)->find(connection);
    if (!client || !client->registered || client->closing) return;
    std::vector<SmProp*> properties;
    for (const auto& [name, property] : client->raw) properties.push_back(property.get());
    SmsReturnProperties(connection, static_cast<int>(properties.size()), properties.data());
  }

  static Status new_client(SmsConn connection, SmPointer data, unsigned long* mask,
                            SmsCallbacks* callbacks, char** reason) {
    auto& self = *static_cast<Impl*>(data);
    if (self.logout != Logout::idle || self.clients.size() + self.retired.size() >= connection_limit || self.clients.contains(connection)) {
      *reason = strdup("Session client limit reached");
      return 0;
    }
    self.clients.emplace(connection, std::make_unique<Client>(connection, ""));
    *mask = SmsRegisterClientProcMask | SmsInteractRequestProcMask | SmsInteractDoneProcMask |
      SmsSaveYourselfRequestProcMask | SmsSaveYourselfP2RequestProcMask | SmsSaveYourselfDoneProcMask |
      SmsCloseConnectionProcMask | SmsSetPropertiesProcMask | SmsDeletePropertiesProcMask | SmsGetPropertiesProcMask;
    callbacks->register_client = {registered, data};
    callbacks->interact_request = {interact_request, data};
    callbacks->interact_done = {interact_done, data};
    callbacks->save_yourself_request = {save_request, data};
    callbacks->save_yourself_phase2_request = {phase2_request, data};
    callbacks->save_yourself_done = {save_done, data};
    callbacks->close_connection = {close_client, data};
    callbacks->set_properties = {set_properties, data};
    callbacks->delete_properties = {delete_properties, data};
    callbacks->get_properties = {get_properties, data};
    return 1;
  }

  std::expected<void, std::string> initialize() {
    if (const char* inherited = std::getenv("SESSION_MANAGER"); inherited && *inherited)
      return std::unexpected("An inherited session manager must be preserved");
    char error[256]{};
    if (!SmsInitialize("MidnightBSD", MIDNIGHT_WM_VERSION, new_client, this, deny_host, sizeof(error), error))
      return std::unexpected(error);
    if (_IceTransNoListen("tcp") != 0) return std::unexpected("Cannot disable ICE TCP transport");
    (void)_IceTransNoListen("inet");
    (void)_IceTransNoListen("inet6");
    if (!IceListenForConnections(&listener_count, &listeners, sizeof(error), error)) return std::unexpected(error);
    for (int i = 0; i < listener_count; ++i) {
      const int fd = IceGetListenConnectionNumber(listeners[i]);
      sockaddr_storage address{};
      socklen_t length = sizeof(address);
      if (getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0 || address.ss_family != AF_UNIX)
        return std::unexpected("Refusing a non-local ICE listener");
      if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) return std::unexpected(std::strerror(errno));
      IceSetHostBasedAuthProc(listeners[i], deny_host);
    }
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    struct stat st{};
    std::filesystem::path base = "/tmp";
    if (runtime && std::filesystem::path(runtime).is_absolute() && lstat(runtime, &st) == 0 &&
        S_ISDIR(st.st_mode) && st.st_uid == getuid() && !(st.st_mode & 0077)) base = runtime;
    std::string pattern = (base / "midnight-session-XXXXXX").string();
    if (!mkdtemp(pattern.data())) return std::unexpected(std::strerror(errno));
    directory = pattern;
    authority = directory / "ICEauthority";
    const int fd = open(authority.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return std::unexpected(std::strerror(errno));
    FILE* file = fdopen(fd, "wb");
    if (!file) { close(fd); return std::unexpected(std::strerror(errno)); }
    authentication.reserve(static_cast<std::size_t>(listener_count) * 2);
    for (int i = 0; i < listener_count; ++i) {
      char* network = IceGetListenConnectionString(listeners[i]);
      if (!network) { fclose(file); return std::unexpected("Cannot identify ICE listener"); }
      for (const char* protocol : {"ICE", "XSMP"}) {
        Auth auth;
        auth.protocol = protocol;
        auth.network = network;
        arc4random_buf(auth.cookie.data(), auth.cookie.size());
        authentication.push_back(std::move(auth));
      }
      std::free(network);
    }
    std::vector<IceAuthDataEntry> entries;
    bool written = true;
    for (auto& auth : authentication) {
      char auth_name[] = "MIT-MAGIC-COOKIE-1";
      IceAuthFileEntry entry{auth.protocol.data(), 0, nullptr, auth.network.data(), auth_name,
        static_cast<unsigned short>(auth.cookie.size()), auth.cookie.data()};
      if (!IceWriteAuthFileEntry(file, &entry)) written = false;
      entries.push_back({auth.protocol.data(), auth.network.data(), const_cast<char*>("MIT-MAGIC-COOKIE-1"),
        static_cast<unsigned short>(auth.cookie.size()), auth.cookie.data()});
    }
    if (fclose(file) != 0) written = false;
    if (!written) return std::unexpected("Cannot write private ICE authority");
    IceSetPaAuthData(static_cast<int>(entries.size()), entries.data());
    char* network_ids = IceComposeNetworkIdList(listener_count, listeners);
    if (!network_ids) return std::unexpected("Cannot compose ICE listener addresses");
    if (const char* old = std::getenv("ICEAUTHORITY")) { old_authority = old; had_authority = true; }
    g_setenv("ICEAUTHORITY", authority.c_str(), true);
    g_setenv("SESSION_MANAGER", network_ids, true);
    std::free(network_ids);
    published = true;
    old_io_handler = IceSetIOErrorHandler(ignore_io_error);
    window_channel = std::make_unique<SessionWindowChannel>();
    if (auto initialized = window_channel->initialize(directory); !initialized) return initialized;
    if (load_config().restore_applications) {
      auto saved = store.load();
      if (!saved) std::cerr << "Application restore skipped: " << saved.error() << '\n';
      else if (*saved && !(**saved).applications.empty()) {
        restore_plan = directory / "restore.ini";
        if (auto copied = ApplicationSessionStore(restore_plan).save(**saved); !copied)
          std::cerr << "Application restore skipped: " << copied.error() << '\n';
        else {
          for (const auto& application : (**saved).applications) restore_ids.insert(application.client_id);
          restore_deadline = Clock::now() + std::chrono::seconds(120);
          g_setenv("MIDNIGHT_WM_RESTORE_PLAN", restore_plan.c_str(), true);
          g_setenv("MIDNIGHT_WM_RESTORE_MANAGER", g_getenv("SESSION_MANAGER"), true);
        }
      }
    }
    return {};
  }

  void checkpoint(bool persist = true, bool final_checkpoint = false) {
    const auto now = Clock::now();
    // Do not mix an in-flight initial/local checkpoint with a new global one.
    if (logout != Logout::idle || pending() || saving || (!final_checkpoint && now - last_checkpoint < std::chrono::seconds(1))) return;
    last_checkpoint = now;
    // Preserve the source checkpoint while saved clients are still expected.
    saving = persist && restore_ids.empty() && load_config().save_application_checkpoints;
    // XSMP has no checkpoint sequence number. A late SaveYourselfDone from a
    // timed-out save cannot safely authorize a later persisted checkpoint.
    checkpoint_ok = std::ranges::none_of(clients, [](const auto& item) {
      return item.second->timed_out;
    });
    checkpoint_deadline = now + timeout;
    if (saving) window_channel->request();
    for (auto& [connection, client] : clients) {
      if (!client->registered || client->closing) continue;
      client->in_checkpoint = saving;
      client->completed.reset();
      begin(*client, SmSaveBoth);
    }
  }

  void finish_checkpoint() {
    if (!saving || logout == Logout::saving) return;
    if (window_channel->pending()) return;
    const bool waiting = std::ranges::any_of(clients, [](const auto& item) {
      return item.second->in_checkpoint && item.second->waiting;
    });
    if (waiting && Clock::now() < checkpoint_deadline) return;
    if (waiting) checkpoint_ok = false;
    ApplicationCheckpoint checkpoint;
    for (const auto& [id, application] : retired) checkpoint.applications.push_back(application);
    for (auto& [connection, client] : clients) {
      if (client->in_checkpoint && client->completed) checkpoint.applications.push_back(*client->completed);
      client->in_checkpoint = false;
    }
    if (window_channel->attached()) {
      auto windows = window_channel->result();
      if (!windows) checkpoint_ok = false;
      else {
        std::set<std::string> ids;
        for (const auto& application : checkpoint.applications) ids.insert(application.client_id);
        for (auto& window : *windows)
          if (ids.contains(window.client_id)) checkpoint.windows.push_back(std::move(window));
      }
    }
    if (!checkpoint_ok) std::cerr << "Application checkpoint not saved: incomplete application or window save\n";
    else if (auto saved = store.save(checkpoint); !saved) {
      checkpoint_ok = false;
      std::cerr << "Application checkpoint not saved: " << saved.error() << '\n';
    }
    saving = false;
  }

  bool pending() const {
    return std::ranges::any_of(clients, [](const auto& item) { return item.second->waiting; });
  }

  void tick(int timeout_ms) {
    window_channel->tick();
    if (Clock::now() >= restore_deadline) restore_ids.clear();
    std::vector<pollfd> descriptors;
    const auto old_peer_count = peers.size();
    for (int i = 0; i < listener_count; ++i)
      descriptors.push_back({IceGetListenConnectionNumber(listeners[i]), POLLIN, 0});
    for (const auto& peer : peers) descriptors.push_back({IceConnectionNumber(peer.ice), POLLIN, 0});
    if (poll(descriptors.data(), descriptors.size(), std::clamp(timeout_ms, 0, 100)) < 0 && errno != EINTR) return;
    for (int i = 0; i < listener_count; ++i) {
      if (!(descriptors[i].revents & POLLIN)) continue;
      IceAcceptStatus status{};
      IceConn ice = IceAcceptConnection(listeners[i], &status);
      if (!ice) continue;
      IceSetShutdownNegotiation(ice, False);
      const int fd = IceConnectionNumber(ice);
      // libICE can read an incomplete frame after poll reports a header byte.
      // Bound that read/write in this separate process, including pre-auth peers.
      const timeval limit{0, 250000};
      const bool configured = fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 &&
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit)) == 0 &&
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit)) == 0;
      if (status != IceAcceptSuccess || !configured || peers.size() >= connection_limit) {
        IceCloseConnection(ice);
        continue;
      }
      peers.push_back({ice});
    }
    std::size_t processed = 0;
    const auto first = old_peer_count ? next_peer % old_peer_count : 0;
    for (std::size_t offset = 0; offset < old_peer_count; ++offset) {
      const auto i = (first + offset) % old_peer_count;
      auto& peer = peers[i];
      const auto events = descriptors[static_cast<std::size_t>(listener_count) + i].revents;
      if ((events & POLLIN) && processed < 4) {
        ++processed;
        next_peer = i + 1;
        const auto status = message_deadline.process(peer.ice);
        if (status != IceProcessMessagesSuccess) peer.close = true;
      }
      if (events & (POLLERR | POLLHUP | POLLNVAL)) peer.close = true;
      const auto status = IceConnectionStatus(peer.ice);
      if (status == IceConnectRejected || status == IceConnectIOError) peer.close = true;
      const bool registered_client = std::ranges::any_of(clients, [&](const auto& item) {
        return SmsGetIceConnection(item.first) == peer.ice && item.second->registered;
      });
      if (!registered_client && Clock::now() - peer.connected > timeout) peer.close = true;
      for (const auto& [connection, client] : clients)
        if (SmsGetIceConnection(connection) == peer.ice && client->closing) peer.close = true;
    }
    for (auto it = peers.begin(); it != peers.end();) {
      if (!it->close) { ++it; continue; }
      for (auto client = clients.begin(); client != clients.end();) {
        if (SmsGetIceConnection(client->first) != it->ice) { ++client; continue; }
        auto& state = *client->second;
        if (interacting == client->first) interacting = nullptr;
        if (state.logout_participant && logout != Logout::dying)
          cancel_logout("Application disconnected during save");
        if (state.waiting && state.in_checkpoint) checkpoint_ok = false;
        // RestartAnyway survives a clean or abrupt client exit. Other styles
        // are retained only while their client is running; no immediate restart
        // is attempted in this persistence-only phase.
        if (!state.timed_out && state.completed && state.completed->style == RestartStyle::anyway)
          retired[state.completed->client_id] = *state.completed;
        SmsCleanUp(client->first);
        client = clients.erase(client);
      }
      IceCloseConnection(it->ice);
      it = peers.erase(it);
    }
    const auto now = Clock::now();
    for (auto& [connection, client] : clients)
      if (client->waiting && now >= client->deadline) {
        client->waiting = false;
        client->timed_out = true;
        client->completed.reset();
        if (client->in_checkpoint) checkpoint_ok = false;
        if (client->logout_participant) cancel_logout("Application save timed out");
      }
    const bool phase1_done = std::ranges::none_of(clients, [](const auto& item) {
      return item.second->waiting && !item.second->phase2;
    });
    if (phase1_done) for (auto& [connection, client] : clients) {
      if (client->waiting && client->phase2 && !client->phase2_sent) {
        client->phase2_sent = true;
        SmsSaveYourselfPhase2(connection);
      }
    }
    advance_logout();
    finish_checkpoint();
    if (logout == Logout::idle && !pending()) for (auto& [connection, client] : clients) {
      if (client->needs_complete) { SmsSaveComplete(connection); client->needs_complete = false; }
    }
  }
};

SessionManager::SessionManager() : impl_(std::make_unique<Impl>()) {}
SessionManager::~SessionManager() = default;
std::expected<void, std::string> SessionManager::initialize() { return impl_->initialize(); }
int SessionManager::release_window_child_fd() { return impl_->window_channel->release_child_fd(); }
void SessionManager::tick(int timeout_ms) { impl_->tick(timeout_ms); }
bool SessionManager::logout_completed() const { return impl_->logout == Impl::Logout::complete; }
void SessionManager::checkpoint(bool persist, bool final_checkpoint) { impl_->checkpoint(persist, final_checkpoint); }
bool SessionManager::checkpoint_pending() const { return impl_->pending() || impl_->saving; }
} // namespace midnight
