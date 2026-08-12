#pragma once

#include <functional>

#include <QString>
#include <QStringList>

// The display manager is deliberately detected independently of the daemon's
// renderer and UI.  A reader can be injected by tests (or a future platform
// adapter) without consulting or mutating systemd.
enum class DisplayManagerKind {
  Unknown,
  Sddm,
  PlasmaLogin,
};

struct DisplayManagerInfo {
  DisplayManagerKind kind = DisplayManagerKind::Unknown;
  QString unit;
  QString source;

  bool detected() const { return kind != DisplayManagerKind::Unknown; }
};

using DisplayManagerUnitReader = std::function<QString(const QString &path)>;

DisplayManagerInfo detectDisplayManager(const DisplayManagerUnitReader &reader = {});
QString displayManagerId(DisplayManagerKind kind);
QString displayManagerLabel(DisplayManagerKind kind);
