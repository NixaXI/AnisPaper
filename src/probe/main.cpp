#include <wayland-client.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr uint32_t kArgb8888 = WL_SHM_FORMAT_ARGB8888;
constexpr uint32_t kAnchorAll = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                              ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                              ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                              ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
constexpr size_t kMaxBufferBytes = 512U * 1024U * 1024U;

struct Output {
  uint32_t global_name{};
  wl_output* object{};
  std::string name;
};

struct Buffer {
  int fd{-1};
  void* data{MAP_FAILED};
  size_t bytes{};
  wl_shm_pool* pool{};
  wl_buffer* object{};

  ~Buffer() {
    if (object != nullptr) wl_buffer_destroy(object);
    if (pool != nullptr) wl_shm_pool_destroy(pool);
    if (data != MAP_FAILED) munmap(data, bytes);
    if (fd >= 0) close(fd);
  }
};

struct App {
  wl_display* display{};
  wl_registry* registry{};
  wl_compositor* compositor{};
  wl_shm* shm{};
  zwlr_layer_shell_v1* layer_shell{};
  std::vector<std::unique_ptr<Output>> outputs;
  wl_surface* surface{};
  zwlr_layer_surface_v1* layer_surface{};
  std::unique_ptr<Buffer> buffer;
  std::string run_id;
  bool configure_received{};
  bool buffer_created{};
  bool buffer_attached{};
  bool buffer_committed{};
  bool buffer_flushed{};
  bool compositor_closed{};
  bool transport_failed{};
  std::string transport_stage;
  int transport_errno{};
  uint32_t layer_shell_version{};
};

void log(std::string_view event, std::string_view detail = {}) {
  std::cout << "ANISPAPER_PROBE event=" << event;
  if (!detail.empty()) std::cout << ' ' << detail;
  std::cout << '\n' << std::flush;
}

std::string safe_value(std::string value) {
  std::replace_if(value.begin(), value.end(), [](unsigned char c) {
    return c == ' ' || c == '\n' || c == '\r' || c == '\t';
  }, '_');
  return value;
}

std::string make_run_id() {
  const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  char result[64];
  std::snprintf(result, sizeof(result), "%llX-%X",
                static_cast<unsigned long long>(millis), static_cast<unsigned>(getpid()));
  return result;
}

