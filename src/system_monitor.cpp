// Copyright (c) 2026 Lucas Holt
// SPDX-License-Identifier: BSD-2-Clause

#include <gtk/gtk.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <csignal>
#include <sys/types.h>

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
#include <libgeom.h>
#include <pwd.h>
#include <unistd.h>
#include <vm/vm_param.h>
#endif

#include "battery.hpp"
#include "display_text.hpp"

namespace {
// Every string shown in the monitor originates outside this process: other
// users' executable names, mount points, passwd entries, and device identity
// strings. Strip controls and bidi formatting so a row cannot read as a
// different process or device than the one it describes.
std::string shown(std::string_view text) { return midnight::sanitize_display_text(text); }

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

#if defined(__FreeBSD__) || defined(__MidnightBSD__)
// ki_comm is limited to COMMLEN bytes. Recover a longer executable basename
// only when it extends that exact prefix, preserving custom process names.
std::string process_name(const kinfo_proc& process) {
  std::string name(process.ki_comm, strnlen(process.ki_comm, sizeof(process.ki_comm)));
  if (name.size() == COMMLEN) {
    std::array<char, MAXPATHLEN> path{};
    size_t length = path.size();
    int mib[]{CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, process.ki_pid};
    if (sysctl(mib, 4, path.data(), &length, nullptr, 0) == 0 &&
        length > 0 && length <= path.size() && path[length - 1] == '\0') {
      const std::string_view full_path(path.data(), strnlen(path.data(), length));
      const auto slash = full_path.find_last_of('/');
      const auto basename = full_path.substr(slash == std::string_view::npos ? 0 : slash + 1);
      if (basename.starts_with(name)) name = basename;
    }
  }
  return shown(name);
}

// Identity recorded per row so a kill request can confirm that the PID still
// names the process the user selected rather than a reused PID.
int64_t process_start(const kinfo_proc& process) {
  return static_cast<int64_t>(process.ki_start.tv_sec) * 1000000 + process.ki_start.tv_usec;
}

// Sensor discovery walks the sysctl tree through the same management OIDs
// sysctl(8) uses: {0,2} next leaf, {0,1} name, {0,4} type and format.
constexpr int sysctl_meta = 0, sysctl_meta_name = 1, sysctl_meta_next = 2, sysctl_meta_format = 4;
constexpr int sysctl_walk_limit = 100000;

struct SysctlLeaf {
  std::vector<int> mib;
  std::string name;
  u_int type{};
  std::string format;
};

// Kernel temperature OIDs use the "IK" format: kelvin scaled by ten, or by
// 10^n for "IKn". Return the scale, or zero for anything else.
int kelvin_scale(std::string_view format) {
  if (!format.starts_with("IK")) return 0;
  if (format.size() == 2) return 10;
  if (format.size() != 3 || format[2] < '0' || format[2] > '9') return 0;
  int scale = 1;
  for (char digit = '0'; digit < format[2]; ++digit) scale *= 10;
  return scale;
}

std::vector<int> sysctl_mib(const char* name) {
  std::array<int, CTL_MAXNAME> mib{};
  size_t length = mib.size();
  if (sysctlnametomib(name, mib.data(), &length) != 0 || length > mib.size()) return {};
  return {mib.begin(), mib.begin() + static_cast<std::ptrdiff_t>(length)};
}

bool sysctl_meta_query(int operation, const std::vector<int>& oid, void* buffer, size_t& length) {
  if (oid.size() > CTL_MAXNAME) return false;
  std::array<int, CTL_MAXNAME + 2> query{sysctl_meta, operation};
  std::copy(oid.begin(), oid.end(), query.begin() + 2);
  return sysctl(query.data(), static_cast<u_int>(oid.size() + 2), buffer, &length, nullptr, 0) == 0;
}

// Integer and string leaves below root, in kernel order. Only names and
// formats are read here; values are read later through the cached MIBs.
void walk_sysctl(const char* root, std::vector<SysctlLeaf>& leaves) {
  const auto prefix = sysctl_mib(root);
  if (prefix.empty()) return;
  auto current = prefix;
  for (int step = 0; step < sysctl_walk_limit; ++step) {
    std::array<int, CTL_MAXNAME> next{};
    size_t length = sizeof(next);
    if (!sysctl_meta_query(sysctl_meta_next, current, next.data(), length)) return;
    const auto count = length / sizeof(int);
    if (count < prefix.size() || count > next.size() ||
        !std::equal(prefix.begin(), prefix.end(), next.begin())) return;
    current.assign(next.begin(), next.begin() + static_cast<std::ptrdiff_t>(count));

    std::array<char, sizeof(u_int) + 64> format{};
    length = format.size();
    if (!sysctl_meta_query(sysctl_meta_format, current, format.data(), length) || length <= sizeof(u_int)) continue;
    u_int kind = 0;
    std::memcpy(&kind, format.data(), sizeof(kind));
    const u_int type = kind & CTLTYPE;
    if (type != CTLTYPE_INT && type != CTLTYPE_UINT && type != CTLTYPE_STRING) continue;

    std::array<char, 512> name{};
    length = name.size();
    if (!sysctl_meta_query(sysctl_meta_name, current, name.data(), length) || length == 0) continue;
    leaves.push_back({current, std::string(name.data(), strnlen(name.data(), length)), type,
        std::string(format.data() + sizeof(u_int), strnlen(format.data() + sizeof(u_int), length - sizeof(u_int)))});
  }
}

// Orders "dev.cpu.10" after "dev.cpu.9".
bool natural_less(std::string_view a, std::string_view b) {
  std::size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    const bool a_digit = std::isdigit(static_cast<unsigned char>(a[i]));
    const bool b_digit = std::isdigit(static_cast<unsigned char>(b[j]));
    if (a_digit && b_digit) {
      const auto a_end = a.find_first_not_of("0123456789", i), b_end = b.find_first_not_of("0123456789", j);
      auto a_run = a.substr(i, a_end == std::string_view::npos ? std::string_view::npos : a_end - i);
      auto b_run = b.substr(j, b_end == std::string_view::npos ? std::string_view::npos : b_end - j);
      while (a_run.size() > 1 && a_run.front() == '0') a_run.remove_prefix(1);
      while (b_run.size() > 1 && b_run.front() == '0') b_run.remove_prefix(1);
      if (a_run.size() != b_run.size()) return a_run.size() < b_run.size();
      if (a_run != b_run) return a_run < b_run;
      i = a_end == std::string_view::npos ? a.size() : a_end;
      j = b_end == std::string_view::npos ? b.size() : b_end;
    } else {
      if (a[i] != b[j]) return a[i] < b[j];
      ++i; ++j;
    }
  }
  return a.size() - i < b.size() - j;
}

