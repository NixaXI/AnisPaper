#pragma once

#include "../bridge/frame_bridge.h"
#include "renderer.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

class QTimer;

struct RendererOptions {
  int fps = 30;
  double defaultVolume = 1.0;
  QString scaleMode = QStringLiteral("cover");
};

// Process-lifetime singleton.  It lives under the daemon QObject, but keeping a
// single manager ensures all output replacement, crash accounting and restart
// decisions happen serially on the daemon's Qt main thread.
class RendererManager final : public QObject {
  Q_OBJECT

 public:
  static RendererManager &instance(QObject *parent = nullptr);
  ~RendererManager() override;

  bool apply(const QJsonObject &item, const QString &output,
             const RendererOptions &options, QString *error, int *errorCode);
  QJsonObject stop(const QString &output);
  QImage lastFrame(const QString &output) const;
  bool safeMode(const QString &output) const;
  QString wallpaperId(const QString &output) const;
  QJsonObject status() const;

 signals:
  void wallpaperActive(const QJsonObject &event);
  void wallpaperStopped(const QJsonObject &event);
  void wallpaperCrash(const QJsonObject &event);
  void wallpaperSafeMode(const QJsonObject &event);

 private:
  struct Entry {
    QString output;
    RendererSpec spec;
    Renderer *renderer = nullptr;
    QTimer *restartTimer = nullptr;
    QTimer *startupTimer = nullptr;
    QTimer *stableTimer = nullptr;
    int crashes = 0;
    int lastBackoffSeconds = 0;
    bool safeMode = false;
    bool sceneNativeUnsupported = false;
    bool rendererReady = false;
    bool handlingFailure = false;
    quint64 serial = 0;
    QString lastError;
  };

  explicit RendererManager(QObject *parent);
  RendererSpec makeSpec(const QJsonObject &item, const QString &output,
                        const RendererOptions &options) const;
  void createRenderer(Entry *entry, bool staticFallback);
  void destroyRenderer(Entry *entry);
  void handleFailure(Entry *entry, const QString &reason);
  void scheduleRestart(Entry *entry, int seconds, bool activateSafeMode);
  void activateSafeMode(Entry *entry);
  void resetCrashCount(Entry *entry);
  void removeEntry(const QString &output, const QString &reason,
                   bool publishEvent);
  QJsonObject eventFor(const Entry *entry) const;
  QJsonObject statusFor(const Entry *entry) const;
  bool isCurrent(const Entry *entry) const;
  static double numericProperty(const QJsonObject &properties, const QString &name,
                                double fallback, double low, double high);
  static bool booleanProperty(const QJsonObject &properties, const QString &name,
                              bool fallback);
  static int stableWindowMs();
  static int startupWindowMs(const RendererSpec &spec, bool sceneNativeUnsupported);
  static int scaledDelayMs(int seconds);

  QHash<QString, Entry *> byOutput_;
  QHash<QString, QSet<QString>> outputsByWallpaperId_;
  FrameBridgeManager bridges_;
  quint64 nextSerial_ = 1;
};
