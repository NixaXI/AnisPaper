#include "plasma_wallpaper_activator.h"

#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScreen>

#include "kde-output-order-v1-client-protocol.h"

#include <cstdio>
#include <algorithm>
#include <cstring>

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
}  // namespace

int main(int argc, char **argv) {
  QGuiApplication app(argc, argv);
  QStringList outputOrder;
  QString error;
  if (!rawKWinOutputOrder(&outputOrder, &error)) {
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
