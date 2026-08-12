#include "isolated_renderer.h"

#include "../bridge/frame_protocol.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QThread>

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr qsizetype kMaxWorkerLine = 4 * 1024 * 1024;

// ANISPAPER_PROFILE=1 enables parent-side frame pipeline profiling: parse,
// base64 decode and JPEG decode costs (JPEG workers) or the shm copy cost
// (scene binary transport) plus the observed inter-frame period.
// Reports to stderr every 120 frames.
struct ParentProfiler {
  bool enabled = qEnvironmentVariableIsSet("ANISPAPER_PROFILE");
  QElapsedTimer clock;
  qint64 lastTickMs = -1;
  QVector<double> periodMs, parseMs, b64Ms, decodeMs, shmCopyMs;

  ParentProfiler() { clock.start(); }

  void reportStage(const char *name, const QVector<double> &v) {
    if (v.isEmpty()) return;
    QVector<double> sorted = v;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0.0;
    for (double d : sorted) sum += d;
    const double avg = sum / sorted.size();
    const double p95 = sorted[(sorted.size() - 1) * 95 / 100];
    qWarning("[parent-profile] %-10s avg=%6.2f ms  p95=%6.2f ms  max=%6.2f ms  n=%d",
             name, avg, p95, sorted.back(), static_cast<int>(sorted.size()));
  }
  void framePeriod() {
    if (!enabled) return;
    const qint64 now = clock.elapsed();
    if (lastTickMs >= 0) periodMs.append(double(now - lastTickMs));
    lastTickMs = now;
  }
  void sample(QVector<double> &bucket, QElapsedTimer &timer) {
    if (enabled) bucket.append(double(timer.nsecsElapsed()) / 1.0e6);
  }
  void maybeReport() {
    if (!enabled || (decodeMs.size() + shmCopyMs.size()) < 120) return;
    reportStage("period", periodMs);
    reportStage("json-parse", parseMs);
    reportStage("b64-decode", b64Ms);
    reportStage("jpeg-decode", decodeMs);
    reportStage("shm-copy", shmCopyMs);
    periodMs.clear(); parseMs.clear(); b64Ms.clear(); decodeMs.clear();
    shmCopyMs.clear();
  }
};

ParentProfiler g_parentProfiler;

QString processFailure(const QProcess &process) {
  if (!process.errorString().isEmpty()) {
    return process.errorString();
  }
  return QStringLiteral("renderer process exited unexpectedly");
}
}  // namespace

IsolatedRenderer::IsolatedRenderer(RendererSpec spec, QObject *parent)
    : Renderer(std::move(spec), parent) {
  process_.setProcessChannelMode(QProcess::SeparateChannels);
  connect(&process_, &QProcess::readyReadStandardOutput, this,
          &IsolatedRenderer::readFrames);
  // Forward child stderr (diagnostics, profile reports) to the daemon
  // journal.  Without a reader the pipe buffer (64 KiB) eventually fills
  // and the child blocks on write(), stalling the whole renderer.
  connect(&process_, &QProcess::readyReadStandardError, this, [this]() {
    const QByteArray err = process_.readAllStandardError();
    if (!err.isEmpty()) {
      ::fwrite(err.constData(), 1, static_cast<size_t>(err.size()), stderr);
      ::fflush(stderr);
    }
  });
  connect(&process_, &QProcess::errorOccurred, this,
          [this](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart && !stopRequested_) {
              childFailure_ = processFailure(process_);
            }
          });
  connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
          this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
            readFrames();
            running_ = false;
            if (!stopRequested_) {
              const QByteArray diagnostics = process_.readAllStandardError();
              QString diagnosticTail;
              if (!diagnostics.isEmpty()) {
                diagnosticTail = QString::fromLocal8Bit(diagnostics.right(2048)).trimmed();
              }
              const QString reason = !childFailure_.isEmpty()
                                         ? childFailure_
                                         : QStringLiteral("renderer child exit=%1 status=%2")
                                               .arg(exitCode)
                                               .arg(exitStatus == QProcess::CrashExit
                                                        ? QStringLiteral("crash")
                                                        : QStringLiteral("normal"));
              cleanupProcessGroup();
              reportFatal(diagnosticTail.isEmpty() ? reason
                                                   : reason + QStringLiteral("; ") + diagnosticTail);
            }
          });
}

IsolatedRenderer::~IsolatedRenderer() { stop(); }

