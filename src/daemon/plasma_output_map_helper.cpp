#include "plasma_wallpaper_activator.h"

#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScreen>

#include "kde-output-order-v1-client-protocol.h"

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <vector>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xrandr.h>
#undef Unsorted
#undef None
#undef Success

namespace {
struct RawOrder {
  wl_registry *registry = nullptr;
  kde_output_order_v1 *order = nullptr;
  QStringList names;
  bool done = false;
};

void outputName(void *data, kde_output_order_v1 *, const char *name) {
  auto *state = static_cast<RawOrder *>(data);
  // A later output event starts a replacement list; only accept the list
  // terminated by the final done event observed before validation.
  if (state->done) {
    state->names.clear();
    state->done = false;
  }
  state->names.push_back(QString::fromUtf8(name ? name : ""));
}

void done(void *data, kde_output_order_v1 *) { static_cast<RawOrder *>(data)->done = true; }

constexpr kde_output_order_v1_listener kOrderListener{outputName, done};

void global(void *data, wl_registry *registry, uint32_t name, const char *interface,
            uint32_t version) {
  auto *state = static_cast<RawOrder *>(data);
  if (std::strcmp(interface, kde_output_order_v1_interface.name) != 0 || state->order) return;
  state->order = static_cast<kde_output_order_v1 *>(
      wl_registry_bind(registry, name, &kde_output_order_v1_interface, std::min(version, 1U)));
  if (state->order) kde_output_order_v1_add_listener(state->order, &kOrderListener, state);
}

void globalRemove(void *, wl_registry *, uint32_t) {}

constexpr wl_registry_listener kRegistryListener{global, globalRemove};

bool rawKWinOutputOrder(QStringList *order, QString *error) {
  wl_display *display = wl_display_connect(nullptr);
  if (!display) {
    if (error) *error = QStringLiteral("could not connect to Wayland for KWin output order");
    return false;
  }
  RawOrder state;
  state.registry = wl_display_get_registry(display);
  wl_registry_add_listener(state.registry, &kRegistryListener, &state);
  const int initialSync = wl_display_roundtrip(display);
  const int doneSync = initialSync >= 0 ? wl_display_roundtrip(display) : -1;
  if (state.order) kde_output_order_v1_destroy(state.order);
  if (state.registry) wl_registry_destroy(state.registry);
  wl_display_disconnect(display);
  if (initialSync < 0 || doneSync < 0 || !state.done) {
    if (error) *error = QStringLiteral("KWin output-order protocol did not send done");
    return false;
  }
  *order = state.names;
  return true;
}

bool rawX11OutputOrder(QStringList *order, QString *error) {
  Display *display = XOpenDisplay(nullptr);
  if (!display) {
    if (error) *error = QStringLiteral("could not connect to X11 for Plasma output order");
    return false;
  }
  XRRScreenResources *resources =
      XRRGetScreenResourcesCurrent(display, DefaultRootWindow(display));
  if (!resources) {
    XCloseDisplay(display);
    if (error) *error = QStringLiteral("could not read XRandR output resources");
    return false;
  }
  const Atom indexAtom = XInternAtom(display, "_KDE_SCREEN_INDEX", True);
  std::vector<std::pair<unsigned long, QString>> indexed;
  bool valid = indexAtom != 0;
  for (int i = 0; valid && i < resources->noutput; ++i) {
    XRROutputInfo *info = XRRGetOutputInfo(display, resources, resources->outputs[i]);
    if (!info) { valid = false; break; }
    if (info->connection == RR_Connected && info->crtc != 0) {
      Atom actualType = 0;
      int actualFormat = 0;
      unsigned long count = 0, remaining = 0;
      unsigned char *value = nullptr;
      const int status = XRRGetOutputProperty(display, resources->outputs[i], indexAtom,
                                               0, 1, False, False, AnyPropertyType,
                                               &actualType, &actualFormat, &count,
                                               &remaining, &value);
      if (status != 0 || (actualType != XA_CARDINAL && actualType != XA_INTEGER) ||
          actualFormat != 32 ||
          count != 1 || !value) {
        valid = false;
      } else {
        const unsigned long screenIndex = *reinterpret_cast<unsigned long *>(value);
        // KDE uses 0 to disable an output; active Plasma screen indexes start at 1.
        if (screenIndex > 0) {
          indexed.emplace_back(screenIndex,
                                QString::fromUtf8(info->name, info->nameLen));
        }
      }
      if (value) XFree(value);
    }
    XRRFreeOutputInfo(info);
  }
  XRRFreeScreenResources(resources);
  XCloseDisplay(display);
  std::sort(indexed.begin(), indexed.end(), [](const auto &left, const auto &right) {
    return left.first < right.first;
  });
  if (!valid || indexed.empty()) {
    if (error) *error = QStringLiteral("X11 Plasma _KDE_SCREEN_INDEX mapping is unavailable or invalid");
    return false;
  }
  for (size_t i = 0; i < indexed.size(); ++i) {
    if ((i && indexed[i - 1].first == indexed[i].first) ||
        indexed[i].first != i + 1 || indexed[i].second.isEmpty()) {
      if (error) *error = QStringLiteral("X11 Plasma screen indexes are duplicated or non-contiguous");
      return false;
    }
    order->push_back(indexed[i].second);
  }
  return true;
}
}  // namespace

int main(int argc, char **argv) {
  QGuiApplication app(argc, argv);
  const QString session = qEnvironmentVariable("ANISPAPER_SESSION_TYPE",
                                                qEnvironmentVariable("XDG_SESSION_TYPE")).toLower();
  const bool x11 = session == QStringLiteral("x11") ||
                   (session.isEmpty() && !qEnvironmentVariableIsEmpty("DISPLAY") &&
                    qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"));
  QStringList outputOrder;
  QString error;
  const bool mapped = x11 ? rawX11OutputOrder(&outputOrder, &error)
                          : rawKWinOutputOrder(&outputOrder, &error);
  if (!mapped) {
    std::fputs(qPrintable(error + QLatin1Char('\n')), stderr);
    return 1;
  }
  QVector<PlasmaOutputDescriptor> screens;
  for (QScreen *screen : QGuiApplication::screens()) {
    if (!screen) {
      std::fputs("Qt returned a null screen\n", stderr);
      return 1;
    }
    screens.push_back({screen->name(), screen->geometry()});
  }
  QVector<PlasmaScreenMapping> mappings;
  if (!PlasmaDbusTransport::validateOutputTopology(outputOrder, screens, &mappings,
                                                   &error)) {
    std::fputs(qPrintable(error + QLatin1Char('\n')), stderr);
    return 1;
  }
  QJsonArray json;
  for (const PlasmaScreenMapping &mapping : mappings) {
    json.append(QJsonObject{{QStringLiteral("connector"), mapping.connector},
                            {QStringLiteral("screenNumber"),
                             static_cast<qint64>(mapping.screenNumber)}});
  }
  std::fputs(QJsonDocument(json).toJson(QJsonDocument::Compact).constData(), stdout);
  std::fputc('\n', stdout);
  return 0;
}
