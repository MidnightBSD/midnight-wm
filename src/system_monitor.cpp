// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause

#include <gtk/gtk.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#if defined(__FreeBSD__) || defined(__MidnightBSD__)
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <sys/socket.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <ifaddrs.h>
#include <libutil.h>
#include <pwd.h>
#include <unistd.h>
#endif

namespace {
std::string size_label(double bytes) {
  constexpr std::array units{"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
  std::size_t unit = 0;
  while (bytes >= 1024 && unit + 1 < units.size()) { bytes /= 1024; ++unit; }
  return std::format("{:.1f} {}", bytes, units[unit]);
}

struct Graph {
  GtkWidget* area{};
  GtkWidget* label{};
  std::deque<double> first, second;
  double ceiling{1};
  bool network{};

  void add(double a, double b = 0) {
    first.push_back(std::max(0.0, a));
    second.push_back(std::max(0.0, b));
    if (first.size() > 90) { first.pop_front(); second.pop_front(); }
    gtk_widget_queue_draw(area);
  }

  static gboolean draw(GtkWidget* widget, cairo_t* cr, gpointer data) {
    auto& graph = *static_cast<Graph*>(data);
    const double width = gtk_widget_get_allocated_width(widget);
    const double height = gtk_widget_get_allocated_height(widget);
    cairo_set_source_rgb(cr, 0.10, 0.09, 0.15);
    cairo_paint(cr);
    cairo_set_source_rgba(cr, 0.65, 0.60, 0.75, 0.25);
    cairo_set_line_width(cr, 1);
    for (int i = 1; i < 4; ++i) {
      const double y = height * i / 4;
      cairo_move_to(cr, 0, y); cairo_line_to(cr, width, y);
    }
    cairo_stroke(cr);
    const double scale = graph.network ? std::max(graph.ceiling, 1.0) : 100.0;
    const auto line = [&](const std::deque<double>& values, double r, double g, double b) {
      if (values.empty()) return;
      cairo_set_source_rgb(cr, r, g, b);
      cairo_set_line_width(cr, 2);
      bool started = false;
      std::size_t index = 0;
      for (double value : values) {
        const double x = width * static_cast<double>(index++) / 89.0;
        const double y = height * (1.0 - std::clamp(value / scale, 0.0, 1.0));
        if (!started) { cairo_move_to(cr, x, y); started = true; }
        else cairo_line_to(cr, x, y);
      }
      cairo_stroke(cr);
    };
    line(graph.first, 0.55, 0.39, 0.90);
    if (graph.network) line(graph.second, 0.23, 0.77, 0.86);
    return FALSE;
  }
};

enum ProcessColumn { PID, NAME, USER, STATE, CPU, MEMORY, PROCESS_COLUMNS };
enum MountColumn { DEVICE, MOUNT, TYPE, TOTAL, USED, AVAILABLE, PERCENT, MOUNT_COLUMNS };

GtkWidget* list_view(GtkListStore* store, const std::initializer_list<std::pair<const char*, int>>& columns) {
  auto* view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
  gtk_tree_view_set_headers_clickable(GTK_TREE_VIEW(view), TRUE);
  gtk_tree_view_set_enable_search(GTK_TREE_VIEW(view), TRUE);
  for (const auto& [title, index] : columns) {
    auto* renderer = gtk_cell_renderer_text_new();
    auto* column = gtk_tree_view_column_new_with_attributes(title, renderer, "text", index, nullptr);
    gtk_tree_view_column_set_resizable(column, TRUE);
    gtk_tree_view_column_set_sort_column_id(column, index);
    if (std::string_view(title) == "CPU %") {
      gtk_tree_view_column_set_cell_data_func(column, renderer,
          +[](GtkTreeViewColumn*, GtkCellRenderer* cell, GtkTreeModel* model,
              GtkTreeIter* row, gpointer) {
            double value = 0;
            gtk_tree_model_get(model, row, CPU, &value, -1);
            g_object_set(cell, "text", std::format("{:.1f}", value).c_str(), nullptr);
          }, nullptr, nullptr);
    }
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), column);
  }
  return view;
}