bool IsolatedRenderer::start(QString *error) {
  if (running_) {
    return true;
  }
  const bool isScene = spec_.type == QStringLiteral("scene");
  if (spec_.type != QStringLiteral("video") && spec_.type != QStringLiteral("web") && !isScene) {
    if (error) {
      *error = QStringLiteral("renderer unavailable");
    }
    return false;
  }
  QString sceneProjectDir;
  if (isScene) {
    // The scene engine mounts the whole project directory (scene.json plus
    // scene.pkg/materials/shaders); the daemon spec carries the catalog file
    // and the project root.
    sceneProjectDir = !spec_.root.isEmpty() && QFileInfo(spec_.root).isDir()
                          ? spec_.root
                          : QFileInfo(spec_.file).absolutePath();
    const QString engineOverride =
        qEnvironmentVariable("ANISPAPER_SCENE_ENGINE_BIN");
    sceneEnginePath_ = !engineOverride.isEmpty()
                           ? engineOverride
                           : QCoreApplication::applicationDirPath() +
                                 QStringLiteral("/anis-paper-scene-engine");
    if (sceneProjectDir.isEmpty() || !QFileInfo(sceneEnginePath_).isFile()) {
      if (error) {
        *error = QStringLiteral("renderer unavailable");
      }
      return false;
    }
  } else if (spec_.file.isEmpty() || !QFileInfo(spec_.file).isFile()) {
    if (error) {
      *error = QStringLiteral("renderer source is unavailable");
    }
    return false;
  }

  stopRequested_ = false;
  fatalReported_ = false;
  childFailure_.clear();
  input_.clear();
  QString program = QCoreApplication::applicationFilePath();
  QStringList args{QStringLiteral("--renderer-child"), QStringLiteral("--type"),
                   spec_.type, QStringLiteral("--file"), spec_.file,
                   QStringLiteral("--width"), QString::number(spec_.width),
                   QStringLiteral("--height"), QString::number(spec_.height),
                   QStringLiteral("--fps"), QString::number(spec_.fps),
                   QStringLiteral("--volume"), QString::number(spec_.volume, 'f', 3),
                   QStringLiteral("--speed"), QString::number(spec_.speed, 'f', 3),
                   QStringLiteral("--loop"), spec_.loop ? QStringLiteral("1")
                                                     : QStringLiteral("0")};
  if (isScene) {
    // The scene child is a separate engine binary (no Qt): it reads the
    // Wallpaper Engine project directory and streams offscreen-rendered
    // frames through the same JSON-lines protocol.
    program = sceneEnginePath_;
    const QString scaling = spec_.scaleMode == QStringLiteral("fit")
                                ? QStringLiteral("fit")
                                : spec_.scaleMode == QStringLiteral("stretch")
                                      ? QStringLiteral("stretch")
                                      : QStringLiteral("fill");
    args = QStringList{QStringLiteral("--file"), sceneProjectDir,
                       QStringLiteral("--width"), QString::number(spec_.width),
                       QStringLiteral("--height"), QString::number(spec_.height),
                       QStringLiteral("--fps"), QString::number(spec_.fps),
                       QStringLiteral("--scaling"), scaling};
  }
  if (!spec_.preview.isEmpty() && !isScene) {
    args << QStringLiteral("--preview") << spec_.preview;
  }

  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  // QWebEngineView itself is marked WA_DontShowOnScreen.  Do not force Qt's
  // offscreen platform plugin here: on some Mesa stacks it cannot create the
  // OpenGL context that libmpv needs.  A user service often lacks
  // WAYLAND_DISPLAY, so reproduce F1's safe runtime-dir discovery first.
  const QString forcedRuntime =
      env.value(QStringLiteral("ANISPAPER_RENDERER_RUNTIME_DIR"));
  if (!forcedRuntime.isEmpty()) {
    // A temporary daemon fixture can own its JSON-RPC socket in /tmp while its
    // isolated GUI worker still needs the real user runtime directory to
    // resolve a relative WAYLAND_DISPLAY name.
    env.insert(QStringLiteral("XDG_RUNTIME_DIR"), forcedRuntime);
  }
  if (!env.contains(QStringLiteral("WAYLAND_DISPLAY"))) {
    const QString runtime = env.value(QStringLiteral("XDG_RUNTIME_DIR"));
    const QStringList sockets = QDir(runtime).entryList(
        {QStringLiteral("wayland-*")}, QDir::System | QDir::NoDotAndDotDot,
        QDir::Name);
    for (const QString &socket : sockets) {
      if (!socket.endsWith(QStringLiteral(".lock"))) {
        env.insert(QStringLiteral("WAYLAND_DISPLAY"), socket);
        break;
      }
    }
  }
  if (env.contains(QStringLiteral("WAYLAND_DISPLAY")) &&
      !env.contains(QStringLiteral("QT_QPA_PLATFORM"))) {
    env.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("wayland"));
  }
  // The scene engine child renders offscreen through a hidden GLFW X11 window
  // (XWayland under Wayland), so it always needs a DISPLAY.
  if (!env.contains(QStringLiteral("DISPLAY"))) {
    env.insert(QStringLiteral("DISPLAY"), QStringLiteral(":0"));
  }
  process_.setProcessEnvironment(env);
  process_.setProgram(program);
  process_.setArguments(args);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  process_.setChildProcessModifier([] { ::setpgid(0, 0); });