struct SensorThreshold {
  std::string label;
  std::vector<int> mib;
  int scale{};
};

struct SensorSource {
  enum Kind { Temperature, Fan } kind{};
  int order{};  // group position: CPU, ACPI, other temperatures, fans
  std::string group, label, oid;
  std::vector<int> mib;
  int scale{};
  std::vector<SensorThreshold> limits;
};

std::vector<SensorSource> discover_sensors() {
  std::vector<SysctlLeaf> leaves;
  walk_sysctl("dev", leaves);
  walk_sysctl("hw.acpi", leaves);

  std::unordered_map<std::string, const SysctlLeaf*> by_name;
  for (const auto& leaf : leaves) by_name.emplace(leaf.name, &leaf);
  const auto threshold = [&](const std::string& name, const char* label) -> std::optional<SensorThreshold> {
    const auto found = by_name.find(name);
    if (found == by_name.end() || found->second->type != CTLTYPE_INT) return std::nullopt;
    const int scale = kelvin_scale(found->second->format);
    if (!scale) return std::nullopt;
    return SensorThreshold{label, found->second->mib, scale};
  };
  const auto description = [&](const std::string& device) {
    const auto found = by_name.find(device + ".%desc");
    if (found == by_name.end() || found->second->type != CTLTYPE_STRING) return std::string{};
    std::array<char, 256> text{};
    size_t length = text.size() - 1;
    if (sysctl(found->second->mib.data(), static_cast<u_int>(found->second->mib.size()), text.data(),
            &length, nullptr, 0) != 0) return std::string{};
    return std::string(text.data(), strnlen(text.data(), std::min(length, text.size() - 1)));
  };

  std::vector<SensorSource> sensors;
  for (const auto& leaf : leaves) {
    const std::string_view name(leaf.name);
    const auto dot = name.find_last_of('.');
    if (dot == std::string_view::npos) continue;
    const std::string parent(name.substr(0, dot));
    const std::string_view last = name.substr(dot + 1);
    const int scale = leaf.type == CTLTYPE_INT ? kelvin_scale(leaf.format) : 0;

    if (scale) {
      // Trip points and junction limits are shown beside their sensor.
      if (last.starts_with('_') || last == "tjmax") continue;
      SensorSource sensor{SensorSource::Temperature, 2, "", "", leaf.name, leaf.mib, scale, {}};
      if (name.starts_with("dev.cpu.") && last == "temperature") {
        sensor.order = 0;
        sensor.group = "CPU";
        sensor.label = "CPU " + std::string(name.substr(8, dot - 8));
        if (auto limit = threshold(parent + ".coretemp.tjmax", "max")) sensor.limits.push_back(std::move(*limit));
      } else if (name.starts_with("hw.acpi.thermal.") && last == "temperature") {
        sensor.order = 1;
        sensor.group = "ACPI thermal zone";
        sensor.label = std::string(name.substr(16, dot - 16));
        if (auto limit = threshold(parent + "._PSV", "passive")) sensor.limits.push_back(std::move(*limit));
        if (auto limit = threshold(parent + "._HOT", "hot")) sensor.limits.push_back(std::move(*limit));
        if (auto limit = threshold(parent + "._CRT", "critical")) sensor.limits.push_back(std::move(*limit));
      } else if (name.starts_with("dev.")) {
        // dev.<driver>.<unit>.<sensor…>: label by the device description.
        const auto driver_end = name.find('.', 4);
        const auto unit_end = driver_end == std::string_view::npos ? driver_end : name.find('.', driver_end + 1);
        if (unit_end == std::string_view::npos) {
          sensor.group = "Other";
          sensor.label = leaf.name;
        } else {
          const std::string device(name.substr(0, unit_end));
          const auto text = description(device);
          sensor.group = std::string(name.substr(4, driver_end - 4)) + std::string(name.substr(driver_end + 1, unit_end - driver_end - 1));
          if (!text.empty()) sensor.group += " (" + text + ")";
          sensor.label = std::string(name.substr(unit_end + 1));
        }
      } else {
        sensor.group = "Other";
        sensor.label = leaf.name;
      }
      sensors.push_back(std::move(sensor));
      continue;
    }

    // Fan tachometers (asmc, acpi_ibm, and similar drivers) report RPM.
    if ((leaf.type == CTLTYPE_INT || leaf.type == CTLTYPE_UINT) && name.find("fan") != std::string_view::npos &&
        (last.find("speed") != std::string_view::npos || last.find("rpm") != std::string_view::npos) &&
        !last.starts_with("min") && !last.starts_with("max") && !last.starts_with("safe") &&
        !last.starts_with("target")) {
      sensors.push_back({SensorSource::Fan, 3, "Fans", leaf.name, leaf.name, leaf.mib, 0, {}});
    }
  }
  std::stable_sort(sensors.begin(), sensors.end(), [](const SensorSource& a, const SensorSource& b) {
    if (a.order != b.order) return a.order < b.order;
    return natural_less(a.oid, b.oid);
  });
  return sensors;
}