struct Monitor {
  GtkWidget* window{};
  GtkWidget* status{};
  GtkListStore* processes{};
  GtkListStore* mounts{};
  Graph cpu, memory, network;
  guint timer{};
  unsigned tick_count{};
#if defined(__FreeBSD__) || defined(__MidnightBSD__)
  std::array<long, CPUSTATES> previous_cpu{};
  bool have_cpu{};
  uint64_t previous_received{}, previous_sent{};
  gint64 previous_network_time{};
#endif

  ~Monitor() {
    if (timer) g_source_remove(timer);
    if (processes) g_object_unref(processes);
    if (mounts) g_object_unref(mounts);
  }

  static GtkWidget* scroller(GtkWidget* child) {
    auto* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), child);
    return scroll;
  }

  void add_graph(GtkWidget* parent, Graph& graph, const char* title, bool is_network = false) {
    graph.network = is_network;
    auto* frame = gtk_frame_new(title);
    auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    gtk_container_add(GTK_CONTAINER(frame), box);
    graph.label = gtk_label_new("Collecting samples…");
    gtk_label_set_xalign(GTK_LABEL(graph.label), 0);
    gtk_box_pack_start(GTK_BOX(box), graph.label, FALSE, FALSE, 0);
    graph.area = gtk_drawing_area_new();
    gtk_widget_set_size_request(graph.area, 200, 100);
    gtk_widget_set_vexpand(graph.area, TRUE);
    g_signal_connect(graph.area, "draw", G_CALLBACK(Graph::draw), &graph);
    gtk_box_pack_start(GTK_BOX(box), graph.area, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(parent), frame, TRUE, TRUE, 0);
  }

  void activate(GtkApplication* application) {
    if (window) { gtk_window_present(GTK_WINDOW(window)); return; }
    window = gtk_application_window_new(application);
    gtk_window_set_title(GTK_WINDOW(window), "System Monitor");
    gtk_window_set_icon_name(GTK_WINDOW(window), "utilities-system-monitor");
    gtk_window_set_default_size(GTK_WINDOW(window), 850, 630);
    gtk_container_set_border_width(GTK_CONTAINER(window), 10);
    g_signal_connect(window, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer data) {
      auto& self = *static_cast<Monitor*>(data);
      self.window = nullptr;
      if (self.timer) { g_source_remove(self.timer); self.timer = 0; }
    }), this);

    auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_add(GTK_CONTAINER(window), box);
    auto* tabs = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(box), tabs, TRUE, TRUE, 0);

    processes = gtk_list_store_new(PROCESS_COLUMNS, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_STRING, G_TYPE_DOUBLE, G_TYPE_STRING);
    auto* process_view = list_view(processes,
        {{"PID", PID}, {"Process", NAME}, {"User", USER}, {"State", STATE}, {"CPU %", CPU}, {"Memory", MEMORY}});
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(processes), CPU, GTK_SORT_DESCENDING);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), scroller(process_view), gtk_label_new("Processes"));

    auto* resource_scroll = gtk_scrolled_window_new(nullptr, nullptr);
    auto* resources = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(resources), 5);
    gtk_container_add(GTK_CONTAINER(resource_scroll), resources);
    add_graph(resources, cpu, "CPU");
    add_graph(resources, memory, "Memory");
    add_graph(resources, network, "Network traffic", true);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), resource_scroll, gtk_label_new("Resources"));

    mounts = gtk_list_store_new(MOUNT_COLUMNS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT);
    auto* mount_view = list_view(mounts, {{"Device", DEVICE}, {"Mount point", MOUNT},
        {"Type", TYPE}, {"Capacity", TOTAL}, {"Used", USED}, {"Available", AVAILABLE}});
    auto* progress = gtk_cell_renderer_progress_new();
    auto* usage = gtk_tree_view_column_new_with_attributes("Usage", progress, "value", PERCENT, nullptr);
    gtk_tree_view_append_column(GTK_TREE_VIEW(mount_view), usage);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), scroller(mount_view), gtk_label_new("File Systems"));

    status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(status), 0);
    gtk_box_pack_start(GTK_BOX(box), status, FALSE, FALSE, 0);
    gtk_widget_show_all(window);
    refresh();
    timer = g_timeout_add_seconds(2, +[](gpointer data) -> gboolean {
      static_cast<Monitor*>(data)->refresh();
      return G_SOURCE_CONTINUE;
    }, this);
  }

  void refresh() {
#if defined(__FreeBSD__) || defined(__MidnightBSD__)
    sample_processes();
    sample_resources();
    if (tick_count++ % 5 == 0) sample_mounts();
#else
    gtk_label_set_text(GTK_LABEL(status), "System sampling requires MidnightBSD or FreeBSD.");
#endif
  }

