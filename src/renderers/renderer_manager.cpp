#include "renderer_manager.h"

#include "../common/wayland_monitor.h"
#include "isolated_renderer.h"
#include "scene_renderer.h"
#include "static_image_renderer.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <utility>
#include <unistd.h>

namespace {
constexpr int kCrashThreshold = 3;

QJsonValue propertyValue(const QJsonObject &properties, const QString &name) {
  QJsonValue value = properties.value(name);
  if (value.isObject()) {
    value = value.toObject().value(QStringLiteral("value"));
  }
  return value;
}

}  // namespace

RendererManager &RendererManager::instance(QObject *parent) {
  static RendererManager *manager = nullptr;
  if (!manager) {
    manager = new RendererManager(parent);
  }
  return *manager;
}

RendererManager::RendererManager(QObject *parent) : QObject(parent) {
  gamingTimer_ = new QTimer(this);
  gamingTimer_->setInterval(1000);
  connect(gamingTimer_, &QTimer::timeout, this,
          &RendererManager::refreshGamingState);
  gamingTimer_->start();
}

RendererManager::~RendererManager() {
  const auto outputs = byOutput_.keys();
  for (const QString &output : outputs) {
    removeEntry(output, QStringLiteral("daemon shutdown"), false);
  }
}

bool RendererManager::apply(const QJsonObject &item, const QString &output,
                            const RendererOptions &options, QString *error,
                            int *errorCode) {
  const QString normalizedOutput = output.trimmed();
  const QString type = item.value(QStringLiteral("type")).toString().trimmed().toLower();
  const QString id = item.value(QStringLiteral("id")).toString();
  const QString file = item.value(QStringLiteral("file")).toString();
  if (normalizedOutput.isEmpty() || normalizedOutput.size() > 128 ||
      normalizedOutput.contains('\n') || normalizedOutput.contains('\r') ||
      id.isEmpty()) {
    if (error) *error = QStringLiteral("invalid params");
    if (errorCode) *errorCode = -32602;
    return false;
  }
  if (type != QStringLiteral("video") && type != QStringLiteral("web") &&
      type != QStringLiteral("scene")) {
    if (error) *error = QStringLiteral("renderer unavailable");
    if (errorCode) *errorCode = -32001;
    return false;
  }
  if (type != QStringLiteral("scene") &&
      (file.isEmpty() || !QFileInfo(file).isFile())) {
    if (error) *error = QStringLiteral("renderer unavailable");
    if (errorCode) *errorCode = -32001;
    return false;
  }

  // Build and validate the requested scene before replacing an active output.
  // A malformed Workshop project must be rejected without touching its current
  // renderer, bridge, child process, or watchdog state.
  const RendererSpec requestedSpec = makeSpec(item, normalizedOutput, options);
  if (requestedSpec.type == QStringLiteral("scene")) {
  // ANISPAPER_SCENE_PROJECT_PREFLIGHT_V2
  // scene.json suele vivir dentro de scene.pkg; validar el proyecto, no sólo
  // la ruta lógica declarada por project.json.
  const QString projectDir =
      !requestedSpec.root.isEmpty()
          ? requestedSpec.root
          : QFileInfo(requestedSpec.file).absolutePath();

  const QFileInfo projectInfo(projectDir);
  const QFileInfo looseScene(requestedSpec.file);
  const QFileInfo packageFile(projectDir + QStringLiteral("/scene.pkg"));

  QString invalidReason;
  if (projectDir.isEmpty() || !projectInfo.isDir()) {
    invalidReason =
        QStringLiteral("scene project directory does not exist: %1").arg(projectDir);
  } else {
    const bool looseSceneOk = looseScene.isFile() && looseScene.size() > 0;
    const bool packageOk = packageFile.isFile() && packageFile.size() > 0;
    if (!looseSceneOk && !packageOk) {
      invalidReason =
          QStringLiteral("scene project has neither a usable loose scene file nor scene.pkg: %1")
              .arg(projectDir);
    }
  }

  if (!invalidReason.isEmpty()) {
    if (error)
      *error = QStringLiteral("INVALID_WALLPAPER: %1").arg(invalidReason);
    if (errorCode)
      *errorCode = -32001;
    return false;
  }
}

if (Entry *existing = byOutput_.value(normalizedOutput, nullptr)) {
    if (existing->safeMode && existing->spec.id == id) {
      if (error) *error = QStringLiteral("safe mode active");
      if (errorCode) *errorCode = -32002;
      return false;
    }
    removeEntry(normalizedOutput, QStringLiteral("replaced"), true);
  }

  // Renderers and the SHM backing store always share the physical wl_output
  // mode.  KWin's logical geometry at 135% is deliberately not used here:
  // wl_output::scale is only an integer buffer scale and multiplying a mode by
  // it would turn a 1920x1080 connector into an incorrect 3840x2160 frame.
  const QSize physicalFrameSize(requestedSpec.width, requestedSpec.height);
  QString bridgeError;
  if (!bridges_.ensure(normalizedOutput,
                       StaticImageRenderer::defaultFrame(physicalFrameSize.width(),
                                                         physicalFrameSize.height()),
                       physicalFrameSize, options.scaleMode, &bridgeError)) {
    if (error) *error = QStringLiteral("renderer unavailable");
    if (errorCode) *errorCode = -32001;
    return false;
  }

  auto *entry = new Entry;
  entry->output = normalizedOutput;
  entry->spec = requestedSpec;
  entry->sceneNativeUnsupported =
      entry->spec.type == QStringLiteral("scene") && !SceneRenderer::nativeSupported();
  entry->serial = nextSerial_++;
  entry->restartTimer = new QTimer(this);
  entry->restartTimer->setSingleShot(true);
  entry->startupTimer = new QTimer(this);
  entry->startupTimer->setSingleShot(true);
  entry->stableTimer = new QTimer(this);
  entry->stableTimer->setSingleShot(true);
  byOutput_.insert(normalizedOutput, entry);
  outputsByWallpaperId_[entry->spec.id].insert(normalizedOutput);
  connect(entry->restartTimer, &QTimer::timeout, this, [this, entry] {
    if (!isCurrent(entry)) {
      return;
    }
    if (entry->safeMode) {
      activateSafeMode(entry);
    } else {
      createRenderer(entry, false);
    }
  });
  connect(entry->stableTimer, &QTimer::timeout, this,
          [this, entry] { resetCrashCount(entry); });
  connect(entry->startupTimer, &QTimer::timeout, this, [this, entry] {
    if (isCurrent(entry) && entry->renderer && !entry->rendererReady &&
        !entry->safeMode) {
      handleFailure(entry, QStringLiteral("renderer start timeout"));
    }
  });
  createRenderer(entry, false);
  emit wallpaperActive(eventFor(entry));
  return true;
}