#endif
  process_.start();
  if (!process_.waitForStarted(1500)) {
    if (error) {
      *error = processFailure(process_);
    }
    return false;
  }
  childPid_ = static_cast<qint64>(process_.processId());
  running_ = true;
  fpsClock_.start();
  frameCount_ = 0;
  fps_ = 0.0;
  return true;
}

void IsolatedRenderer::stop() {
  if (process_.state() == QProcess::NotRunning) {
    // The finished/fatal path can reach RendererManager::destroyRenderer only
    // after QProcess has transitioned to NotRunning.  The child-owned scene
    // mapping is still ours in that case, so it must be released before this
    // renderer is deleted just as it is on an orderly stop.
    running_ = false;
    childPid_ = 0;
    closeSceneTransport();
    return;
  }
  stopRequested_ = true;
  const qint64 pid = childPid_;
  sendCommand(QStringLiteral("stop"));
  process_.waitForBytesWritten(200);
  if (!process_.waitForFinished(800)) {
    process_.terminate();
    if (!process_.waitForFinished(400)) {
      process_.kill();
      process_.waitForFinished(400);
    }
  }
  if (pid > 0) {
    // The child creates a process group before QtWebEngine starts Chromium
    // subprocesses.  Killing that group is a best-effort cleanup only.
    ::kill(static_cast<pid_t>(-pid), SIGTERM);
  }
  running_ = false;
  childPid_ = 0;
  closeSceneTransport();
}

void IsolatedRenderer::pause() { sendCommand(QStringLiteral("pause")); }

void IsolatedRenderer::resume() { sendCommand(QStringLiteral("resume")); }

QImage IsolatedRenderer::lastFrame() const {
  // Native Scene frames are directly copied to the bridge.  Keep previews and
  // explicit snapshots independent by taking an owned bridge snapshot only on
  // demand, never as part of the per-frame path.
  if (sceneTransportMap_ && bridgeSnapshot_) {
    return bridgeSnapshot_();
  }
  return frame_;
}

void IsolatedRenderer::setSceneTransportCallbacks(SceneTransportSink sink,
                                                   BridgeSnapshot snapshot) {
  sceneTransportSink_ = std::move(sink);
  bridgeSnapshot_ = std::move(snapshot);
}

QString IsolatedRenderer::rendererName() const { return spec_.type; }

bool IsolatedRenderer::isRunning() const {
  return running_ && process_.state() != QProcess::NotRunning;
}

bool IsolatedRenderer::isFallback() const { return fallback_; }

qint64 IsolatedRenderer::processId() const { return childPid_; }

double IsolatedRenderer::frameRate() const { return fps_; }

void IsolatedRenderer::sendCommand(const QString &command) {
  if (process_.state() == QProcess::NotRunning) {
    return;
  }
  const QJsonObject message{{QStringLiteral("command"), command}};
  process_.write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
}

void IsolatedRenderer::readFrames() {
  input_ += process_.readAllStandardOutput();
  if (input_.size() > kMaxWorkerLine && !input_.contains('\n')) {
    childFailure_ = QStringLiteral("renderer worker protocol line is oversized");
    process_.kill();
    return;
  }
  while (true) {
    const qsizetype newline = input_.indexOf('\n');
    if (newline < 0) {
      break;
    }
    const QByteArray line = input_.left(newline);
    input_.remove(0, newline + 1);
    if (line.size() > kMaxWorkerLine) {
      childFailure_ = QStringLiteral("renderer worker frame is oversized");
      process_.kill();
      return;
    }
    parseLine(line);
  }
}