std::optional<int> read_int(const std::vector<int>& mib) {
  int value = 0;
  size_t length = sizeof(value);
  if (mib.empty() || sysctl(mib.data(), static_cast<u_int>(mib.size()), &value, &length, nullptr, 0) != 0 ||
      length != sizeof(value)) return std::nullopt;
  return value;
}

// A kelvin reading at or below zero is a "not supported" sentinel such as
// an ACPI trip point of -1.
std::optional<double> read_celsius(const std::vector<int>& mib, int scale) {
  const auto value = read_int(mib);
  if (!value || *value <= 0 || scale <= 0) return std::nullopt;
  return static_cast<double>(*value) / scale - 273.15;
}
#endif

enum ProcessColumn { PID, NAME, USER, STATE, CPU, MEMORY, START, OWNER, PROCESS_COLUMNS };
enum MountColumn { DEVICE, MOUNT, TYPE, TOTAL, USED, AVAILABLE, PERCENT, MOUNT_COLUMNS };
enum DriveColumn { DRIVE_DEVICE, DRIVE_TYPE, DRIVE_MODEL, DRIVE_SIZE, DRIVE_COLUMNS };
enum SensorColumn { SENSOR_GROUP, SENSOR_NAME, SENSOR_READING, SENSOR_LIMIT, SENSOR_SOURCE, SENSOR_COLUMNS };