QJsonObject RendererManager::stop(const QString &output) {
  const QString normalizedOutput = output.trimmed();
  const bool existed = byOutput_.contains(normalizedOutput);
  if (existed) {
    removeEntry(normalizedOutput, QStringLiteral("stopped"), true);
  }
  return {{QStringLiteral("output"), normalizedOutput},
          {QStringLiteral("stopped"), existed}};
}

QImage RendererManager::lastFrame(const QString &output) const {
  const Entry *entry = byOutput_.value(output, nullptr);
  if (!entry) return {};
  if (entry->renderer) {
    const QImage frame = entry->renderer->lastFrame();
    if (!frame.isNull()) return frame;
  }
  // A child can die between watchdog attempts.  handleFailure() has already
  // published a static image to the existing bridge.  The same bridge
  // fallback is also used while Gaming Mode pauses a child before its first
  // frame, so previews remain usable instead of returning -32001.
  return bridges_.snapshot(output);
}

bool RendererManager::safeMode(const QString &output) const {
  const Entry *entry = byOutput_.value(output, nullptr);
  return entry && entry->safeMode;
}

QString RendererManager::wallpaperId(const QString &output) const {
  const Entry *entry = byOutput_.value(output, nullptr);
  return entry ? entry->spec.id : QString();
}