void IsolatedRenderer::parseLine(const QByteArray &line) {
  const bool profilePicture =
      line.size() > 1024 || sceneTransportMap_ != nullptr;
  QElapsedTimer stageTimer;
  if (profilePicture) stageTimer.start();
  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    return;
  }
  if (profilePicture) g_parentProfiler.sample(g_parentProfiler.parseMs, stageTimer);
  const QJsonObject message = document.object();
  const QString event = message.value(QStringLiteral("event")).toString();
  if (event == QStringLiteral("ready")) {
    emit ready();
    return;
  }
  if (event == QStringLiteral("fatal")) {
    childFailure_ = message.value(QStringLiteral("message"))
                        .toString(QStringLiteral("renderer worker failed"));
    return;
  }
  if (event == QStringLiteral("transport")) {
    // Scene child published its binary shm transport; all subsequent "frame"
    // events are tiny notifications and the pixels come from the mapping.
    openSceneTransport(message.value(QStringLiteral("path")).toString());
    return;
  }
  if (event != QStringLiteral("frame")) {
    return;
  }
  const QString jpegPayload = message.value(QStringLiteral("jpeg")).toString();
  bool directPublished = false;
  if (jpegPayload.isEmpty()) {
    // Native Scene frames go straight from the validated child slot to the
    // output-specific bridge.  The owned QImage route remains the fallback
    // for unavailable callbacks and layout/metadata incompatibility.
    if (profilePicture) stageTimer.restart();
    const SceneTransportPublishResult direct = publishSceneTransportFrame();
    if (direct == SceneTransportPublishResult::Published) {
      // No private frame_ memcpy in the eligible direct path.
      directPublished = true;
    } else if (direct == SceneTransportPublishResult::Raced) {
      return;
    } else if (!copySceneTransportFrame()) {
      return;
    }
    if (profilePicture) {
      g_parentProfiler.sample(g_parentProfiler.shmCopyMs, stageTimer);
      g_parentProfiler.framePeriod();
      g_parentProfiler.maybeReport();
    }
  } else {
    if (profilePicture) stageTimer.restart();
    const QByteArray bytes = QByteArray::fromBase64(jpegPayload.toLatin1());
    if (bytes.isEmpty() || bytes.size() > kMaxWorkerLine) {
      return;
    }
    if (profilePicture) {
      g_parentProfiler.sample(g_parentProfiler.b64Ms, stageTimer);
      stageTimer.restart();
    }
    QImage decoded;
    if (!decoded.loadFromData(bytes, "JPEG") || decoded.isNull()) {
      return;
    }
    frame_ = decoded.convertToFormat(QImage::Format_RGBA8888);
    if (profilePicture) {
      g_parentProfiler.sample(g_parentProfiler.decodeMs, stageTimer);
      g_parentProfiler.framePeriod();
      g_parentProfiler.maybeReport();
    }
  }
  fallback_ = message.value(QStringLiteral("fallback")).toBool(false);
  ++frameCount_;
  const qint64 elapsed = fpsClock_.elapsed();
  if (elapsed >= 1000) {
    fps_ = static_cast<double>(frameCount_) * 1000.0 /
           static_cast<double>(elapsed);
    frameCount_ = 0;
    fpsClock_.restart();
  }
  if (!directPublished) emit frameReady(frame_);
}