uint32_t glyph(char c, int row) {
  // Five-pixel-wide glyph rows. The small built-in font avoids a renderer
  // dependency while keeping the run identifier visible in a screenshot.
  const auto rows = [&]() -> std::array<uint8_t, 7> {
    switch (c) {
      case '0': return {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e};
      case '1': return {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e};
      case '2': return {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f};
      case '3': return {0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e};
      case '4': return {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02};
      case '5': return {0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e};
      case '6': return {0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e};
      case '7': return {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
      case '8': return {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e};
      case '9': return {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c};
      case 'A': return {0x04, 0x0a, 0x11, 0x11, 0x1f, 0x11, 0x11};
      case 'E': return {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f};
      case 'F': return {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10};
      case 'I': return {0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e};
      case 'N': return {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
      case 'P': return {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10};
      case 'R': return {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11};
      case 'S': return {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e};
      case 'U': return {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e};
      case '-': return {0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00};
      default: return {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    }
  }();
  return rows[static_cast<size_t>(row)];
}

void put_pixel(uint32_t* pixels, int width, int height, int x, int y, uint32_t color) {
  if (x >= 0 && x < width && y >= 0 && y < height) pixels[y * width + x] = color;
}

void draw_text(uint32_t* pixels, int width, int height, int x, int y,
               std::string_view text, int scale, uint32_t color) {
  for (const char c : text) {
    for (int row = 0; row < 7; ++row) {
      const uint32_t bits = glyph(c, row);
      for (int column = 0; column < 5; ++column) {
        if ((bits & (1U << (4 - column))) == 0) continue;
        for (int sy = 0; sy < scale; ++sy) {
          for (int sx = 0; sx < scale; ++sx) {
            put_pixel(pixels, width, height, x + column * scale + sx,
                      y + row * scale + sy, color);
          }
        }
      }
    }
    x += 6 * scale;
  }
}

std::unique_ptr<Buffer> create_buffer(App& app, int width, int height) {
  if (width <= 0 || height <= 0) return nullptr;
  const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
  if (pixels > kMaxBufferBytes / sizeof(uint32_t)) return nullptr;
  const size_t bytes = pixels * sizeof(uint32_t);
  const char* runtime = std::getenv("XDG_RUNTIME_DIR");
  std::string template_path = std::string(runtime != nullptr ? runtime : "/tmp") + "/anispaper-f0-XXXXXX";
  std::vector<char> path(template_path.begin(), template_path.end());
  path.push_back('\0');
  const int fd = mkstemp(path.data());
  if (fd < 0 || ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
    if (fd >= 0) close(fd);
    return nullptr;
  }
  unlink(path.data());
  void* mapped = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mapped == MAP_FAILED) {
    close(fd);
    return nullptr;
  }
  auto result = std::make_unique<Buffer>();
  result->fd = fd;
  result->data = mapped;
  result->bytes = bytes;
  result->pool = wl_shm_create_pool(app.shm, fd, static_cast<int>(bytes));
  result->object = wl_shm_pool_create_buffer(result->pool, 0, width, height, width * 4,
                                             kArgb8888);
  if (result->pool == nullptr || result->object == nullptr) return nullptr;

  auto* data = static_cast<uint32_t*>(mapped);
  std::fill(data, data + pixels, 0xff101622U); // ANIS STAR dark-blue, opaque ARGB.
  const int box_width = std::min(width - 32, 6 * 6 * static_cast<int>(app.run_id.size() + 7) + 32);
  const int box_height = std::min(height - 32, 100);
  for (int py = 16; py < 16 + box_height; ++py) {
    for (int px = 16; px < 16 + box_width; ++px) put_pixel(data, width, height, px, py, 0xff0a0d14U);
  }
  draw_text(data, width, height, 32, 30, "F0 RUN", 4, 0xffffd000U);
  draw_text(data, width, height, 32, 62, app.run_id, 3, 0xff00c2ffU);
  return result;
}

void output_geometry(void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t,
                     const char*, const char*, int32_t) {}
void output_mode(void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) {}
void output_done(void*, wl_output*) {}
void output_scale(void*, wl_output*, int32_t) {}
void output_name(void* data, wl_output*, const char* name) {
  static_cast<Output*>(data)->name = name != nullptr ? name : "";
}
void output_description(void*, wl_output*, const char*) {}
constexpr wl_output_listener kOutputListener = {
    output_geometry, output_mode, output_done, output_scale, output_name, output_description};

void layer_configure(void* data, zwlr_layer_surface_v1* layer_surface, uint32_t serial,
                     uint32_t width, uint32_t height) {
  auto& app = *static_cast<App*>(data);
  zwlr_layer_surface_v1_ack_configure(layer_surface, serial);
  if (app.configure_received) return;
  app.configure_received = true;
  log("surface", "status=configure_received layer=background width=" + std::to_string(width) +
                     " height=" + std::to_string(height) + " run_id=" + app.run_id);
  app.buffer = create_buffer(app, static_cast<int>(width), static_cast<int>(height));
  if (!app.buffer) {
    log("surface", "status=buffer_failed width=" + std::to_string(width) +
                       " height=" + std::to_string(height));
    return;
  }
  app.buffer_created = true;
  log("surface", "status=buffer_created width=" + std::to_string(width) +
                     " height=" + std::to_string(height) + " run_id=" + app.run_id);
  wl_surface_attach(app.surface, app.buffer->object, 0, 0);
  app.buffer_attached = true;
  wl_surface_damage(app.surface, 0, 0, static_cast<int>(width), static_cast<int>(height));
  wl_surface_commit(app.surface);
  app.buffer_committed = true;
  log("surface", "status=buffer_committed layer=background width=" + std::to_string(width) +
                     " height=" + std::to_string(height) + " run_id=" + app.run_id +
                     " presentation=not_observable");
}
void layer_closed(void* data, zwlr_layer_surface_v1*) {
  static_cast<App*>(data)->compositor_closed = true;
  log("surface", "status=closed_by_compositor");
}
constexpr zwlr_layer_surface_v1_listener kLayerListener = {layer_configure, layer_closed};

void registry_global(void* data, wl_registry* registry, uint32_t name,
                     const char* interface, uint32_t version) {
  auto& app = *static_cast<App*>(data);
  const std::string_view iface(interface != nullptr ? interface : "");
  if (iface == wl_compositor_interface.name) {
    app.compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name,
        &wl_compositor_interface, std::min(version, 4U)));
  } else if (iface == wl_shm_interface.name) {
    app.shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
  } else if (iface == zwlr_layer_shell_v1_interface.name) {
    app.layer_shell_version = std::min(version, 4U);
    app.layer_shell = static_cast<zwlr_layer_shell_v1*>(wl_registry_bind(registry, name,
        &zwlr_layer_shell_v1_interface, app.layer_shell_version));
    log("global", "protocol=zwlr_layer_shell_v1 status=advertised version=" +
                      std::to_string(version));
  } else if (iface == wl_output_interface.name) {
    auto output = std::make_unique<Output>();
    output->global_name = name;
    output->object = static_cast<wl_output*>(wl_registry_bind(registry, name,
        &wl_output_interface, std::min(version, 4U)));
    wl_output_add_listener(output->object, &kOutputListener, output.get());
    app.outputs.push_back(std::move(output));
  }
}
void registry_remove(void*, wl_registry*, uint32_t) {}
constexpr wl_registry_listener kRegistryListener = {registry_global, registry_remove};

