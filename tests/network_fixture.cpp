// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause
// Test-only libc interposer. Never linked into or installed with the desktop.
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
#include <array>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>

namespace {
struct Fixture { int mode{}; unsigned long long received{123456}, sent{654321}; };
Fixture read_fixture() {
  Fixture result;
  if (const char* path = std::getenv("MIDNIGHT_NETWORK_FIXTURE"))
    if (auto* file = std::fopen(path, "r")) {
      std::fscanf(file, "%d %llu %llu", &result.mode, &result.received, &result.sent);
      std::fclose(file);
    }
  return result;
}
void log(const char* event) {
  if (const char* path = std::getenv("MIDNIGHT_NETWORK_LOG"))
    if (auto* file = std::fopen(path, "a")) { std::fprintf(file, "%s\n", event); std::fclose(file); }
}
struct Block {
  std::array<ifaddrs, 6> nodes;
  std::array<sockaddr_dl, 3> links;
  std::array<sockaddr_in, 3> ips;
  std::array<if_data, 3> data;
};
bool active(int index, int mode) { return index == 0 ? (mode == 1 || mode == 3 || mode == 4) : index == 1 ? (mode == 2 || mode == 3 || mode == 4) : mode == 4; }
}
extern "C" int getifaddrs(ifaddrs** result) {
  if (!result) { errno = EINVAL; return -1; }
  log("status");
  auto* block = static_cast<Block*>(std::calloc(1, sizeof(Block)));
  if (!block) return -1;
  const auto fixture = read_fixture();
  for (int index = 0; index < 3; ++index) {
    const char* name = index == 0 ? "emtest0" : index == 1 ? "wlantest0" : "laggtest0";
    auto& data = block->data[index];
    data.ifi_type = index == 1 ? IFT_IEEE80211 : IFT_ETHER;
    data.ifi_link_state = active(index, fixture.mode) ? LINK_STATE_UP : LINK_STATE_DOWN;
    data.ifi_ibytes = fixture.received;
    data.ifi_obytes = fixture.sent;
    auto& link = block->links[index];
    link.sdl_family = AF_LINK; link.sdl_len = sizeof(link);
    auto& ip = block->ips[index];
    ip.sin_family = AF_INET; ip.sin_len = sizeof(ip);
    ip.sin_addr.s_addr = htonl(0xc000020a + index);
    auto& node = block->nodes[index * 2];
    node.ifa_name = const_cast<char*>(name); node.ifa_flags = IFF_UP | IFF_RUNNING;
    node.ifa_addr = reinterpret_cast<sockaddr*>(&link); node.ifa_data = &data;
    auto& address = block->nodes[index * 2 + 1];
    address.ifa_name = const_cast<char*>(name); address.ifa_flags = node.ifa_flags;
    address.ifa_addr = reinterpret_cast<sockaddr*>(&ip);
  }
  for (int index = 0; index < 5; ++index) block->nodes[index].ifa_next = &block->nodes[index + 1];
  *result = block->nodes.data();
  return 0;
}
extern "C" void freeifaddrs(ifaddrs* list) { std::free(list); }
extern "C" int ioctl(int socket, unsigned long command, ...) {
  va_list arguments;
  va_start(arguments, command);
  void* argument = va_arg(arguments, void*);
  va_end(arguments);
  if (command == SIOCGLAGG) {
    auto* request = static_cast<lagg_reqall*>(argument);
    if (std::strcmp(request->ra_ifname, "laggtest0") == 0 && read_fixture().mode == 4) {
      request->ra_ports = 2;
      strlcpy(request->ra_port[0].rp_portname, "wlantest0", IFNAMSIZ);
      request->ra_port[0].rp_flags = LAGG_PORT_ACTIVE;
      strlcpy(request->ra_port[1].rp_portname, "emtest0", IFNAMSIZ);
      return 0;
    }
    errno = ENOTTY; return -1;
  }
  if (command == SIOCGIFMEDIA) {
    auto* media = static_cast<ifmediareq*>(argument);
    const bool wifi = std::strcmp(media->ifm_name, "wlantest0") == 0;
    if (wifi || std::strcmp(media->ifm_name, "emtest0") == 0) {
      media->ifm_active = wifi ? IFM_IEEE80211 | IFM_IEEE80211_11NG : IFM_ETHER | IFM_1000_T;
      media->ifm_status = IFM_AVALID | (active(wifi ? 1 : 0, read_fixture().mode) ? IFM_ACTIVE : 0);
      return 0;
    }
  }
  if (command == SIOCG80211) {
    auto* request = static_cast<ieee80211req*>(argument);
    if (std::strcmp(request->i_name, "wlantest0") == 0) {
      if (request->i_type == IEEE80211_IOC_SSID) {
        log("details");
        const char ssid[] = "Midnight Test";
        if (request->i_len < static_cast<int>(sizeof(ssid) - 1)) { errno = ENOSPC; return -1; }
        std::memcpy(request->i_data, ssid, sizeof(ssid) - 1);
        request->i_len = sizeof(ssid) - 1;
        return 0;
      }
      if (request->i_type == IEEE80211_IOC_BSSID) {
        std::memset(request->i_data, 1, IEEE80211_ADDR_LEN);
        request->i_len = IEEE80211_ADDR_LEN;
        return 0;
      }
      if (request->i_type == IEEE80211_IOC_STA_INFO) {
        auto* station = static_cast<ieee80211req_sta_req*>(request->i_data);
        station->info[0].isi_len = sizeof(ieee80211req_sta_info);
        station->info[0].isi_txmbps = 1080;
        request->i_len = sizeof(ieee80211req_sta_info);
        return 0;
      }
      errno = ENOTTY; return -1;
    }
  }
  using Ioctl = int (*)(int, unsigned long, ...);
  static auto real = reinterpret_cast<Ioctl>(dlsym(RTLD_NEXT, "ioctl"));
  if (!real) { errno = ENOSYS; return -1; }
  return real(socket, command, argument);
}
