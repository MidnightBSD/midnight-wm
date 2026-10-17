#include <xcb/xcb.h>

#include <cstdint>
#include <cstdlib>

// Send a synthetic root ConfigureNotify to exercise the window manager's
// resize path on Xvfb, whose RandR output exposes only its initial mode.
int main(int argc, char** argv) {
  if (argc != 3) return 2;
  const int width = std::atoi(argv[1]);
  const int height = std::atoi(argv[2]);
  if (width < 1 || width > 65535 || height < 1 || height > 65535) return 2;
  auto* connection = xcb_connect(nullptr, nullptr);
  if (!connection || xcb_connection_has_error(connection)) return 1;
  auto root = xcb_setup_roots_iterator(xcb_get_setup(connection));
  if (!root.data) { xcb_disconnect(connection); return 1; }
  xcb_configure_notify_event_t event{};
  event.response_type = XCB_CONFIGURE_NOTIFY;
  event.event = root.data->root;
  event.window = root.data->root;
  event.width = static_cast<std::uint16_t>(width);
  event.height = static_cast<std::uint16_t>(height);
  xcb_send_event(connection, 0, root.data->root, XCB_EVENT_MASK_STRUCTURE_NOTIFY,
                 reinterpret_cast<const char*>(&event));
  xcb_flush(connection);
  const int result = xcb_connection_has_error(connection) ? 1 : 0;
  xcb_disconnect(connection);
  return result;
}