GtkWidget* list_view(GtkListStore* store, const std::initializer_list<std::pair<const char*, int>>& columns) {
  auto* view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
  gtk_tree_view_set_headers_clickable(GTK_TREE_VIEW(view), TRUE);
  gtk_tree_view_set_enable_search(GTK_TREE_VIEW(view), TRUE);
  for (const auto& [title, index] : columns) {
    auto* renderer = gtk_cell_renderer_text_new();
    auto* column = gtk_tree_view_column_new_with_attributes(title, renderer, "text", index, nullptr);
    gtk_tree_view_column_set_resizable(column, TRUE);
    gtk_tree_view_column_set_sort_column_id(column, index);
    if (std::string_view(title) == "Process") {
      gtk_tree_view_column_set_min_width(column, 260);
      gtk_tree_view_column_set_expand(column, TRUE);
    }
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
  GtkListStore* drives{};
  GtkListStore* sensors{};
  GtkWidget* sensor_status{};
  GtkWidget *hardware_cpu{}, *hardware_ram{}, *hardware_swap{}, *hardware_os{}, *hardware_status{};
  GtkWidget* process_view{};
  GtkWidget* process_menu{};
  GtkWidget *end_button{}, *force_button{};
  Graph cpu, memory, network;
  guint timer{};
  unsigned tick_count{};
  // Rows are updated in place so sorting and the user's selection survive
  // each refresh. GtkListStore iterators stay valid until their row is removed.
  std::unordered_map<pid_t, GtkTreeIter> process_rows;
  std::unordered_map<uid_t, std::string> users;
  // Sensor rows are keyed by their source so they also update in place.
  std::unordered_map<std::string, GtkTreeIter> sensor_rows;
#if defined(__FreeBSD__) || defined(__MidnightBSD__)
  std::vector<SensorSource> sensor_sources;
  bool sensors_discovered{};
  std::string power_daemon;
  midnight::BatteryMonitor battery;
  // ACPI battery reads can call slow embedded-controller methods; poll them
  // every 30 seconds like the panel instead of on every 2 second refresh.
  std::optional<midnight::BatteryState> battery_state;
  gint64 battery_read_time{};
  std::array<long, CPUSTATES> previous_cpu{};
  bool have_cpu{};
  uint64_t previous_received{}, previous_sent{};
  gint64 previous_network_time{};
#endif

  ~Monitor() {
    if (timer) g_source_remove(timer);
    if (processes) g_object_unref(processes);
    if (mounts) g_object_unref(mounts);
    if (drives) g_object_unref(drives);
    if (sensors) g_object_unref(sensors);
  }

  // The list stores outlive the window so a closed-and-reactivated monitor
  // reuses them instead of leaking the previous set.
  void ensure_stores() {
    if (!processes)
      processes = gtk_list_store_new(PROCESS_COLUMNS, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING,
          G_TYPE_STRING, G_TYPE_DOUBLE, G_TYPE_STRING, G_TYPE_INT64, G_TYPE_UINT);
    if (!mounts)
      mounts = gtk_list_store_new(MOUNT_COLUMNS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
          G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT);
    if (!drives)
      drives = gtk_list_store_new(DRIVE_COLUMNS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    if (!sensors)
      sensors = gtk_list_store_new(SENSOR_COLUMNS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
          G_TYPE_STRING, G_TYPE_STRING);
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

  static GtkWidget* hardware_row(GtkWidget* grid, int row, const char* title) {
    auto* heading = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(heading), 0);
    gtk_widget_set_halign(heading, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), heading, 0, row, 1, 1);
    auto* value = gtk_label_new("Loading…");
    gtk_label_set_xalign(GTK_LABEL(value), 0);
    gtk_label_set_selectable(GTK_LABEL(value), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(value), TRUE);
    gtk_widget_set_hexpand(value, TRUE);
    gtk_grid_attach(GTK_GRID(grid), value, 1, row, 1, 1);
    return value;
  }

  void add_hardware_tab(GtkWidget* tabs) {
    auto* scroll = gtk_scrolled_window_new(nullptr, nullptr);
    auto* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    gtk_container_add(GTK_CONTAINER(scroll), page);

    auto* system = gtk_frame_new("System");
    auto* grid = gtk_grid_new();
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 18);
    gtk_container_add(GTK_CONTAINER(system), grid);
    gtk_box_pack_start(GTK_BOX(page), system, FALSE, FALSE, 0);
    hardware_cpu = hardware_row(grid, 0, "CPU");
    hardware_ram = hardware_row(grid, 1, "Physical memory");
    hardware_swap = hardware_row(grid, 2, "Swap");
    hardware_os = hardware_row(grid, 3, "OS release");

    auto* frame = gtk_frame_new("Physical drives");
    auto* drive_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(drive_box), 8);
    gtk_container_add(GTK_CONTAINER(frame), drive_box);
    gtk_box_pack_start(GTK_BOX(page), frame, TRUE, TRUE, 0);
    auto* drive_view = list_view(drives, {{"Device", DRIVE_DEVICE}, {"Type", DRIVE_TYPE},
        {"Model", DRIVE_MODEL}, {"Capacity", DRIVE_SIZE}});
    // A size request on the tree view does not pass through its scrolled
    // window, which otherwise shrinks to a few pixels and hides every row
    // past the first. Reserve room for the header and at least five drives,
    // and let the list grow to its natural height when more are present.
    auto* drive_scroll = scroller(drive_view);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(drive_scroll), 200);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(drive_scroll), 480);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(drive_scroll), TRUE);
    gtk_box_pack_start(GTK_BOX(drive_box), drive_scroll, TRUE, TRUE, 0);
    hardware_status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(hardware_status), 0);
    gtk_box_pack_start(GTK_BOX(drive_box), hardware_status, FALSE, FALSE, 0);

    auto* refresh_button = gtk_button_new_with_mnemonic("_Refresh hardware");
    gtk_widget_set_halign(refresh_button, GTK_ALIGN_START);
    g_signal_connect(refresh_button, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
      static_cast<Monitor*>(data)->refresh_hardware();
    }), this);
    gtk_box_pack_start(GTK_BOX(page), refresh_button, FALSE, FALSE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), scroll, gtk_label_new("Hardware"));
  }

  void add_sensor_tab(GtkWidget* tabs) {
    auto* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    auto* view = list_view(sensors, {{"Group", SENSOR_GROUP}, {"Sensor", SENSOR_NAME},
        {"Reading", SENSOR_READING}, {"Limits", SENSOR_LIMIT}, {"Source", SENSOR_SOURCE}});
    gtk_tree_view_set_search_column(GTK_TREE_VIEW(view), SENSOR_NAME);
    gtk_box_pack_start(GTK_BOX(page), scroller(view), TRUE, TRUE, 0);
    sensor_status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(sensor_status), 0);
    gtk_label_set_line_wrap(GTK_LABEL(sensor_status), TRUE);
    gtk_box_pack_start(GTK_BOX(page), sensor_status, FALSE, FALSE, 0);
    auto* refresh_button = gtk_button_new_with_mnemonic("Re_scan sensors");
    gtk_widget_set_tooltip_text(refresh_button, "Look for sensors again, for example after loading coretemp(4)");
    gtk_widget_set_halign(refresh_button, GTK_ALIGN_START);
    g_signal_connect(refresh_button, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
      static_cast<Monitor*>(data)->rescan_sensors();
    }), this);
    gtk_box_pack_start(GTK_BOX(page), refresh_button, FALSE, FALSE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), page, gtk_label_new("Sensors"));
  }

  void rescan_sensors() {
#if defined(__FreeBSD__) || defined(__MidnightBSD__)
    sensors_discovered = false;
    sample_sensors();
#endif
  }

  void add_process_tab(GtkWidget* tabs) {
    auto* page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    process_view = list_view(processes,
        {{"PID", PID}, {"Process", NAME}, {"User", USER}, {"State", STATE}, {"CPU %", CPU}, {"Memory", MEMORY}});
    gtk_tree_view_set_search_column(GTK_TREE_VIEW(process_view), NAME);
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(processes), CPU, GTK_SORT_DESCENDING);
    gtk_box_pack_start(GTK_BOX(page), scroller(process_view), TRUE, TRUE, 0);

    auto* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    end_button = gtk_button_new_with_mnemonic("_End Process");
    gtk_widget_set_tooltip_text(end_button, "Ask the selected process to exit (SIGTERM)");
    force_button = gtk_button_new_with_mnemonic("_Force Quit");
    gtk_widget_set_tooltip_text(force_button, "Terminate the selected process immediately (SIGKILL)");
    gtk_box_pack_end(GTK_BOX(buttons), force_button, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(buttons), end_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), buttons, FALSE, FALSE, 0);
    g_signal_connect(end_button, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
      static_cast<Monitor*>(data)->end_selected_process(SIGTERM);
    }), this);
    g_signal_connect(force_button, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
      static_cast<Monitor*>(data)->end_selected_process(SIGKILL);
    }), this);

    process_menu = gtk_menu_new();
    auto* end_item = gtk_menu_item_new_with_mnemonic("_End Process");
    auto* force_item = gtk_menu_item_new_with_mnemonic("_Force Quit");
    gtk_menu_shell_append(GTK_MENU_SHELL(process_menu), end_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(process_menu), force_item);
    gtk_widget_show_all(process_menu);
    gtk_menu_attach_to_widget(GTK_MENU(process_menu), process_view, nullptr);
    g_signal_connect(end_item, "activate", G_CALLBACK(+[](GtkMenuItem*, gpointer data) {
      static_cast<Monitor*>(data)->end_selected_process(SIGTERM);
    }), this);
    g_signal_connect(force_item, "activate", G_CALLBACK(+[](GtkMenuItem*, gpointer data) {
      static_cast<Monitor*>(data)->end_selected_process(SIGKILL);
    }), this);
    g_signal_connect(process_view, "button-press-event", G_CALLBACK(+[](GtkWidget* view, GdkEventButton* event,
        gpointer data) -> gboolean {
      if (event->type != GDK_BUTTON_PRESS || event->button != GDK_BUTTON_SECONDARY) return FALSE;
      auto& self = *static_cast<Monitor*>(data);
      GtkTreePath* path = nullptr;
      if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(view), static_cast<int>(event->x),
              static_cast<int>(event->y), &path, nullptr, nullptr, nullptr)) return FALSE;
      gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)), path);
      gtk_tree_path_free(path);
      if (self.process_menu) gtk_menu_popup_at_pointer(GTK_MENU(self.process_menu), reinterpret_cast<GdkEvent*>(event));
      return TRUE;
    }), this);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(process_view)), "changed",
        G_CALLBACK(+[](GtkTreeSelection*, gpointer data) { static_cast<Monitor*>(data)->update_process_buttons(); }),
        this);
    update_process_buttons();
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), page, gtk_label_new("Processes"));
  }

  void update_process_buttons() {
    if (!process_view || !end_button) return;
    const bool selected = gtk_tree_selection_get_selected(
        gtk_tree_view_get_selection(GTK_TREE_VIEW(process_view)), nullptr, nullptr);
    gtk_widget_set_sensitive(end_button, selected);
    gtk_widget_set_sensitive(force_button, selected);
  }

  void message(GtkMessageType type, const std::string& primary, const std::string& secondary) {
    auto* dialog = gtk_message_dialog_new(GTK_WINDOW(window), static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
        type, GTK_BUTTONS_CLOSE, "%s", primary.c_str());
    if (!secondary.empty())
      gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", secondary.c_str());
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
  }

  void end_selected_process(int signal_number) {
    if (!process_view) return;
    GtkTreeIter row;
    if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(process_view)), nullptr, &row))
      return;
    int pid = 0;
    gchar* raw_name = nullptr;
    gint64 start = 0;
    guint owner = 0;
    gtk_tree_model_get(GTK_TREE_MODEL(processes), &row, PID, &pid, NAME, &raw_name, START, &start, OWNER, &owner, -1);
    const std::string name = raw_name ? raw_name : "";
    g_free(raw_name);
    // kill(2) treats 0 and negative PIDs as process groups or every process;
    // a row can never legitimately request that.
    if (pid <= 0) return;

    const bool force = signal_number == SIGKILL;
    auto* dialog = gtk_message_dialog_new(GTK_WINDOW(window), static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
        GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "%s",
        std::format("{} “{}” (PID {})?", force ? "Force quit" : "End process", name, pid).c_str());
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", force ?
        "The process is terminated immediately and cannot save its work." :
        "The process is asked to exit and may save its work first. Use Force Quit if it does not respond.");
    gtk_dialog_add_button(GTK_DIALOG(dialog), "_Cancel", GTK_RESPONSE_CANCEL);
    auto* confirm = gtk_dialog_add_button(GTK_DIALOG(dialog), force ? "_Force Quit" : "_End Process",
        GTK_RESPONSE_ACCEPT);
    gtk_style_context_add_class(gtk_widget_get_style_context(confirm), "destructive-action");
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    const int response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    if (response != GTK_RESPONSE_ACCEPT) return;