#if defined(__FreeBSD__) || defined(__MidnightBSD__)
  template <class T> static bool sysctl_value(const char* name, T& value) {
    size_t length = sizeof(value);
    return sysctlbyname(name, &value, &length, nullptr, 0) == 0 && length == sizeof(value);
  }

  void sample_processes() {
    int count = 0;
    std::unique_ptr<kinfo_proc, decltype(&std::free)> items(kinfo_getallproc(&count), std::free);
    if (!items) {
      gtk_label_set_text(GTK_LABEL(status),
          std::format("Cannot read processes: {}", std::strerror(errno)).c_str());
      return;
    }
    gtk_list_store_clear(processes);
    const long page_size = sysconf(_SC_PAGESIZE);
    std::unordered_map<uid_t, std::string> users;
    for (int i = 0; i < count; ++i) {
      const auto& item = items.get()[i];
      auto [user_entry, inserted] = users.try_emplace(item.ki_uid);
      if (inserted) {
        const auto* passwd = getpwuid(item.ki_uid);
        user_entry->second = passwd ? passwd->pw_name : std::to_string(item.ki_uid);
      }
      const char* state = "Other";
      switch (item.ki_stat) {
        case SRUN: state = "Running"; break;
        case SSLEEP: state = "Sleeping"; break;
        case SSTOP: state = "Stopped"; break;
        case SZOMB: state = "Zombie"; break;
      }
      const auto bytes = static_cast<double>(item.ki_rssize) * std::max(page_size, 0L);
      const auto memory_text = size_label(bytes);
      GtkTreeIter row;
      gtk_list_store_append(processes, &row);
      gtk_list_store_set(processes, &row, PID, item.ki_pid, NAME, item.ki_comm,
          USER, user_entry->second.c_str(), STATE, state, CPU, 100.0 * item.ki_pctcpu / FSCALE,
          MEMORY, memory_text.c_str(), -1);
    }
    gtk_label_set_text(GTK_LABEL(status), std::format("{} processes · refresh every 2 seconds", count).c_str());
  }

  void sample_resources() {
    std::array<long, CPUSTATES> times{};
    size_t length = sizeof(times);
    if (sysctlbyname("kern.cp_time", times.data(), &length, nullptr, 0) == 0 && length == sizeof(times)) {
      if (have_cpu) {
        uint64_t total = 0;
        for (std::size_t i = 0; i < times.size(); ++i)
          if (times[i] >= previous_cpu[i]) total += static_cast<uint64_t>(times[i] - previous_cpu[i]);
        const auto idle = times[CP_IDLE] >= previous_cpu[CP_IDLE] ?
            static_cast<uint64_t>(times[CP_IDLE] - previous_cpu[CP_IDLE]) : 0;
        const double percent = total ? 100.0 * (total - idle) / total : 0;
        cpu.add(percent);
        gtk_label_set_text(GTK_LABEL(cpu.label), std::format("CPU usage: {:.1f}%", percent).c_str());
      }
      previous_cpu = times;
      have_cpu = true;
    } else gtk_label_set_text(GTK_LABEL(cpu.label), "CPU data unavailable");

    uint64_t physical = 0;
    uint32_t free_pages = 0, inactive_pages = 0, laundry_pages = 0;
    uint32_t page_size = 0;
    if (sysctl_value("hw.physmem", physical) && sysctl_value("hw.pagesize", page_size) &&
        sysctl_value("vm.stats.vm.v_free_count", free_pages) &&
        sysctl_value("vm.stats.vm.v_inactive_count", inactive_pages) &&
        sysctl_value("vm.stats.vm.v_laundry_count", laundry_pages) && physical) {
      const uint64_t reclaimable = (static_cast<uint64_t>(free_pages) + inactive_pages + laundry_pages) * page_size;
      const uint64_t used = physical > reclaimable ? physical - reclaimable : 0;
      const double percent = 100.0 * used / physical;
      memory.add(percent);
      gtk_label_set_text(GTK_LABEL(memory.label),
          std::format("Memory: {} used of {} ({:.1f}%)", size_label(used), size_label(physical), percent).c_str());
    } else gtk_label_set_text(GTK_LABEL(memory.label), "Memory data unavailable");

    ifaddrs* addresses = nullptr;
    if (getifaddrs(&addresses) == 0) {
      uint64_t received = 0, sent = 0;
      for (auto* item = addresses; item; item = item->ifa_next) {
        if (!item->ifa_addr || item->ifa_addr->sa_family != AF_LINK || !item->ifa_data ||
            (item->ifa_flags & IFF_LOOPBACK)) continue;
        const auto* data = static_cast<const if_data*>(item->ifa_data);
        received += data->ifi_ibytes;
        sent += data->ifi_obytes;
      }
      freeifaddrs(addresses);
      const gint64 now = g_get_monotonic_time();
      if (previous_network_time && now > previous_network_time) {
        const double seconds = static_cast<double>(now - previous_network_time) / G_USEC_PER_SEC;
        const double down = received >= previous_received ? (received - previous_received) / seconds : 0;
        const double up = sent >= previous_sent ? (sent - previous_sent) / seconds : 0;
        network.add(down, up);
        network.ceiling = std::max({1024.0, down * 1.2, up * 1.2, network.ceiling * 0.98});
        gtk_label_set_text(GTK_LABEL(network.label),
            std::format("Receive: {}/s     Send: {}/s", size_label(down), size_label(up)).c_str());
      }
      previous_received = received;
      previous_sent = sent;
      previous_network_time = now;
    } else gtk_label_set_text(GTK_LABEL(network.label), "Network data unavailable");
  }

  void sample_mounts() {
    struct statfs* entries = nullptr;
    const int count = getmntinfo(&entries, MNT_NOWAIT);
    if (count <= 0) return;
    gtk_list_store_clear(mounts);
    for (int i = 0; i < count; ++i) {
      const auto& entry = entries[i];
      if (!entry.f_blocks || !entry.f_bsize) continue;
      const double total = static_cast<double>(entry.f_blocks) * entry.f_bsize;
      const double used = static_cast<double>(entry.f_blocks - std::min(entry.f_blocks, entry.f_bfree)) * entry.f_bsize;
      const double available = static_cast<double>(std::max<int64_t>(0, entry.f_bavail)) * entry.f_bsize;
      const int percent = static_cast<int>(std::clamp(100.0 * used / total, 0.0, 100.0));
      const auto total_text = size_label(total), used_text = size_label(used), available_text = size_label(available);
      GtkTreeIter row;
      gtk_list_store_append(mounts, &row);
      gtk_list_store_set(mounts, &row, DEVICE, entry.f_mntfromname, MOUNT, entry.f_mntonname,
          TYPE, entry.f_fstypename, TOTAL, total_text.c_str(), USED, used_text.c_str(),
          AVAILABLE, available_text.c_str(), PERCENT, percent, -1);
    }
  }
#endif
};
}

int main(int argc, char** argv) {
  auto* application = gtk_application_new("org.midnightbsd.Workspace.SystemMonitor", G_APPLICATION_DEFAULT_FLAGS);
  Monitor monitor;
  g_signal_connect(application, "activate", G_CALLBACK(+[](GtkApplication* app, gpointer data) {
    static_cast<Monitor*>(data)->activate(app);
  }), &monitor);
  const int result = g_application_run(G_APPLICATION(application), argc, argv);
  g_object_unref(application);
  return result;
}
