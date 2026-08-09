#include "isolated_renderer.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>

#include <csignal>

namespace {
constexpr qsizetype kMaxWorkerLine = 4 * 1024 * 1024;

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
  if (spec_.type != QStringLiteral("video") && spec_.type != QStringLiteral("web")) {
    if (error) {
      *error = QStringLiteral("renderer unavailable");
    }
    return false;
  }
  if (spec_.file.isEmpty() || !QFileInfo(spec_.file).isFile()) {
    if (error) {
      *error = QStringLiteral("renderer source is unavailable");
    }
    return false;
  }

  stopRequested_ = false;
  fatalReported_ = false;
  childFailure_.clear();
  input_.clear();
  const QString program = QCoreApplication::applicationFilePath();
  QStringList args{QStringLiteral("--renderer-child"), QStringLiteral("--type"),
                   spec_.type, QStringLiteral("--file"), spec_.file,
                   QStringLiteral("--width"), QString::number(spec_.width),
                   QStringLiteral("--height"), QString::number(spec_.height),
                   QStringLiteral("--fps"), QString::number(spec_.fps),
                   QStringLiteral("--volume"), QString::number(spec_.volume, 'f', 3),
                   QStringLiteral("--speed"), QString::number(spec_.speed, 'f', 3),
                   QStringLiteral("--loop"), spec_.loop ? QStringLiteral("1")
                                                     : QStringLiteral("0")};
  if (!spec_.preview.isEmpty()) {
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
    running_ = false;
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
}

void IsolatedRenderer::pause() { sendCommand(QStringLiteral("pause")); }

void IsolatedRenderer::resume() { sendCommand(QStringLiteral("resume")); }

QImage IsolatedRenderer::lastFrame() const { return frame_; }

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
  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    return;
  }
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
  if (event != QStringLiteral("frame")) {
    return;
  }
  const QByteArray bytes = QByteArray::fromBase64(
      message.value(QStringLiteral("jpeg")).toString().toLatin1());
  if (bytes.isEmpty() || bytes.size() > kMaxWorkerLine) {
    return;
  }
  QImage decoded;
  if (!decoded.loadFromData(bytes, "JPEG") || decoded.isNull()) {
    return;
  }
  frame_ = decoded.convertToFormat(QImage::Format_RGBA8888);
  fallback_ = message.value(QStringLiteral("fallback")).toBool(false);
  ++frameCount_;
  const qint64 elapsed = fpsClock_.elapsed();
  if (elapsed >= 1000) {
    fps_ = static_cast<double>(frameCount_) * 1000.0 /
           static_cast<double>(elapsed);
    frameCount_ = 0;
    fpsClock_.restart();
  }
  emit frameReady(frame_);
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
