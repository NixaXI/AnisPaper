#pragma once

#include "../bridge/frame_bridge.h"
#include "renderer.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QStringList>

class QTimer;

struct RendererOptions {
  int fps = 60;
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
  void setGamingMode(const QString &mode);
  // User Gaming Mode blacklist: case-insensitive substrings matched against
  // each non-skipped process' cmdline + exe target (e.g. "sober").  Patterns
  // are normalized (trimmed, lowercased, 2..128 chars, max 64) here so a
  // one-char typo can never match the whole process table.
  void setGamingBlacklist(const QStringList &patterns);
  void setPlaybackOptions(int fps, double volume);
  // Fed by the KWin script over D-Bus (see packaging/kwin): the set of outputs
  // whose wallpaper is currently covered by a fullscreen (or maximized, when
  // configured) window covers.  Per-output on purpose: a game on one screen
  // must not freeze the wallpaper on the other.
  void setCoveredOutputs(const QStringList &outputs);

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
    // Tracks whether this output's renderer is currently paused by gaming mode
    // or occlusion, so refreshGamingState only issues real transitions.
    bool occluded = false;
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
  static bool steamGameRunning(const QStringList &blacklist,
                               QString *reason = nullptr);
  void refreshGamingState();
  QHash<QString, Entry *> byOutput_;
  QHash<QString, QSet<QString>> outputsByWallpaperId_;
  FrameBridgeManager bridges_;
  quint64 nextSerial_ = 1;
  QTimer *gamingTimer_ = nullptr;
  QString gamingMode_ = QStringLiteral("auto");
  QStringList gamingBlacklist_;
  bool gamingActive_ = false;
  // Outputs the KWin script reports as covered; empty when nothing covers a
  // wallpaper.
  QSet<QString> coveredOutputs_;
};