QJsonObject RendererManager::status() const {
  QJsonArray outputs;
  int totalCrashes = 0;
  bool anySafeMode = false;
  QStringList names = byOutput_.keys();
  std::sort(names.begin(), names.end());
  for (const QString &name : names) {
    const Entry *entry = byOutput_.value(name);
    outputs.append(statusFor(entry));
    totalCrashes += entry->crashes;
    anySafeMode = anySafeMode || entry->safeMode;
  }
  return {{QStringLiteral("outputs"), outputs},
          {QStringLiteral("gamingMode"), gamingMode_},
          {QStringLiteral("gamingActive"), gamingActive_},
          {QStringLiteral("watchdog"),
           QJsonObject{{QStringLiteral("count"), totalCrashes},
                       {QStringLiteral("safeMode"), anySafeMode},
                       {QStringLiteral("backoffSeconds"), QJsonArray{1, 3, 9}},
                       {QStringLiteral("stableResetSeconds"), 60}}}};
}

void RendererManager::setGamingMode(const QString &mode) {
  const QString normalized = mode.trimmed().toLower();
  gamingMode_ = (normalized == QStringLiteral("on") ||
                 normalized == QStringLiteral("off"))
                    ? normalized
                    : QStringLiteral("auto");
  refreshGamingState();
}

bool RendererManager::steamGameRunning() {
  QDir proc(QStringLiteral("/proc"));
  const QString self = QString::number(static_cast<qint64>(::getpid()));
  const QStringList pids = proc.entryList({QStringLiteral("[0-9]*")},
                                            QDir::Dirs | QDir::NoDotAndDotDot,
                                            QDir::Name);
  for (const QString &pid : pids) {
    if (pid == self) continue;
    QFile cmdFile(QStringLiteral("/proc/") + pid + QStringLiteral("/cmdline"));
    QFile envFile(QStringLiteral("/proc/") + pid + QStringLiteral("/environ"));
    if (!cmdFile.open(QIODevice::ReadOnly)) continue;
    const QByteArray cmd = cmdFile.read(256 * 1024);
    if (cmd.contains("steamwebhelper") || cmd.contains("steam-runtime")) continue;
    bool steamApp = false;
    if (envFile.open(QIODevice::ReadOnly)) {
      const QByteArray env = envFile.read(256 * 1024);
      steamApp = env.contains("STEAM_COMPAT_APP_ID=") ||
                 env.contains("SteamAppId=") || env.contains("SteamGameId=");
    }
    if (steamApp) return true;
    const QString command = QString::fromLocal8Bit(cmd);
    if (command.contains(QStringLiteral("/steamapps/common/")) &&
        !command.contains(QStringLiteral("steam.exe"), Qt::CaseInsensitive)) return true;
    if ((command.contains(QStringLiteral("wine"), Qt::CaseInsensitive) ||
         command.contains(QStringLiteral("pressure-vessel"), Qt::CaseInsensitive)) &&
        command.contains(QStringLiteral("/compatdata/"))) return true;
  }
  return false;
}

void RendererManager::refreshGamingState() {
  const bool active = gamingMode_ == QStringLiteral("on") ||
                      (gamingMode_ == QStringLiteral("auto") && steamGameRunning());
  if (active == gamingActive_) return;
  gamingActive_ = active;
  for (Entry *entry : std::as_const(byOutput_)) {
    if (!entry || !entry->renderer) continue;
    if (gamingActive_) {
      entry->renderer->pause();
    } else if (entry->renderer->isRunning()) {
      entry->renderer->resume();
      if (!entry->rendererReady && !entry->safeMode) {
        entry->startupTimer->start(startupWindowMs(entry->spec,
                                                   entry->sceneNativeUnsupported));
      }
    }
  }
}

