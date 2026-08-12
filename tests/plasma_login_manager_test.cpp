#include "../src/daemon/plasma_login_manager.h"

#include <QCoreApplication>
#include <QHash>
#include <QTextStream>

namespace {
int failures = 0;

void check(const bool condition, const char *message)
{
  if (condition) {
    return;
  }
  QTextStream(stderr) << "FAIL: " << message << '\n';
  ++failures;
}
} // namespace

int main(int argc, char **argv)
{
  QCoreApplication app(argc, argv);
  qunsetenv("ANISPAPER_DISPLAY_MANAGER_UNIT");
  qunsetenv("ANISPAPER_TEST_DISPLAY_MANAGER_UNIT");

  const auto plasmalogin = detectDisplayManager([](const QString &path) {
    return path.startsWith(QStringLiteral("/run/"))
        ? QStringLiteral("/usr/lib/systemd/system/plasmalogin.service")
        : QString();
  });
  check(plasmalogin.kind == DisplayManagerKind::PlasmaLogin,
        "runtime display-manager alias detects Plasma Login");
  check(plasmalogin.unit == QStringLiteral("plasmalogin.service"),
        "detected unit is basename-safe");
  check(displayManagerId(plasmalogin.kind) == QStringLiteral("plasmalogin"),
        "Plasma Login manager id is stable");

  const auto sddm = detectDisplayManager([](const QString &) {
    return QStringLiteral("/usr/lib/systemd/system/sddm.service");
  });
  check(sddm.kind == DisplayManagerKind::Sddm,
        "SDDM alias dispatch remains available");
  check(displayManagerId(sddm.kind) == QStringLiteral("sddm"),
        "SDDM manager id is stable");

  const auto missing = detectDisplayManager([](const QString &) { return QString(); });
  check(missing.kind == DisplayManagerKind::Unknown,
        "missing display-manager alias is reported as unknown");
  check(displayManagerId(missing.kind) == QStringLiteral("unknown"),
        "unknown manager id is explicit");

  qputenv("ANISPAPER_DISPLAY_MANAGER_UNIT", QByteArray("sddm.service;plasmalogin.service"));
  const auto hostile = detectDisplayManager([](const QString &) {
    return QStringLiteral("/usr/lib/systemd/system/plasmalogin.service");
  });
  check(hostile.kind == DisplayManagerKind::Unknown,
        "unit override is basename/argument-safe and rejects compound values");
  qunsetenv("ANISPAPER_DISPLAY_MANAGER_UNIT");

  QTextStream(stdout) << (failures == 0 ? "plasma_login_manager: PASS\n"
                                       : "plasma_login_manager: FAIL\n");
  return failures == 0 ? 0 : 1;
}
