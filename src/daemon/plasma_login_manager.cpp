#include "plasma_login_manager.h"

#include <QFileInfo>

namespace {

DisplayManagerKind kindForUnit(const QString &rawUnit)
{
  const QString unit = QFileInfo(rawUnit.trimmed()).fileName().toLower();
  if (unit == QStringLiteral("sddm.service") || unit == QStringLiteral("sddm")) {
    return DisplayManagerKind::Sddm;
  }
  if (unit == QStringLiteral("plasmalogin.service") ||
      unit == QStringLiteral("plasmalogin")) {
    return DisplayManagerKind::PlasmaLogin;
  }
  return DisplayManagerKind::Unknown;
}

QString envOverride()
{
  // The first name is intentionally public for diagnostics; the second is a
  // stable test hook retained for deterministic unit tests.
  QString value = qEnvironmentVariable("ANISPAPER_DISPLAY_MANAGER_UNIT").trimmed();
  if (value.isEmpty()) {
    value = qEnvironmentVariable("ANISPAPER_TEST_DISPLAY_MANAGER_UNIT").trimmed();
  }
  return value;
}

} // namespace

DisplayManagerInfo detectDisplayManager(const DisplayManagerUnitReader &injectedReader)
{
  const QString overrideUnit = envOverride();
  if (!overrideUnit.isEmpty()) {
    return {kindForUnit(overrideUnit), QFileInfo(overrideUnit).fileName(),
            QStringLiteral("environment")};
  }

  const DisplayManagerUnitReader reader = injectedReader ? injectedReader :
      [](const QString &path) {
        const QFileInfo info(path);
        return info.isSymLink() ? info.symLinkTarget() : QString();
      };

  // display-manager.service is the systemd contract for the active display
  // manager.  Prefer the runtime alias, then the persistent alias.  Reading a
  // symlink is side-effect free and works without a system bus or privileges.
  const QStringList aliases{
      QStringLiteral("/run/systemd/system/display-manager.service"),
      QStringLiteral("/etc/systemd/system/display-manager.service"),
  };
  for (const QString &alias : aliases) {
    const QString target = reader(alias).trimmed();
    const DisplayManagerKind kind = kindForUnit(target);
    if (kind != DisplayManagerKind::Unknown) {
      return {kind, QFileInfo(target).fileName(), alias};
    }
  }
  return {};
}

QString displayManagerId(const DisplayManagerKind kind)
{
  switch (kind) {
  case DisplayManagerKind::Sddm:
    return QStringLiteral("sddm");
  case DisplayManagerKind::PlasmaLogin:
    return QStringLiteral("plasmalogin");
  case DisplayManagerKind::Unknown:
    return QStringLiteral("unknown");
  }
  return QStringLiteral("unknown");
}

QString displayManagerLabel(const DisplayManagerKind kind)
{
  switch (kind) {
  case DisplayManagerKind::Sddm:
    return QStringLiteral("SDDM");
  case DisplayManagerKind::PlasmaLogin:
    return QStringLiteral("Plasma Login");
  case DisplayManagerKind::Unknown:
    return QStringLiteral("unknown display manager");
  }
  return QStringLiteral("unknown display manager");
}