void mark_transport_error(App& app, std::string_view stage) {
  if (app.transport_failed) return;
  app.transport_failed = true;
  app.transport_stage = stage;
  app.transport_errno = errno;
  log("transport", "status=error stage=" + app.transport_stage +
                       " errno=" + std::to_string(app.transport_errno) +
                       " message=" + safe_value(std::strerror(app.transport_errno)));
}

std::string finish_status(const App& app) {
  if (app.transport_failed) return "transport_error";
  if (app.compositor_closed) return "compositor_closed";
  if (!app.configure_received) return "not_configured";
  if (!app.buffer_created) return "buffer_failed";
  if (!app.buffer_attached) return "attach_failed";
  if (!app.buffer_committed) return "commit_failed";
  if (!app.buffer_flushed) return "commit_unflushed";
  return "timeout_clean";
}

bool probe_succeeded(const App& app) {
  // Wayland has no event that proves pixels became visibly composited here.
  // Success means only a valid configure plus a buffer attach/commit completed
  // without a transport failure or compositor close during the finite probe.
  return app.configure_received && app.buffer_created && app.buffer_attached &&
         app.buffer_committed && app.buffer_flushed && !app.compositor_closed &&
         !app.transport_failed;
}

void destroy(App& app) {
  app.buffer.reset();
  if (app.layer_surface != nullptr) zwlr_layer_surface_v1_destroy(app.layer_surface);
  if (app.surface != nullptr) wl_surface_destroy(app.surface);
  for (auto& output : app.outputs) {
    if (output->object != nullptr) wl_output_destroy(output->object);
  }
  if (app.layer_shell != nullptr) zwlr_layer_shell_v1_destroy(app.layer_shell);
  if (app.shm != nullptr) wl_shm_destroy(app.shm);
  if (app.compositor != nullptr) wl_compositor_destroy(app.compositor);
  if (app.registry != nullptr) wl_registry_destroy(app.registry);
  if (app.display != nullptr) wl_display_disconnect(app.display);
}

void usage(const char* executable) {
  std::cout << "Usage: " << executable << " [--output NAME] [--timeout-ms N]\n"
            << "Creates one zwlr_layer_shell_v1 background surface with a visible F0 run ID.\n";
}

std::optional<int> parse_timeout(const char* value) {
  char* end = nullptr;
  errno = 0;
  const long parsed = std::strtol(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || parsed < 100 || parsed > 60000) return std::nullopt;
  return static_cast<int>(parsed);
}

}  // namespace

