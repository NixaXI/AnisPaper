#include "../src/daemon/plasma_wallpaper_activator.h"

#include <QJsonObject>

#include <cstdio>

namespace {
struct FakeTransport final : PlasmaWallpaperTransport {
  QVector<PlasmaScreenMapping> mappings;
  QVector<PlasmaWallpaperState> wallpapers;
  int mutations = 0;
  bool failMappings = false;
  bool refuseMutation = false;
  bool mutateOther = false;

  bool screenMappings(QVector<PlasmaScreenMapping> *result, QString *error) override {
    if (failMappings) {
      if (error) *error = QStringLiteral("authoritative mapping source unavailable");
      return false;
    }
    *result = mappings;
    return true;
  }
  bool wallpaper(quint32 screen, PlasmaWallpaperState *result, QString *error) override {
    if (screen >= static_cast<quint32>(wallpapers.size())) {
      if (error) *error = QStringLiteral("unknown fake screen");
      return false;
    }
    *result = wallpapers[static_cast<int>(screen)];
    return true;
  }
  bool setWallpaper(const QString &plugin, const QVariantMap &parameters, quint32 screen,
                    QString *error) override {
    ++mutations;
    if (refuseMutation) return true;
    if (screen >= static_cast<quint32>(wallpapers.size())) {
      if (error) *error = QStringLiteral("unknown fake screen");
      return false;
    }
    QVariantMap readback = parameters;
    if (plugin == QStringLiteral("org.anispaper.frame")) {
      readback.insert(QStringLiteral("ImageDefault"), QStringLiteral("Plasma supplied default"));
    }
    wallpapers[static_cast<int>(screen)] = {plugin, readback};
    if (mutateOther && wallpapers.size() > 1) wallpapers[1] = {QStringLiteral("bad"), {}};
    return true;
  }
};

struct FakeMapRunner final : PlasmaOutputMapRunner {
  QString output;
  bool fail = false;

  bool run(QString *stdoutText, QString *error) override {
    if (fail) {
      if (error) *error = QStringLiteral("fake map helper failure");
      return false;
    }
    *stdoutText = output;
    return true;
  }
};

int check(bool ok, const char *name) {
  std::fprintf(ok ? stdout : stderr, "%s: %s\n", ok ? "PASS" : "FAIL", name);
  return ok ? 0 : 1;
}

PlasmaWallpaperState state(const QString &plugin, const QString &marker) {
  return {plugin, {{QStringLiteral("wallpaperPlugin"), plugin},
                   {QStringLiteral("marker"), marker}}};
}

void reset(FakeTransport *transport) {
  transport->mappings = {{QStringLiteral("HDMI-A-1"), 0}, {QStringLiteral("DP-2"), 1}};
  transport->wallpapers = {state(QStringLiteral("org.kde.image"), QStringLiteral("hdmi")),
                           state(QStringLiteral("org.kde.image"), QStringLiteral("dp"))};
  transport->mutations = 0;
  transport->failMappings = false;
  transport->refuseMutation = false;
  transport->mutateOther = false;
}
}  // namespace