#if defined(__FreeBSD__) || defined(__MidnightBSD__)
    // The dialog may have been open for a while; make sure the PID still
    // belongs to the process the row described before signalling it.
    std::unique_ptr<kinfo_proc, decltype(&std::free)> current(kinfo_getproc(pid), std::free);
    if (!current || process_start(*current) != start || current->ki_uid != owner) {
      message(GTK_MESSAGE_INFO, std::format("“{}” has already exited.", name),
          "The process list has been refreshed.");
      refresh();
      return;
    }
#endif
    if (kill(pid, signal_number) != 0) {
      const int error = errno;
      message(GTK_MESSAGE_ERROR, std::format("Cannot end “{}” (PID {}).", name, pid), error == EPERM ?
          "You do not have permission to end this process. It belongs to another user or the system." :
          std::strerror(error));
    }
    refresh();
  }

  void activate(GtkApplication* application) {
    if (window) { gtk_window_present(GTK_WINDOW(window)); return; }
    window = gtk_application_window_new(application);
    gtk_window_set_title(GTK_WINDOW(window), "System Monitor");
    gtk_window_set_icon_name(GTK_WINDOW(window), "utilities-system-monitor");
    gtk_window_set_default_size(GTK_WINDOW(window), 850, 640);
    gtk_container_set_border_width(GTK_CONTAINER(window), 10);
    g_signal_connect(window, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer data) {
      auto& self = *static_cast<Monitor*>(data);
      self.window = nullptr;
      self.process_view = nullptr;
      self.process_menu = nullptr;  // attached to the view, destroyed with it
      self.end_button = self.force_button = nullptr;
      self.hardware_cpu = self.hardware_ram = self.hardware_swap = self.hardware_os = self.hardware_status = nullptr;
      self.status = nullptr;
      self.sensor_status = nullptr;
      if (self.timer) { g_source_remove(self.timer); self.timer = 0; }
    }), this);
    ensure_stores();

    auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_add(GTK_CONTAINER(window), box);
    auto* tabs = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(box), tabs, TRUE, TRUE, 0);

    add_process_tab(tabs);

    auto* resource_scroll = gtk_scrolled_window_new(nullptr, nullptr);
    auto* resources = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(resources), 5);
    gtk_container_add(GTK_CONTAINER(resource_scroll), resources);
    add_graph(resources, cpu, "CPU");
    add_graph(resources, memory, "Memory");
    add_graph(resources, network, "Network traffic", true);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), resource_scroll, gtk_label_new("Resources"));

    auto* mount_view = list_view(mounts, {{"Device", DEVICE}, {"Mount point", MOUNT},
        {"Type", TYPE}, {"Capacity", TOTAL}, {"Used", USED}, {"Available", AVAILABLE}});
    auto* progress = gtk_cell_renderer_progress_new();
    auto* usage = gtk_tree_view_column_new_with_attributes("Usage", progress, "value", PERCENT, nullptr);
    gtk_tree_view_append_column(GTK_TREE_VIEW(mount_view), usage);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), scroller(mount_view), gtk_label_new("File Systems"));
    add_hardware_tab(tabs);
    add_sensor_tab(tabs);

    status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(status), 0);
    gtk_box_pack_start(GTK_BOX(box), status, FALSE, FALSE, 0);
    gtk_widget_show_all(window);
    refresh();
    refresh_hardware();
    timer = g_timeout_add_seconds(2, +[](gpointer data) -> gboolean {
      static_cast<Monitor*>(data)->refresh();
      return G_SOURCE_CONTINUE;
    }, this);
  }

  void refresh() {
#if defined(__FreeBSD__) || defined(__MidnightBSD__)
    // Name lookups can reach NIS or LDAP, so cache them across refreshes and
    // only drop the cache every five minutes to pick up renamed accounts.
    if (tick_count % 150 == 0) users.clear();
    sample_processes();
    sample_resources();
    // Rescan once a minute so sensors appear after a driver is loaded.
    if (tick_count % 30 == 0) sensors_discovered = false;
    sample_sensors();
    if (tick_count++ % 5 == 0) sample_mounts();
#else
    gtk_label_set_text(GTK_LABEL(status), "System sampling requires MidnightBSD or FreeBSD.");
    if (sensor_status) gtk_label_set_text(GTK_LABEL(sensor_status), "Sensors require MidnightBSD or FreeBSD.");
#endif
  }

  void refresh_hardware() {
#if defined(__FreeBSD__) || defined(__MidnightBSD__)
    const auto model = sysctl_text("hw.model");
    int logical_cpus = 0;
    const bool has_cpu_count = sysctl_value("hw.ncpu", logical_cpus) && logical_cpus > 0;
    set_hardware_label(hardware_cpu, model.empty() ? "Unavailable" :
        has_cpu_count ? std::format("{} ({} logical CPUs)", model, logical_cpus) : model);

    uint64_t physical = 0;
    set_hardware_label(hardware_ram, sysctl_value("hw.physmem", physical) && physical ?
        size_label(physical) : "Unavailable");

    uint64_t swap_total = 0, swap_used = 0;
    if (read_swap(swap_total, swap_used))
      set_hardware_label(hardware_swap, swap_total ?
          std::format("{} allocated · {} used · {} available", size_label(swap_total),
              size_label(swap_used), size_label(swap_total - swap_used)) : "No swap configured");
    else set_hardware_label(hardware_swap, "Unavailable");

    const auto system = sysctl_text("kern.ostype");
    const auto release = sysctl_text("kern.osrelease");
    set_hardware_label(hardware_os, system.empty() || release.empty() ? "Unavailable" : system + " " + release);
    sample_drives();
#else
    set_hardware_label(hardware_cpu, "Requires MidnightBSD or FreeBSD");
    set_hardware_label(hardware_ram, "Unavailable");
    set_hardware_label(hardware_swap, "Unavailable");
    set_hardware_label(hardware_os, "Unavailable");
    gtk_label_set_text(GTK_LABEL(hardware_status), "Drive information unavailable.");
#endif
  }

  static void set_hardware_label(GtkWidget* label, const std::string& value) {
    if (label) gtk_label_set_text(GTK_LABEL(label), shown(value).c_str());
  }