int main(int argc, char** argv) {
  std::string requested_output;
  int timeout_ms = 8000;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--help") {
      usage(argv[0]);
      return 0;
    }
    if (argument == "--output" && index + 1 < argc) {
      requested_output = argv[++index];
      continue;
    }
    if (argument == "--timeout-ms" && index + 1 < argc) {
      const auto parsed = parse_timeout(argv[++index]);
      if (parsed.has_value()) {
        timeout_ms = *parsed;
        continue;
      }
    }
    std::cerr << "Invalid argument: " << argument << '\n';
    usage(argv[0]);
    return 64;
  }

  App app;
  app.run_id = make_run_id();
  log("start", "run_id=" + app.run_id + " timeout_ms=" + std::to_string(timeout_ms) +
                   " requested_output=" + (requested_output.empty() ? "auto" : safe_value(requested_output)));
  app.display = wl_display_connect(nullptr);
  if (app.display == nullptr) {
    log("connect", "status=failed errno=" + std::to_string(errno) + " message=" + safe_value(std::strerror(errno)));
    return 2;
  }
  log("connect", "status=ok");
  app.registry = wl_display_get_registry(app.display);
  wl_registry_add_listener(app.registry, &kRegistryListener, &app);
  if (wl_display_roundtrip(app.display) < 0 || wl_display_roundtrip(app.display) < 0) {
    log("registry", "status=failed errno=" + std::to_string(errno) + " message=" + safe_value(std::strerror(errno)));
    destroy(app);
    return 2;
  }
  if (app.layer_shell == nullptr) {
    log("global", "protocol=zwlr_layer_shell_v1 status=not_advertised");
    destroy(app);
    return 3;
  }
  if (app.compositor == nullptr || app.shm == nullptr) {
    log("registry", "status=missing_required_global compositor=" + std::to_string(app.compositor != nullptr) +
                        " shm=" + std::to_string(app.shm != nullptr));
    destroy(app);
    return 3;
  }

  wl_output* target_output = nullptr;
  if (!requested_output.empty()) {
    for (const auto& output : app.outputs) {
      if (output->name == requested_output) {
        target_output = output->object;
        break;
      }
    }
    if (target_output == nullptr) {
      log("output", "status=not_found requested=" + safe_value(requested_output));
      for (const auto& output : app.outputs) {
        log("output", "status=available name=" + safe_value(output->name));
      }
      destroy(app);
      return 4;
    }
  }
  for (const auto& output : app.outputs) {
    log("output", "status=available name=" + safe_value(output->name));
  }

  app.surface = wl_compositor_create_surface(app.compositor);
  app.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
      app.layer_shell, app.surface, target_output, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "anispaper-f0-probe");
  zwlr_layer_surface_v1_add_listener(app.layer_surface, &kLayerListener, &app);
  zwlr_layer_surface_v1_set_anchor(app.layer_surface, kAnchorAll);
  zwlr_layer_surface_v1_set_size(app.layer_surface, 0, 0);
  // A wallpaper must not reserve work area; it is strictly a visual probe.
  zwlr_layer_surface_v1_set_exclusive_zone(app.layer_surface, 0);
  zwlr_layer_surface_v1_set_keyboard_interactivity(app.layer_surface,
                                                    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
  wl_surface_commit(app.surface);
  log("surface", "status=requested layer=background protocol_version=" +
                     std::to_string(app.layer_shell_version) + " run_id=" + app.run_id);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!app.compositor_closed && !app.transport_failed &&
         std::chrono::steady_clock::now() < deadline) {
    if (wl_display_dispatch_pending(app.display) < 0) {
      mark_transport_error(app, "dispatch_pending");
      break;
    }
    const int flush_result = wl_display_flush(app.display);
    const bool waiting_for_write = flush_result < 0 && errno == EAGAIN;
    if (flush_result < 0 && !waiting_for_write) {
      mark_transport_error(app, "flush");
      break;
    }
    if (flush_result >= 0 && app.buffer_committed) app.buffer_flushed = true;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    if (remaining <= 0) break;
    pollfd descriptor{wl_display_get_fd(app.display),
                      static_cast<short>(POLLIN | (waiting_for_write ? POLLOUT : 0)), 0};
    const int ready = poll(&descriptor, 1, static_cast<int>(remaining));
    if (ready > 0) {
      if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        mark_transport_error(app, "poll_revents");
        break;
      }
      if ((descriptor.revents & POLLIN) != 0 && wl_display_dispatch(app.display) < 0) {
        mark_transport_error(app, "dispatch");
        break;
      }
    } else if (ready < 0 && errno != EINTR) {
      mark_transport_error(app, "poll");
      break;
    }
  }
  const std::string status = finish_status(app);
  log("finish", "status=" + status +
                    " configure_received=" + std::to_string(app.configure_received) +
                    " buffer_created=" + std::to_string(app.buffer_created) +
                    " buffer_attached=" + std::to_string(app.buffer_attached) +
                    " buffer_committed=" + std::to_string(app.buffer_committed) +
                    " buffer_flushed=" + std::to_string(app.buffer_flushed) +
                    " compositor_closed=" + std::to_string(app.compositor_closed) +
                    " transport_failed=" + std::to_string(app.transport_failed) +
                    " presentation=not_observable run_id=" + app.run_id);
  const bool success = probe_succeeded(app);
  destroy(app);
  return success ? 0 : 5;
}