int main() {
  int failures = 0;
  FakeTransport transport;
  reset(&transport);
  PlasmaWallpaperActivator activator(&transport);
  PlasmaActivationPlan plan;
  QString error;
  const QJsonArray outputs{QJsonObject{{QStringLiteral("name"), QStringLiteral("HDMI-A-1")}},
                           QJsonObject{{QStringLiteral("name"), QStringLiteral("DP-2")}}};

  failures += check(activator.preflight(QStringLiteral("HDMI-A-1"), QStringLiteral("fit"),
                                        &plan, &error) && plan.screenNumber == 0,
                    "exact HDMI-A-1 maps to its typed Plasma screen");
  const PlasmaWallpaperState dpBefore = transport.wallpapers[1];
  failures += check(activator.commit(plan, &error) && transport.mutations == 1,
                    "target-only typed setWallpaper commits HDMI-A-1");
  failures += check(transport.wallpapers[0].plugin == QStringLiteral("org.anispaper.frame") &&
                        transport.wallpapers[0].parameters.value(QStringLiteral("Output")) ==
                            QStringLiteral("HDMI-A-1") &&
                        transport.wallpapers[0].parameters.value(QStringLiteral("ScaleMode")) ==
                            QStringLiteral("fit") &&
                        transport.wallpapers[1] == dpBefore,
                    "postcondition carries exact properties and leaves DP-2 unchanged");

  reset(&transport);
  failures += check(activator.preflight(QStringLiteral("DP-2"), QStringLiteral("cover"),
                                        &plan, &error) && plan.screenNumber == 1 &&
                        activator.commit(plan, &error) && transport.wallpapers[0].plugin ==
                            QStringLiteral("org.kde.image"),
                    "DP-2 activates independently without changing HDMI-A-1");

  reset(&transport);
  transport.mappings = {{QStringLiteral("DP-2"), 1}};
  failures += check(!activator.preflight(QStringLiteral("HDMI-A-1"), QStringLiteral("cover"),
                                         &plan, &error) && transport.mutations == 0,
                    "zero exact mapping fails before mutation");
  reset(&transport);
  transport.mappings = {{QStringLiteral("HDMI-A-1"), 0}, {QStringLiteral("HDMI-A-1"), 1}};
  failures += check(!activator.preflight(QStringLiteral("HDMI-A-1"), QStringLiteral("cover"),
                                         &plan, &error) && transport.mutations == 0,
                    "duplicate mapping fails before mutation");
  reset(&transport);
  failures += check(activator.preflight(QStringLiteral("HDMI-A-1"), QStringLiteral("cover"),
                                        &plan, &error),
                    "stale mapping preflight setup");
  transport.mappings = {{QStringLiteral("HDMI-A-1"), 1}, {QStringLiteral("DP-2"), 0}};
  failures += check(!activator.commit(plan, &error) && transport.mutations == 0 &&
                        error.contains(QStringLiteral("mapping changed")),
                    "stale mapping is rechecked before mutation");

  reset(&transport);
  transport.refuseMutation = true;
  const PlasmaWallpaperState targetBefore = transport.wallpapers[0];
  failures += check(activator.preflight(QStringLiteral("HDMI-A-1"), QStringLiteral("cover"),
                                        &plan, &error) && !activator.commit(plan, &error) &&
                        error.contains(QStringLiteral("postcondition")) &&
                        transport.wallpapers[0] == targetBefore && transport.mutations == 2,
                    "failed postcondition rolls target snapshot back");

  reset(&transport);
  transport.mutateOther = true;
  failures += check(activator.preflight(QStringLiteral("HDMI-A-1"), QStringLiteral("cover"),
                                        &plan, &error) && !activator.commit(plan, &error) &&
                        error.contains(QStringLiteral("non-target")) && transport.mutations == 2,
                    "non-target change fails closed and rolls target back");

  reset(&transport);
  transport.failMappings = true;
  failures += check(!activator.preflight(QStringLiteral("HDMI-A-1"), QStringLiteral("cover"),
                                         &plan, &error) && error.contains(QStringLiteral("unavailable")),
                    "authoritative mapping source unavailability fails closed");

  FakeMapRunner mapRunner;
  mapRunner.output = QStringLiteral(
      "[{\"connector\":\"DP-2\",\"screenNumber\":0},"
      "{\"connector\":\"HDMI-A-1\",\"screenNumber\":1}]");
  PlasmaDbusTransport dbusTransport(&mapRunner);
  QVector<PlasmaScreenMapping> parsed;
  failures += check(dbusTransport.screenMappings(&parsed, &error) && parsed.size() == 2 &&
                        parsed[0] == PlasmaScreenMapping{QStringLiteral("DP-2"), 0} &&
                        parsed[1] == PlasmaScreenMapping{QStringLiteral("HDMI-A-1"), 1},
                    "injected helper JSON parser preserves exact typed screen mappings");
  mapRunner.output = QStringLiteral("[{\"connector\":\"HDMI-A-1\",\"screenNumber\":1.5}]");
  failures += check(!dbusTransport.screenMappings(&parsed, &error) &&
                        error.contains(QStringLiteral("invalid mapping")),
                    "helper parser rejects non-integral screen numbers");

  QVector<PlasmaOutputDescriptor> helperScreens{
      {QStringLiteral("HDMI-A-1"), QRect(0, 0, 1920, 1080)},
      {QStringLiteral("DP-2"), QRect(1920, 0, 1920, 1080)}};
  failures += check(PlasmaDbusTransport::validateOutputTopology(
                        {QStringLiteral("DP-2"), QStringLiteral("HDMI-A-1")}, helperScreens,
                        &parsed, &error) && parsed[0].connector == QStringLiteral("DP-2") &&
                        parsed[0].screenNumber == 0,
                    "helper topology maps KWin order indexes only after exact one-to-one validation");
  helperScreens[1].geometry = QRect(0, 0, 1920, 1080);
  failures += check(!PlasmaDbusTransport::validateOutputTopology(
                        {QStringLiteral("DP-2"), QStringLiteral("HDMI-A-1")}, helperScreens,
                        &parsed, &error) && error.contains(QStringLiteral("overlapping")),
                    "helper topology rejects mirrored or overlapping screens");

  const QVector<PlasmaScreenMapping> exactMappings{{QStringLiteral("HDMI-A-1"), 1},
                                                    {QStringLiteral("DP-2"), 0}};
  failures += check(PlasmaWallpaperActivator::mappingsMatchWaylandOutputs(
                        exactMappings, outputs, &error),
                    "helper connectors must exactly match the daemon Wayland inventory");
  const QVector<PlasmaScreenMapping> missingMappings{{QStringLiteral("HDMI-A-1"), 1}};
  failures += check(!PlasmaWallpaperActivator::mappingsMatchWaylandOutputs(
                        missingMappings, outputs, &error) &&
                        error.contains(QStringLiteral("exactly match")),
                    "missing helper connector rejects physical activation before renderer apply");
  const QJsonArray unknownOutputs{
      QJsonObject{{QStringLiteral("name"), QStringLiteral("HDMI-A-1")}},
      QJsonObject{{QStringLiteral("name"), QStringLiteral("DP-2")}},
      QJsonObject{{QStringLiteral("name"), QStringLiteral("UNKNOWN-9")}}};
  failures += check(!PlasmaWallpaperActivator::mappingsMatchWaylandOutputs(
                        exactMappings, unknownOutputs, &error) &&
                        error.contains(QStringLiteral("exactly match")),
                    "unknown daemon connector rejects physical activation before renderer apply");

  reset(&transport);
  const QString escapedOutput = QStringLiteral("HDMI-\"A\\1");
  transport.mappings = {{escapedOutput, 0}, {QStringLiteral("DP-2"), 1}};
  failures += check(activator.preflight(escapedOutput, QStringLiteral("stretch"), &plan, &error) &&
                        activator.commit(plan, &error) &&
                        transport.wallpapers[0].parameters.value(QStringLiteral("Output")) == escapedOutput,
                    "typed properties preserve quote and backslash connector values without script escaping");

  failures += check(PlasmaWallpaperActivator::isCanonicalOutputRequest(QStringLiteral("HDMI-A-1")) &&
                        !PlasmaWallpaperActivator::isCanonicalOutputRequest(
                            QStringLiteral(" HDMI-A-1 ")) &&
                        PlasmaWallpaperActivator::connectedOutputIdentity(
                            QStringLiteral("HDMI-A-1"), outputs) == QStringLiteral("HDMI-A-1") &&
                        PlasmaWallpaperActivator::connectedOutputIdentity(
                            QStringLiteral("__anispaper-ui-preview__"), outputs).isEmpty(),
                    "synthetic and noncanonical outputs remain renderer-only");
  return failures == 0 ? 0 : 1;
}