RendererSpec RendererManager::makeSpec(const QJsonObject &item,
                                       const QString &output,
                                       const RendererOptions &options) const {
  RendererSpec spec;
  spec.id = item.value(QStringLiteral("id")).toString();
  spec.type = item.value(QStringLiteral("type")).toString().trimmed().toLower();
  spec.file = item.value(QStringLiteral("file")).toString();
  spec.preview = item.value(QStringLiteral("preview")).toString();
  spec.output = output;
  spec.root = item.value(QStringLiteral("root")).toString().trimmed();
  spec.properties = item.value(QStringLiteral("properties")).toObject();
  spec.fps = qBound(1, options.fps, 60);
  spec.volume = numericProperty(spec.properties, QStringLiteral("volume"),
                                options.defaultVolume, 0.0, 1.0);
  // Some Workshop manifests express volume in percent.  Values over one are
  // only accepted as the intentional 0..100 form.
  if (propertyValue(spec.properties, QStringLiteral("volume")).isDouble() &&
      propertyValue(spec.properties, QStringLiteral("volume")).toDouble() > 1.0) {
    spec.volume = qBound(0.0,
                         propertyValue(spec.properties, QStringLiteral("volume"))
                                 .toDouble() /
                             100.0,
                         1.0);
  }
  spec.speed = numericProperty(spec.properties, QStringLiteral("speed"), 1.0,
                               0.1, 4.0);
  spec.loop = booleanProperty(spec.properties, QStringLiteral("loop"), true);
  {
    const QString mode = options.scaleMode.trimmed().toLower();
    spec.scaleMode = mode == QStringLiteral("fit") || mode == QStringLiteral("stretch")
                         ? mode
                         : QStringLiteral("cover");
  }
  const QSize physical = physicalWaylandOutputSize(output);
  if (physical.width() >= 64 && physical.width() <= 3840 &&
      physical.height() >= 64 && physical.height() <= 2160) {
    spec.width = physical.width();
    spec.height = physical.height();
  }
  return spec;
}

void RendererManager::createRenderer(Entry *entry, bool staticFallback) {
  if (!isCurrent(entry)) {
    return;
  }
  destroyRenderer(entry);
  Renderer *renderer = nullptr;
  if (staticFallback) {
    renderer = new StaticImageRenderer(entry->spec, this);
  } else if (entry->sceneNativeUnsupported) {
    renderer = new SceneRenderer(entry->spec, this);
  } else {
    renderer = new IsolatedRenderer(entry->spec, this);
  }
  entry->renderer = renderer;
  entry->rendererReady = false;
  if (auto *isolated = qobject_cast<IsolatedRenderer *>(renderer)) {
    isolated->setSceneTransportCallbacks(
        [this, entry](const SceneTransportView &source, quint64 *lastSceneFrame) {
          if (!isCurrent(entry)) return SceneTransportPublishResult::Ineligible;
          return bridges_.publishSceneTransport(entry->output, source, lastSceneFrame);
        },
        [this, entry] {
          return isCurrent(entry) ? bridges_.snapshot(entry->output) : QImage();
        });
  }
  connect(renderer, &Renderer::fatal, this, [this, entry](const QString &reason) {
    handleFailure(entry, reason);
  });
  connect(renderer, &Renderer::ready, this, [this, entry] {
    if (!isCurrent(entry) || !entry->renderer) {
      return;
    }
    entry->rendererReady = true;
    entry->startupTimer->stop();
    if (!entry->safeMode) {
      entry->stableTimer->start(stableWindowMs());
    }
  });
  connect(renderer, &Renderer::frameReady, this,
          [this, entry](const QImage &frame) {
            if (!isCurrent(entry) || frame.isNull()) return;
            // A bridge can only disappear when its owning entry is stopped;
            // rendering remains isolated even if a transient shm write fails.
            bridges_.publish(entry->output, frame);
          });
  QString error;
  if (!renderer->start(&error)) {
    QTimer::singleShot(0, this, [this, entry, error] {
      handleFailure(entry, error.isEmpty() ? QStringLiteral("renderer start failed")
                                            : error);
    });
    return;
  }
  if (gamingActive_ && !staticFallback) {
    renderer->pause();
  }
  // A renderer may be intentionally paused before it emits ready when a game
  // is already running (notably the native scene child).  Do not turn that
  // deliberate pause into a watchdog crash; arm the startup deadline when
  // Gaming Mode resumes the renderer instead.
  if (!staticFallback && !entry->rendererReady && !gamingActive_) {
    entry->startupTimer->start(startupWindowMs(entry->spec, entry->sceneNativeUnsupported));
  }
}

void RendererManager::destroyRenderer(Entry *entry) {
  if (!entry || !entry->renderer) {
    return;
  }
  QObject::disconnect(entry->renderer, nullptr, this, nullptr);
  entry->startupTimer->stop();
  entry->renderer->stop();
  delete entry->renderer;
  entry->renderer = nullptr;
}

