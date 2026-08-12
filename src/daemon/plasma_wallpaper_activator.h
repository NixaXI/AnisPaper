#pragma once

#include <QDBusConnection>
#include <QJsonArray>
#include <QRect>
#include <QString>
#include <QVariantMap>
#include <QVector>

#include <optional>
#include <memory>

// This is deliberately a typed seam: PlasmaShell::wallpaper() and
// PlasmaShell::setWallpaper() are the only source of wallpaper state or
// mutation.  Mapping is separate because PlasmaShell exposes screen numbers,
// not connector names.
struct PlasmaScreenMapping {
  QString connector;
  quint32 screenNumber = 0;

  bool operator==(const PlasmaScreenMapping &) const = default;
};

struct PlasmaWallpaperState {
  QString plugin;
  QVariantMap parameters;

  bool operator==(const PlasmaWallpaperState &) const = default;
};

struct PlasmaOutputDescriptor {
  QString connector;
  QRect geometry;
};

class PlasmaOutputMapRunner {
 public:
  virtual ~PlasmaOutputMapRunner() = default;
  virtual bool run(QString *stdoutText, QString *error) = 0;
};

class PlasmaWallpaperTransport {
 public:
  virtual ~PlasmaWallpaperTransport() = default;

  // Must originate from a Plasma/KDE API that explicitly associates the
  // connector with PlasmaShell's screen number.  Registry order is invalid.
  virtual bool screenMappings(QVector<PlasmaScreenMapping> *mappings,
                              QString *error) = 0;
  virtual bool wallpaper(quint32 screenNumber, PlasmaWallpaperState *state,
                         QString *error) = 0;
  virtual bool setWallpaper(const QString &plugin, const QVariantMap &parameters,
                            quint32 screenNumber, QString *error) = 0;
};

class PlasmaDbusTransport final : public PlasmaWallpaperTransport {
 public:
  explicit PlasmaDbusTransport(PlasmaOutputMapRunner *mappingRunner = nullptr);
  ~PlasmaDbusTransport() override;

  bool screenMappings(QVector<PlasmaScreenMapping> *mappings,
                      QString *error) override;
  bool wallpaper(quint32 screenNumber, PlasmaWallpaperState *state,
                 QString *error) override;
  bool setWallpaper(const QString &plugin, const QVariantMap &parameters,
                    quint32 screenNumber, QString *error) override;

  static bool parseScreenMappings(const QString &json,
                                  QVector<PlasmaScreenMapping> *mappings,
                                  QString *error);
  static bool validateOutputTopology(const QStringList &outputOrder,
                                     const QVector<PlasmaOutputDescriptor> &screens,
                                     QVector<PlasmaScreenMapping> *mappings,
                                     QString *error);

 private:
  QDBusConnection connection(QString *error);

  QString fallbackConnectionName_;
  std::optional<QDBusConnection> fallbackConnection_;
  std::unique_ptr<PlasmaOutputMapRunner> ownedMappingRunner_;
  PlasmaOutputMapRunner *mappingRunner_ = nullptr;
};

struct PlasmaActivationPlan {
  quint32 screenNumber = 0;
  QString output;
  QString scaleMode;
  PlasmaWallpaperState targetBefore;
  QVector<PlasmaScreenMapping> mappingsBefore;
  QVector<PlasmaWallpaperState> otherScreensBefore;
};

class PlasmaWallpaperActivator final {
 public:
  explicit PlasmaWallpaperActivator(PlasmaWallpaperTransport *transport);

  // A matching actual Wayland output is the sole condition for invoking
  // Plasma.  Preview and fixture labels intentionally remain renderer-only.
  static bool isCanonicalOutputRequest(const QString &requested);
  static QString connectedOutputIdentity(const QString &requested,
                                         const QJsonArray &waylandOutputs);
  static bool mappingsMatchWaylandOutputs(const QVector<PlasmaScreenMapping> &mappings,
                                          const QJsonArray &waylandOutputs,
                                          QString *error);

  bool preflight(const QString &output, const QString &scaleMode,
                 PlasmaActivationPlan *plan, QString *error);
  bool commit(const PlasmaActivationPlan &plan, QString *error);

 private:
  static bool validScaleMode(const QString &scaleMode);
  static bool findUniqueMapping(const QVector<PlasmaScreenMapping> &mappings,
                                const QString &output, quint32 *screenNumber,
                                QString *error);
  bool mappingSnapshot(QVector<PlasmaScreenMapping> *mappings, QString *error);
  bool restoreTarget(const PlasmaActivationPlan &plan, QString *error);
  bool otherScreensUnchanged(const PlasmaActivationPlan &plan, QString *error);
  static PlasmaWallpaperState desiredState(const PlasmaActivationPlan &plan);
  static bool matchesDesiredState(const PlasmaWallpaperState &state,
                                  const PlasmaActivationPlan &plan);
  static bool equalWallpaperStates(const PlasmaWallpaperState &left,
                                   const PlasmaWallpaperState &right);

  PlasmaWallpaperTransport *transport_ = nullptr;
};
