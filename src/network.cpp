// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
#include "network.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <memory>
#include <utility>

#if defined(__FreeBSD__) || defined(__MidnightBSD__)
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_media.h>
#include <net/if_types.h>
#include <net/ethernet.h>
#include <net/if_lagg.h>
#include <netinet/in.h>
#include <net80211/ieee80211.h>
#include <net80211/ieee80211_ioctl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <unistd.h>
#endif

namespace midnight {
namespace {
#if defined(__FreeBSD__) || defined(__MidnightBSD__)
// SSIDs are arbitrary bytes; escape control/non-ASCII bytes for safe display.
std::string display_ssid(const char* bytes, std::size_t length) {
  constexpr char hex[] = "0123456789ABCDEF";
  std::string result;
  for (std::size_t i = 0; i < length; ++i) {
    const auto c = static_cast<unsigned char>(bytes[i]);
    if (c >= 32 && c < 127 && c != '\\') result += static_cast<char>(c);
    else { result += "\\x"; result += hex[c >> 4]; result += hex[c & 15]; }
  }
  return result;
}

bool wireless_ioctl(int socket, const std::string& name, int type, void* data, std::size_t capacity, std::size_t& length) {
  ieee80211req request{};
  strlcpy(request.i_name, name.c_str(), sizeof(request.i_name));
  request.i_type = type;
  request.i_data = data;
  request.i_len = static_cast<int>(capacity);
  if (ioctl(socket, SIOCG80211, &request) != 0 || request.i_len < 0 ||
      static_cast<std::size_t>(request.i_len) > capacity) return false;
  length = static_cast<std::size_t>(request.i_len);
  return true;
}

void wireless_details(int socket, NetworkInterface& interface, int media) {
  std::array<char, IEEE80211_NWID_LEN> ssid{};
  std::size_t length{};
  if (wireless_ioctl(socket, interface.name, IEEE80211_IOC_SSID, ssid.data(), ssid.size(), length))
    interface.ssid = display_ssid(ssid.data(), length);
  switch (IFM_MODE(media)) {
    case IFM_IEEE80211_11A: interface.standard = "802.11a"; break;
    case IFM_IEEE80211_11B: interface.standard = "802.11b"; break;
    case IFM_IEEE80211_11G: interface.standard = "802.11g"; break;
    case IFM_IEEE80211_11NA:
    case IFM_IEEE80211_11NG: interface.standard = "802.11n (Wi-Fi 4)"; break;
    case IFM_IEEE80211_VHT5G:
    case IFM_IEEE80211_VHT2G: interface.standard = "802.11ac (Wi-Fi 5)"; break;
    default: break;
  }
  // Request only the associated AP, following ifconfig's station query.
  struct StationBuffer { ieee80211req_sta_req station; std::array<std::byte, 24 * 1024> extra; } buffer{};
  auto& station = buffer.station;
  if (!wireless_ioctl(socket, interface.name, IEEE80211_IOC_BSSID,
      station.is_u.macaddr, IEEE80211_ADDR_LEN, length) || length != IEEE80211_ADDR_LEN) return;
  if (!wireless_ioctl(socket, interface.name, IEEE80211_IOC_STA_INFO, &buffer, sizeof(buffer) - offsetof(ieee80211req_sta_req, info), length) ||
      length < sizeof(ieee80211req_sta_info) || station.info[0].isi_len < sizeof(ieee80211req_sta_info) ||
      station.info[0].isi_len > length) return;
  if (station.info[0].isi_txmbps)
    interface.speed_mbps = station.info[0].isi_txmbps / 2.0;
}

std::expected<std::vector<NetworkInterface>, std::string> native_read(bool details) {
  ifaddrs* raw{};
  if (getifaddrs(&raw) != 0) return std::unexpected(std::string(std::strerror(errno)));
  const std::unique_ptr<ifaddrs, decltype(&freeifaddrs)> addresses(raw, freeifaddrs);
  std::map<std::string, NetworkInterface> interfaces;
  std::map<std::string, bool> has_address;
  for (const auto* address = raw; address; address = address->ifa_next) {
    if (!address->ifa_name || !address->ifa_addr || (address->ifa_flags & IFF_LOOPBACK) ||
        !(address->ifa_flags & IFF_UP)) continue;
    const std::string name(address->ifa_name);
    auto& item = interfaces[name];
    item.name = name;
    const int family = address->ifa_addr->sa_family;
    if (family == AF_LINK && address->ifa_data) {
      const auto* data = static_cast<const if_data*>(address->ifa_data);
      item.kind = data->ifi_type == IFT_IEEE80211 ? NetworkKind::wifi : NetworkKind::unknown;
      item.connected = data->ifi_link_state == LINK_STATE_UP ||
          (data->ifi_link_state == LINK_STATE_UNKNOWN && (address->ifa_flags & IFF_RUNNING));
      if (details) { item.received = data->ifi_ibytes; item.transmitted = data->ifi_obytes; }
    } else if (family == AF_INET || family == AF_INET6) {
      bool usable = true;
      if (family == AF_INET) {
        const auto ip = ntohl(reinterpret_cast<const sockaddr_in*>(address->ifa_addr)->sin_addr.s_addr);
        usable = ip != 0 && (ip >> 24) != 127 && (ip >> 16) != 0xa9fe;
      } else {
        const auto& ip = reinterpret_cast<const sockaddr_in6*>(address->ifa_addr)->sin6_addr;
        usable = !IN6_IS_ADDR_UNSPECIFIED(&ip) && !IN6_IS_ADDR_LOOPBACK(&ip) && !IN6_IS_ADDR_LINKLOCAL(&ip);
      }
      has_address[name] = has_address[name] || usable;
      if (details) {
        std::array<char, NI_MAXHOST> host{};
        if (getnameinfo(address->ifa_addr, address->ifa_addr->sa_len,
            host.data(), host.size(), nullptr, 0, NI_NUMERICHOST) == 0 &&
            std::ranges::find(item.addresses, host.data()) == item.addresses.end())
          item.addresses.emplace_back(host.data());
      }
    }
  }
  const int socket = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  for (auto& [name, item] : interfaces) {
    ifmediareq media{};
    strlcpy(media.ifm_name, name.c_str(), sizeof(media.ifm_name));
    if (socket >= 0 && ioctl(socket, SIOCGIFMEDIA, &media) == 0) {
      if (IFM_TYPE(media.ifm_active) == IFM_IEEE80211) item.kind = NetworkKind::wifi;
      else if (IFM_TYPE(media.ifm_active) == IFM_ETHER) item.kind = NetworkKind::wired;
      if (media.ifm_status & IFM_AVALID) item.connected = (media.ifm_status & IFM_ACTIVE) != 0;
      if (details && item.connected && item.kind == NetworkKind::wifi)
        wireless_details(socket, item, media.ifm_active);
    }
    // A virtual interface with no carrier information needs a usable address.
    if (item.kind == NetworkKind::unknown && !has_address[name]) item.connected = false;
  }
  // A failover lagg presents as Ethernet even when its active port is Wi-Fi.
  // Keep its IP/counters, but derive the icon and radio details from active ports.
  if (socket >= 0) for (auto& [name, aggregate] : interfaces) {
    std::array<lagg_reqport, LAGG_MAX_PORTS> ports{};
    lagg_reqall request{};
    strlcpy(request.ra_ifname, name.c_str(), sizeof(request.ra_ifname));
    request.ra_port = ports.data();
    request.ra_size = sizeof(ports);
    if (ioctl(socket, SIOCGLAGG, &request) != 0 || request.ra_ports < 0 ||
        request.ra_ports > static_cast<int>(ports.size())) continue;
    aggregate.kind = NetworkKind::unknown;
    for (int index = 0; index < request.ra_ports; ++index) {
      const std::string port_name(ports[index].rp_portname,
          strnlen(ports[index].rp_portname, sizeof(ports[index].rp_portname)));
      const auto child = interfaces.find(port_name);
      if (child == interfaces.end()) continue;
      child->second.parent = name;
      if (!(ports[index].rp_flags & LAGG_PORT_ACTIVE) || !child->second.connected) continue;
      if (child->second.kind == NetworkKind::wifi && aggregate.kind != NetworkKind::wired) {
        aggregate.kind = NetworkKind::wifi;
        aggregate.ssid = child->second.ssid;
        aggregate.standard = child->second.standard;
        aggregate.speed_mbps = child->second.speed_mbps;
      } else if (child->second.kind == NetworkKind::wired) {
        aggregate.kind = NetworkKind::wired;
        aggregate.ssid.clear(); aggregate.standard.clear(); aggregate.speed_mbps.reset();
      }
    }
  }
  if (socket >= 0) close(socket);
  std::vector<NetworkInterface> result;
  for (auto& [name, item] : interfaces) result.push_back(std::move(item));
  return result;
}
#else
std::expected<std::vector<NetworkInterface>, std::string> native_read(bool) {
  return std::unexpected("Network status is unavailable on this platform");
}
#endif
} // namespace

NetworkMonitor::NetworkMonitor() : reader_(native_read) {}
NetworkMonitor::NetworkMonitor(Reader reader) : reader_(std::move(reader)) {}

bool NetworkMonitor::poll(bool details, bool force, Clock::time_point now) {
  if (!force && last_status_ && now - *last_status_ < std::chrono::seconds(30) &&
      (!details || (last_details_ && now - *last_details_ < std::chrono::seconds(2)))) return false;
  last_status_ = now;
  if (details) last_details_ = now;
  NetworkState next;
  auto result = reader_(details);
  if (!result) { next.kind = NetworkKind::unknown; next.error = result.error(); }
  else {
    std::map<std::string, Clock::time_point> connected;
    for (auto& item : *result) {
      if (!item.connected) continue;
      const auto previous = connected_since_.find(item.name);
      item.connected_since = previous == connected_since_.end() ? now : previous->second;
      connected[item.name] = item.connected_since;
      next.interfaces.push_back(std::move(item));
    }
    connected_since_ = std::move(connected);
    // Prefer an active wired link when both are present; still list all links.
    if (!next.interfaces.empty()) {
      next.kind = NetworkKind::unknown;
      for (const auto& item : next.interfaces) {
        if (!item.parent.empty() && std::ranges::any_of(next.interfaces,
            [&](const auto& parent) { return parent.name == item.parent; })) continue;
        if (item.kind == NetworkKind::wired) { next.kind = NetworkKind::wired; break; }
        if (item.kind == NetworkKind::wifi) next.kind = NetworkKind::wifi;
      }
    }
  }
  const bool changed = next != state_;
  state_ = std::move(next);
  return changed;
}
} // namespace midnight