bool IsolatedRenderer::openSceneTransport(const QString &name) {
  closeSceneTransport();
  if (spec_.type != QStringLiteral("scene") ||
      !name.startsWith(QLatin1Char('/'))) {
    return false;
  }
  const QByteArray native = name.toLocal8Bit();
  const int fd = ::shm_open(native.constData(), O_RDONLY | O_CLOEXEC, 0400);
  if (fd < 0) {
    return false;
  }
  struct stat statBuffer {};
  if (::fstat(fd, &statBuffer) != 0 ||
      statBuffer.st_size < static_cast<off_t>(sizeof(SceneTransportHeader))) {
    ::close(fd);
    return false;
  }
  const size_t mapSize = static_cast<size_t>(statBuffer.st_size);
  void *mapping = ::mmap(nullptr, mapSize, PROT_READ, MAP_SHARED, fd, 0);
  if (mapping == MAP_FAILED) {
    ::close(fd);
    return false;
  }
  const auto *header = static_cast<const SceneTransportHeader *>(mapping);
  const quint64 expectedSize =
      quint64(sizeof(SceneTransportHeader)) +
      quint64(header->stride) * header->height * header->buffers;
  const bool headerValid =
      ::memcmp(header->magic, kSceneTransportMagic, 4) == 0 &&
      header->version == kSceneTransportVersion &&
      header->format == kSceneTransportFormatRgba8888 && header->width != 0 &&
      header->height != 0 && header->buffers != 0 &&
      header->stride >= header->width * 4 && expectedSize <= quint64(mapSize);
  // frame_ must stay a writable, privately-owned image: every new frame
  // memcpy's into its bits while downstream consumers may still hold the
  // previous implicit-shared copy of it.
  QImage target(static_cast<int>(header->width), static_cast<int>(header->height),
                QImage::Format_RGBA8888);
  const bool layoutMatch =
      !target.isNull() &&
      static_cast<quint32>(target.bytesPerLine()) == header->stride &&
      static_cast<size_t>(target.sizeInBytes()) ==
          static_cast<size_t>(header->stride) * header->height;
  if (!headerValid || !layoutMatch) {
    ::munmap(mapping, mapSize);
    ::close(fd);
    return false;
  }
  frame_ = target;
  sceneTransportFd_ = fd;
  sceneTransportMap_ = mapping;
  sceneTransportMapSize_ = mapSize;
  sceneTransportLastSeq_ = 0;
  sceneTransportView_ = {header,
                         static_cast<const uchar *>(mapping) + sizeof(SceneTransportHeader),
                         static_cast<size_t>(header->stride) * header->height,
                         header->buffers,
                         header->width,
                         header->height,
                         header->stride,
                         header->format};
  return true;
}

void IsolatedRenderer::closeSceneTransport() {
  if (sceneTransportMap_) {
    ::munmap(sceneTransportMap_, sceneTransportMapSize_);
    sceneTransportMap_ = nullptr;
  }
  sceneTransportMapSize_ = 0;
  if (sceneTransportFd_ >= 0) {
    ::close(sceneTransportFd_);
    sceneTransportFd_ = -1;
  }
  sceneTransportLastSeq_ = 0;
  sceneTransportView_ = {};
}

SceneTransportPublishResult IsolatedRenderer::publishSceneTransportFrame() {
  if (!sceneTransportSink_ || !sceneTransportMap_ || !sceneTransportView_.header) {
    return SceneTransportPublishResult::Ineligible;
  }
  // parseLine() is invoked by this QProcess object's main-thread signal.  The
  // callback is direct and completes before closeSceneTransport() can unmap.
  if (QThread::currentThread() != thread()) return SceneTransportPublishResult::Ineligible;
  return sceneTransportSink_(sceneTransportView_, &sceneTransportLastSeq_);
}

bool IsolatedRenderer::copySceneTransportFrame() {
  const auto *header = sceneTransportView_.header;
  if (!header || frame_.isNull()) {
    return false;
  }
  if (std::memcmp(header->magic, kSceneTransportMagic, 4) != 0 ||
      header->version != kSceneTransportVersion || header->format != sceneTransportView_.format ||
      header->width != sceneTransportView_.width || header->height != sceneTransportView_.height ||
      header->stride != sceneTransportView_.stride || header->buffers != sceneTransportView_.buffers) {
    return false;
  }
  const char *slotBase = reinterpret_cast<const char *>(sceneTransportView_.slotData);
  const size_t frameBytes = sceneTransportView_.slotBytes;
  for (int attempt = 0; attempt < 2; ++attempt) {
    const quint64 seq = loadSceneFrameNo(header);
    if (seq == 0 || (attempt == 0 && seq == sceneTransportLastSeq_)) {
      return false;  // nothing published yet / already consumed
    }
    const quint32 writeIndex = static_cast<quint32>(seq % sceneTransportView_.buffers);
    if (loadSceneWriteIndex(header) != writeIndex) {
      return false;
    }
    ::memcpy(frame_.bits(), slotBase + static_cast<size_t>(writeIndex) * frameBytes,
             frameBytes);
    if (loadSceneFrameNo(header) == seq) {
      sceneTransportLastSeq_ = seq;
      return true;
    }
    // The writer raced the copy; retry once with the newer index.  If it
    // races again the next notification (next frame) delivers the newer one.
  }
  return false;
}

void IsolatedRenderer::reportFatal(const QString &reason) {
  if (fatalReported_) {
    return;
  }
  fatalReported_ = true;
  emit fatal(reason);
}

void IsolatedRenderer::cleanupProcessGroup() {
  if (childPid_ > 0) {
    ::kill(static_cast<pid_t>(-childPid_), SIGTERM);
  }
  childPid_ = 0;
}