#if defined(__FreeBSD__) || defined(__MidnightBSD__)
  template <class T> static bool sysctl_value(const char* name, T& value) {
    size_t length = sizeof(value);
    return sysctlbyname(name, &value, &length, nullptr, 0) == 0 && length == sizeof(value);
  }

  static std::string sysctl_text(const char* name) {
    size_t length = 0;
    if (sysctlbyname(name, nullptr, &length, nullptr, 0) != 0 || length == 0 || length > 4096) return {};
    std::string value(length, '\0');
    if (sysctlbyname(name, value.data(), &length, nullptr, 0) != 0) return {};
    value.resize(length);
    if (!value.empty() && value.back() == '\0') value.pop_back();
    return value;
  }

  static bool read_swap(uint64_t& total, uint64_t& used) {
    std::array<int, CTL_MAXNAME> mib{};
    size_t mib_size = mib.size() - 1;
    if (sysctlnametomib("vm.swap_info", mib.data(), &mib_size) != 0) return false;
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return false;
    for (int index = 0; index < 256; ++index) {
      mib[mib_size] = index;
      xswdev device{};
      size_t length = sizeof(device);
      if (sysctl(mib.data(), static_cast<u_int>(mib_size + 1), &device, &length, nullptr, 0) != 0)
        return errno == ENOENT;
      if (length != sizeof(device) || device.xsw_version != XSWDEV_VERSION ||
          device.xsw_nblks < 0 || device.xsw_used < 0) return false;
      const auto blocks = static_cast<uint64_t>(device.xsw_nblks);
      total += blocks * static_cast<uint64_t>(page_size);
      used += std::min(blocks, static_cast<uint64_t>(device.xsw_used)) * static_cast<uint64_t>(page_size);
    }
    return false;
  }

  static std::string_view drive_type(std::string_view name, std::string_view rotation) {
    if (name.starts_with("cd") || name.starts_with("acd")) return "Optical";
    if (name.starts_with("vtbd") || name.starts_with("xbd")) return "Virtual disk";
    if (name.starts_with("nvd") || name.starts_with("nda") || rotation == "0") return "SSD";
    if (!rotation.empty()) {
      const std::string rpm(rotation);
      char* end = nullptr;
      const long speed = std::strtol(rpm.c_str(), &end, 10);
      if (end != rpm.c_str() && *end == '\0' && speed > 0) return "Hard disk";
    }
    return "Disk (type unknown)";
  }

  static std::string_view drive_config(const gprovider& provider, std::string_view key) {
    gconfig* config = nullptr;
    LIST_FOREACH(config, &provider.lg_config, lg_config)
      if (config->lg_name && key == config->lg_name) return config->lg_val ? config->lg_val : "";
    return {};
  }

  void sample_drives() {
    gmesh mesh{};
    const int error = geom_gettree(&mesh);
    if (error != 0) {
      gtk_label_set_text(GTK_LABEL(hardware_status),
          std::format("Cannot read drive information: {}", std::strerror(error)).c_str());
      return;
    }
    gtk_list_store_clear(drives);
    int count = 0;
    gclass* drive_class = nullptr;
    LIST_FOREACH(drive_class, &mesh.lg_class, lg_class) {
      if (!drive_class->lg_name || std::string_view(drive_class->lg_name) != "DISK") continue;
      ggeom* geom = nullptr;
      LIST_FOREACH(geom, &drive_class->lg_geom, lg_geom) {
        if (!geom->lg_name) continue;
        const std::string_view name(geom->lg_name);
        // Memory disks are software devices, not physical drives.
        if (name.starts_with("md") && name.size() > 2 && std::isdigit(static_cast<unsigned char>(name[2]))) continue;
        auto* provider = LIST_FIRST(&geom->lg_provider);
        if (!provider) continue;
        const auto type = drive_type(name, drive_config(*provider, "rotationrate"));
        const auto description = drive_config(*provider, "descr");
        const std::string model = description.empty() ? "Unknown model" : shown(description);
        const std::string capacity = provider->lg_mediasize > 0 ?
            size_label(static_cast<double>(provider->lg_mediasize)) : "No media";
        const std::string device = shown(std::format("/dev/{}", name));
        GtkTreeIter row;
        gtk_list_store_append(drives, &row);
        gtk_list_store_set(drives, &row, DRIVE_DEVICE, device.c_str(), DRIVE_TYPE, type.data(),
            DRIVE_MODEL, model.c_str(), DRIVE_SIZE, capacity.c_str(), -1);
        ++count;
      }
    }
    geom_deletetree(&mesh);
    gtk_label_set_text(GTK_LABEL(hardware_status), count ?
        std::format("{} drive{} found", count, count == 1 ? "" : "s").c_str() : "No drives found");
  }

  void sample_processes() {
    int count = 0;
    std::unique_ptr<kinfo_proc, decltype(&std::free)> items(kinfo_getallproc(&count), std::free);
    if (!items) {
      gtk_label_set_text(GTK_LABEL(status),
          std::format("Cannot read processes: {}", std::strerror(errno)).c_str());
      return;
    }
    const long page_size = sysconf(_SC_PAGESIZE);
    std::unordered_map<pid_t, GtkTreeIter> seen;
    seen.reserve(static_cast<std::size_t>(count));
    power_daemon.clear();
    for (int i = 0; i < count; ++i) {
      const auto& item = items.get()[i];
      if (item.ki_pid <= 0) continue;
      if (item.ki_uid == 0 && power_daemon.empty()) {
        const std::string_view command(item.ki_comm, strnlen(item.ki_comm, sizeof(item.ki_comm)));
        if (command == "powerd" || command == "powerd++") power_daemon = command;
      }
      auto [user_entry, inserted] = users.try_emplace(item.ki_uid);
      if (inserted) {
        const auto* passwd = getpwuid(item.ki_uid);
        user_entry->second = passwd ? shown(passwd->pw_name) : std::to_string(item.ki_uid);
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
      const auto name = process_name(item);
      GtkTreeIter row;
      if (const auto existing = process_rows.find(item.ki_pid); existing != process_rows.end()) row = existing->second;
      else gtk_list_store_append(processes, &row);
      gtk_list_store_set(processes, &row, PID, item.ki_pid, NAME, name.c_str(),
          USER, user_entry->second.c_str(), STATE, state, CPU, 100.0 * item.ki_pctcpu / FSCALE,
          MEMORY, memory_text.c_str(), START, process_start(item), OWNER, static_cast<guint>(item.ki_uid), -1);
      seen.insert_or_assign(item.ki_pid, row);
    }
    for (auto& [pid, row] : process_rows)
      if (!seen.contains(pid)) gtk_list_store_remove(processes, &row);
    process_rows = std::move(seen);
    gtk_label_set_text(GTK_LABEL(status),
        std::format("{} processes · refresh every 2 seconds", process_rows.size()).c_str());
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

  void set_sensor_row(std::unordered_map<std::string, GtkTreeIter>& seen, const std::string& key,
      const std::string& group, const std::string& name, const std::string& reading,
      const std::string& limit, const std::string& source) {
    GtkTreeIter row;
    if (const auto existing = sensor_rows.find(key); existing != sensor_rows.end()) row = existing->second;
    else gtk_list_store_append(sensors, &row);
    gtk_list_store_set(sensors, &row, SENSOR_GROUP, shown(group).c_str(), SENSOR_NAME, shown(name).c_str(),
        SENSOR_READING, reading.c_str(), SENSOR_LIMIT, limit.c_str(), SENSOR_SOURCE, shown(source).c_str(), -1);
    seen.insert_or_assign(key, row);
  }

  void sample_sensors() {
    if (!sensors) return;
    if (!sensors_discovered) {
      sensor_sources = discover_sensors();
      sensors_discovered = true;
    }
    std::unordered_map<std::string, GtkTreeIter> seen;
    int temperatures = 0, cpu_temperatures = 0;
    for (const auto& sensor : sensor_sources) {
      std::string reading = "Unavailable", limits;
      if (sensor.kind == SensorSource::Temperature) {
        if (const auto celsius = read_celsius(sensor.mib, sensor.scale)) reading = std::format("{:.1f} °C", *celsius);
        for (const auto& limit : sensor.limits) {
          const auto celsius = read_celsius(limit.mib, limit.scale);
          if (!celsius) continue;
          if (!limits.empty()) limits += " · ";
          limits += std::format("{} {:.1f} °C", limit.label, *celsius);
        }
        ++temperatures;
        if (sensor.order == 0) ++cpu_temperatures;
      } else if (const auto rpm = read_int(sensor.mib); rpm && *rpm >= 0) {
        reading = std::format("{} RPM", *rpm);
      }
      set_sensor_row(seen, sensor.oid, sensor.group, sensor.label, reading, limits, sensor.oid);
    }

    // Power state, following the battery/AC/frequency summary of sensors(8).
    int frequency = 0;
    if (sysctl_value("dev.cpu.0.freq", frequency) && frequency > 0)
      set_sensor_row(seen, "dev.cpu.0.freq", "Power", "CPU frequency", std::format("{} MHz", frequency), "",
          "dev.cpu.0.freq");
    if (const auto lowest = sysctl_text("hw.acpi.cpu.cx_lowest"); !lowest.empty())
      set_sensor_row(seen, "hw.acpi.cpu.cx_lowest", "Power", "Lowest C-state", shown(lowest), "",
          "hw.acpi.cpu.cx_lowest");
    if (const auto usage = sysctl_text("dev.cpu.0.cx_usage"); !usage.empty())
      set_sensor_row(seen, "dev.cpu.0.cx_usage", "Power", "C-state usage (CPU 0)", shown(usage), "",
          "dev.cpu.0.cx_usage");
    int acline = 0;
    if (sysctl_value("hw.acpi.acline", acline))
      set_sensor_row(seen, "hw.acpi.acline", "Power", "AC power", acline ? "Connected" : "Disconnected", "",
          "hw.acpi.acline");
    if (const gint64 now = g_get_monotonic_time(); !battery_read_time || now - battery_read_time >= 30 * G_USEC_PER_SEC) {
      battery_state = battery.read();
      battery_read_time = now;
    }
    if (const auto& state = battery_state) {
      std::string text = state->percent ? std::format("{}%", *state->percent) : "Capacity unknown";
      if (state->charging) text += " · charging";
      if (state->minutes && *state->minutes >= 0)
        text += std::format(" · {}:{:02} remaining", *state->minutes / 60, *state->minutes % 60);
      set_sensor_row(seen, "hw.acpi.battery", "Power", "Battery", text, "", "hw.acpi.battery.life");
    }
    set_sensor_row(seen, "powerd", "Power", "Power daemon",
        power_daemon.empty() ? "Not running" : power_daemon + " running", "", "powerd(8) / powerd++(8)");

    for (auto& [key, row] : sensor_rows)
      if (!seen.contains(key)) gtk_list_store_remove(sensors, &row);
    sensor_rows = std::move(seen);

    if (!sensor_status) return;
    std::string text = temperatures ?
        std::format("{} temperature sensor{} · refresh every 2 seconds.", temperatures, temperatures == 1 ? "" : "s") :
        std::string("No temperature sensors found.");
    if (!cpu_temperatures) text += " Load coretemp(4) or amdtemp(4) for CPU temperatures.";
    text += " Drive temperatures that the kernel does not publish through sysctl are not shown.";
    gtk_label_set_text(GTK_LABEL(sensor_status), text.c_str());
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
      // Mount names come from whoever mounted the file system, which includes
      // unprivileged users when vfs.usermount is enabled.
      const auto device = shown(std::string_view(entry.f_mntfromname, strnlen(entry.f_mntfromname, MNAMELEN)));
      const auto mount_point = shown(std::string_view(entry.f_mntonname, strnlen(entry.f_mntonname, MNAMELEN)));
      const auto fs_type = shown(std::string_view(entry.f_fstypename, strnlen(entry.f_fstypename, MFSNAMELEN)));
      GtkTreeIter row;
      gtk_list_store_append(mounts, &row);
      gtk_list_store_set(mounts, &row, DEVICE, device.c_str(), MOUNT, mount_point.c_str(),
          TYPE, fs_type.c_str(), TOTAL, total_text.c_str(), USED, used_text.c_str(),
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