void RendererManager::handleFailure(Entry *entry, const QString &reason) {
  if (!isCurrent(entry) || !entry->renderer || entry->handlingFailure ||
      entry->safeMode) {
    return;
  }
  entry->handlingFailure = true;
  entry->startupTimer->stop();
  entry->stableTimer->stop();
  destroyRenderer(entry);
  // Keep a valid, owned frame in the public bridge while the isolated child
  // backs off.  This is especially important for corrupt Scene projects:
  // their parser failure is contained in the child, but the desktop and
  // preview RPC must not become blank during the 1/3/9-second watchdog path.
  StaticImageRenderer visibleFallback(entry->spec);
  QString fallbackError;
  if (visibleFallback.start(&fallbackError)) {
    bridges_.publish(entry->output, visibleFallback.lastFrame());
  }
  ++entry->crashes;
  entry->lastError = reason;
  const bool enteringSafeMode = entry->crashes >= kCrashThreshold;
  const int backoff = enteringSafeMode ? 9 : (entry->crashes == 1 ? 1 : 3);
  entry->lastBackoffSeconds = backoff;
  emit wallpaperCrash(eventFor(entry));
  scheduleRestart(entry, backoff, enteringSafeMode);
  entry->handlingFailure = false;
}

void RendererManager::scheduleRestart(Entry *entry, int seconds,
                                      bool activateSafeMode) {
  if (!isCurrent(entry)) {
    return;
  }
  if (activateSafeMode) {
    // Safe mode is a replacement, not a delayed state flag.  A dead isolated
    // child must never remain observable as the active renderer after its
    // third crash.
    this->activateSafeMode(entry);
    return;
  }
  entry->restartTimer->start(scaledDelayMs(seconds));
}

void RendererManager::activateSafeMode(Entry *entry) {
  if (!isCurrent(entry)) {
    return;
  }
  entry->safeMode = true;
  entry->lastBackoffSeconds = 0;
  createRenderer(entry, true);
  emit wallpaperSafeMode(eventFor(entry));
  emit wallpaperActive(eventFor(entry));
}

void RendererManager::resetCrashCount(Entry *entry) {
  if (!isCurrent(entry) || entry->safeMode || !entry->renderer ||
      !entry->renderer->isRunning()) {
    return;
  }
  entry->crashes = 0;
  entry->lastBackoffSeconds = 0;
  // A stable child has recovered.  Leaving an old "Process crashed" error in
  // status after the count was reset made a healthy renderer look broken.
  entry->lastError.clear();
}

void RendererManager::removeEntry(const QString &output, const QString &reason,
                                  bool publishEvent) {
  Entry *entry = byOutput_.take(output);
  if (!entry) {
    return;
  }
  if (entry->restartTimer) {
    entry->restartTimer->stop();
    entry->restartTimer->deleteLater();
  }
  if (entry->stableTimer) {
    entry->stableTimer->stop();
    entry->stableTimer->deleteLater();
  }
  if (entry->startupTimer) {
    entry->startupTimer->stop();
    entry->startupTimer->deleteLater();
  }
  destroyRenderer(entry);
  bridges_.stop(output);
  auto it = outputsByWallpaperId_.find(entry->spec.id);
  if (it != outputsByWallpaperId_.end()) {
    it->remove(output);
    if (it->isEmpty()) {
      outputsByWallpaperId_.erase(it);
    }
  }
  if (publishEvent) {
    QJsonObject event{{QStringLiteral("output"), output},
                      {QStringLiteral("id"), entry->spec.id},
                      {QStringLiteral("reason"), reason},
                      {QStringLiteral("safeMode"), entry->safeMode}};
    emit wallpaperStopped(event);
  }
  delete entry;
}

QJsonObject RendererManager::eventFor(const Entry *entry) const {
  const QString state = entry->safeMode
                            ? QStringLiteral("safe-mode")
                            : (!entry->renderer
                                   ? QStringLiteral("restarting")
                                   : (entry->renderer->isRunning()
                                          ? QStringLiteral("running")
                                          : QStringLiteral("starting")));
  QJsonObject event{{QStringLiteral("id"), entry->spec.id},
                    {QStringLiteral("output"), entry->output},
                    {QStringLiteral("renderer"),
                     entry->renderer ? entry->renderer->rendererName() : entry->spec.type},
                    {QStringLiteral("crashes"), entry->crashes},
                    {QStringLiteral("safeMode"), entry->safeMode},
                    {QStringLiteral("state"), state}};
  if (!entry->lastError.isEmpty()) {
    event.insert(QStringLiteral("error"), entry->lastError);
  }
  if (entry->spec.type == QStringLiteral("scene")) {
    event.insert(QStringLiteral("sceneNativeSupported"),
                 !entry->sceneNativeUnsupported);
    if (entry->sceneNativeUnsupported) {
      event.insert(QStringLiteral("badge"), SceneRenderer::unsupportedBadge());
    }
  }
  return event;
}

QJsonObject RendererManager::statusFor(const Entry *entry) const {
  QJsonObject result = eventFor(entry);
  result.insert(QStringLiteral("wallpaperId"), entry->spec.id);
  result.insert(QStringLiteral("fps"), entry->renderer ? entry->renderer->frameRate() : 0.0);
  result.insert(QStringLiteral("pid"), entry->renderer ? entry->renderer->processId() : 0);
  const QImage currentFrame = lastFrame(entry->output);
  result.insert(QStringLiteral("hasFrame"), !currentFrame.isNull());
  result.insert(QStringLiteral("fallback"),
                (entry->renderer && entry->renderer->isFallback()) ||
                    (!entry->renderer && !currentFrame.isNull()));
  result.insert(QStringLiteral("lastBackoffSeconds"), entry->lastBackoffSeconds);
  result.insert(QStringLiteral("gamingPaused"), gamingActive_);
  result.insert(QStringLiteral("bridge"), bridges_.statusFor(entry->output));
  return result;
}

bool RendererManager::isCurrent(const Entry *entry) const {
  return entry && byOutput_.value(entry->output, nullptr) == entry;
}

double RendererManager::numericProperty(const QJsonObject &properties,
                                        const QString &name, double fallback,
                                        double low, double high) {
  const QJsonValue value = propertyValue(properties, name);
  bool ok = false;
  double parsed = fallback;
  if (value.isDouble()) {
    parsed = value.toDouble();
    ok = std::isfinite(parsed);
  } else if (value.isString()) {
    parsed = value.toString().toDouble(&ok);
  }
  return ok ? qBound(low, parsed, high) : fallback;
}

bool RendererManager::booleanProperty(const QJsonObject &properties,
                                      const QString &name, bool fallback) {
  const QJsonValue value = propertyValue(properties, name);
  if (value.isBool()) {
    return value.toBool();
  }
  if (value.isDouble()) {
    return value.toDouble() != 0.0;
  }
  if (value.isString()) {
    const QString normalized = value.toString().trimmed().toLower();
    if (normalized == QStringLiteral("true") || normalized == QStringLiteral("1") ||
        normalized == QStringLiteral("yes") || normalized == QStringLiteral("on")) {
      return true;
    }
    if (normalized == QStringLiteral("false") || normalized == QStringLiteral("0") ||
        normalized == QStringLiteral("no") || normalized == QStringLiteral("off")) {
      return false;
    }
  }
  return fallback;
}

int RendererManager::stableWindowMs() {
  bool ok = false;
  const int testValue = qEnvironmentVariable("ANISPAPER_TEST_STABLE_MS").toInt(&ok);
  return ok && testValue >= 10 && testValue <= 60000 ? testValue : 60000;
}

int RendererManager::startupWindowMs(const RendererSpec &spec, bool sceneNativeUnsupported) {
  bool ok = false;
  const int testValue = qEnvironmentVariable("ANISPAPER_TEST_STARTUP_MS").toInt(&ok);
  if (ok && testValue >= 100 && testValue <= 10000) {
    return testValue;
  }

  // Native Wallpaper Engine scenes parse and initialize their project before
  // publishing the first frame. Real projects can legitimately exceed the
  // video/web renderer window, but the watchdog must still remain bounded.
  const bool nativeScene = spec.type == QStringLiteral("scene") && !sceneNativeUnsupported;
  return nativeScene ? 15000 : 6000;
}

int RendererManager::scaledDelayMs(int seconds) {
  bool ok = false;
  const double scale = qEnvironmentVariable("ANISPAPER_TEST_WATCHDOG_SCALE").toDouble(&ok);
  const double effective = ok && std::isfinite(scale) && scale > 0.0 && scale <= 1.0
                               ? scale
                               : 1.0;
  return qMax(1, qRound(static_cast<double>(seconds * 1000) * effective));
}
