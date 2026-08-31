#include "renderer_child.h"

#include "renderer.h"
#include "shm_frame_transport.h"
#include "video_renderer.h"
#include "web_renderer.h"

#include <QApplication>
#include <QBuffer>
#include <QCommandLineParser>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSocketNotifier>
#include <QTextStream>
#include <QTimer>
#include <QDateTime>

#include <atomic>
#include <memory>
#include <cmath>


#include <csignal>

#include <unistd.h>

namespace {
bool benchRenderFramesEnabled() {
  static const bool enabled =
      qEnvironmentVariable("ANISPAPER_BENCH_RENDER_FRAMES") == QStringLiteral("1");
  return enabled;
}

std::atomic<quint64> g_benchRenderFrames{0};
std::atomic<qint64> g_benchRenderEpochMs{0};

void benchRecordCompletedFrame() {
  if (!benchRenderFramesEnabled()) {
    return;
  }
  qint64 expected = 0;
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  if (g_benchRenderEpochMs.compare_exchange_strong(expected, now)) {
    // first frame establishes the epoch
  }
  g_benchRenderFrames.fetch_add(1, std::memory_order_relaxed);
}

void benchReportRenderFrames(const char *phase) {
  if (!benchRenderFramesEnabled()) {
    return;
  }
  const quint64 frames = g_benchRenderFrames.load(std::memory_order_relaxed);
  const qint64 epoch = g_benchRenderEpochMs.load(std::memory_order_relaxed);
  const qint64 elapsedMs =
      epoch > 0 ? QDateTime::currentMSecsSinceEpoch() - epoch : 0;
  const double elapsedS = elapsedMs > 0 ? static_cast<double>(elapsedMs) / 1000.0 : 0.0;
  const double fps = elapsedMs > 0 ? static_cast<double>(frames) * 1000.0 /
                                           static_cast<double>(elapsedMs) :
                                     0.0;
  fprintf(stderr,
          "bench_render_frames phase=%s frames=%llu elapsed_s=%.3f fps=%.2f\n",
          phase, static_cast<unsigned long long>(frames), elapsedS, fps);
  fflush(stderr);
}

class ChildProtocol final : public QObject {
 public:
  explicit ChildProtocol(Renderer *renderer, bool useShmTransport,
                         QObject *parent = nullptr)
      : QObject(parent), renderer_(renderer), useTransport_(useShmTransport),
        stdinNotifier_(STDIN_FILENO, QSocketNotifier::Read, this) {
    connect(&stdinNotifier_, &QSocketNotifier::activated, this,
            &ChildProtocol::readCommands);
    connect(renderer_, &Renderer::frameReady, this,
            [this](const QImage &image) { publishFrame(image); });
    connect(renderer_, &Renderer::fatal, this, [this](const QString &reason) {
      publish({{QStringLiteral("event"), QStringLiteral("fatal")},
               {QStringLiteral("message"), reason}});
      QTimer::singleShot(0, qApp, [] { QCoreApplication::exit(2); });
    });
    if (benchRenderFramesEnabled()) {
      auto *reportTimer = new QTimer(this);
      reportTimer->setInterval(30000);
      connect(reportTimer, &QTimer::timeout, this,
              [] { benchReportRenderFrames("periodic"); });
      reportTimer->start();
    }
  }

  void publishReady() {
    publish({{QStringLiteral("event"), QStringLiteral("ready")},
             {QStringLiteral("renderer"), renderer_->rendererName()}});
  }

  // Creates the binary frame transport and announces it.  Must run after
  // renderer->start() succeeded and before the first frame so the parent maps
  // the object in message order.  When creation fails the legacy JPEG path
  // stays active, which keeps direct child invocations (tests, debugging)
  // fully functional.
  void enableTransport(int width, int height) {
    if (!useTransport_ || !transport_.create(width, height)) {
      if (useTransport_) {
        ::fprintf(stderr, "anispaper child: shm transport unavailable, "
                          "falling back to JPEG over JSON\n");
        ::fflush(stderr);
      }
      return;
    }
    // Video renderers can land their PBO readback straight in the transport
    // slot (see VideoRenderer::setFrameTransport); web renderers keep the
    // publish(QImage) path.
    if (auto *video = qobject_cast<VideoRenderer *>(renderer_)) {
      video->setFrameTransport(&transport_);
      videoDirectPublish_ = true;
    }
    publish({{QStringLiteral("event"), QStringLiteral("transport")},
             {QStringLiteral("path"), transport_.name()},
             {QStringLiteral("width"), transport_.width()},
             {QStringLiteral("height"), transport_.height()},
             {QStringLiteral("stride"), static_cast<int>(transport_.stride())},
             {QStringLiteral("buffers"), static_cast<int>(ShmFrameTransport::kBuffers)}});
  }

 private:
  void readCommands() {
    char buffer[4096];
    const ssize_t count = ::read(STDIN_FILENO, buffer, sizeof(buffer));
    if (count <= 0) {
      return;
    }
    commands_ += QByteArray(buffer, static_cast<int>(count));
    while (true) {
      const qsizetype newline = commands_.indexOf('\n');
      if (newline < 0) {
        break;
      }
      const QByteArray line = commands_.left(newline);
      commands_.remove(0, newline + 1);
      QJsonParseError error;
      const QJsonDocument document = QJsonDocument::fromJson(line, &error);
      if (error.error != QJsonParseError::NoError || !document.isObject()) {
        continue;
      }
      const QString command = document.object().value(QStringLiteral("command")).toString();
      if (command == QStringLiteral("pause")) {
        renderer_->pause();
      } else if (command == QStringLiteral("resume")) {
        renderer_->resume();
      } else if (command == QStringLiteral("configure")) {
        const auto object = document.object();
        const int fps = object.value(QStringLiteral("fps")).toInt(renderer_->spec().fps);
        const double volume = object.value(QStringLiteral("volume")).toDouble(renderer_->spec().volume);
        renderer_->applyPlayback(fps, volume);
      } else if (command == QStringLiteral("stop")) {
        renderer_->stop();
        QCoreApplication::quit();
      }
    }
  }

  void publishFrame(const QImage &image) {
    if (image.isNull()) {
      return;
    }
    benchRecordCompletedFrame();
    if (qEnvironmentVariable("ANISPAPER_BENCH_SKIP_FRAME_PUBLISH") ==
        QStringLiteral("1")) {
      return;
    }
    // Video direct-publish: the renderer already landed this frame in the
    // transport slot inside consumeOldestReadback(), so the sequence moved
    // past what we last announced.  Only the tiny notification crosses the
    // pipe — the QImage (and the second slot copy) never happens.
    if (videoDirectPublish_) {
      if (transport_.frameNo() > announcedSeq_) {
        announcedSeq_ = transport_.frameNo();
        publish({{QStringLiteral("event"), QStringLiteral("frame")},
                 {QStringLiteral("shm"), true},
                 {QStringLiteral("seq"), static_cast<qint64>(announcedSeq_)},
                 {QStringLiteral("width"), transport_.width()},
                 {QStringLiteral("height"), transport_.height()},
                 {QStringLiteral("fallback"), renderer_->isFallback()}});
      }
      return;
    }
    // Preferred path: a single memcpy into the shared slot plus a tiny
    // notification.  At 4K this replaces a JPEG encode, a base64 inflate and a
    // parent-side decode per frame.
    if (transport_.isActive() && transport_.publish(image)) {
      publish({{QStringLiteral("event"), QStringLiteral("frame")},
               {QStringLiteral("shm"), true},
               {QStringLiteral("seq"), static_cast<qint64>(transport_.frameNo())},
               {QStringLiteral("width"), image.width()},
               {QStringLiteral("height"), image.height()},
               {QStringLiteral("fallback"), renderer_->isFallback()}});
      return;
    }
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "JPEG", 82)) {
      return;
    }
    publish({{QStringLiteral("event"), QStringLiteral("frame")},
             {QStringLiteral("jpeg"), QString::fromLatin1(jpeg.toBase64())},
             {QStringLiteral("width"), image.width()},
             {QStringLiteral("height"), image.height()},
             {QStringLiteral("fallback"), renderer_->isFallback()}});
  }

  void publish(const QJsonObject &message) {
    const QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    // stdout is exclusively the parent protocol.  Qt/WebEngine diagnostics use
    // stderr, so the parent can keep a strict line parser.
    fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stdout);
    fflush(stdout);
  }

  Renderer *renderer_ = nullptr;
  ShmFrameTransport transport_;
  bool useTransport_ = false;
  bool videoDirectPublish_ = false;
  quint64 announcedSeq_ = 0;
  QSocketNotifier stdinNotifier_;
  QByteArray commands_;
};

bool boundedInteger(const QString &value, int low, int high, int *out) {
  bool ok = false;
  const int parsed = value.toInt(&ok);
  if (!ok || parsed < low || parsed > high) {
    return false;
  }
  *out = parsed;
  return true;
}

bool boundedDouble(const QString &value, double low, double high, double *out) {
  bool ok = false;
  const double parsed = value.toDouble(&ok);
  if (!ok || !std::isfinite(parsed) || parsed < low || parsed > high) {
    return false;
  }
  *out = parsed;
  return true;
}
}  // namespace

bool isRendererChildInvocation(int argc, char **argv) {
  for (int index = 1; index < argc; ++index) {
    if (QString::fromLocal8Bit(argv[index]) == QStringLiteral("--renderer-child")) {
      return true;
    }
  }
  return false;
}

int runRendererChild(int argc, char **argv) {
  QString type;
  for (int index = 1; index + 1 < argc; ++index) {
    if (QString::fromLocal8Bit(argv[index]) == QStringLiteral("--type")) {
      type = QString::fromLocal8Bit(argv[index + 1]);
      break;
    }
  }
  if (type == QStringLiteral("web")) {
    QByteArray flags = qgetenv("QTWEBENGINE_CHROMIUM_FLAGS");
    if (!flags.contains("--disable-gpu")) {
      if (!flags.isEmpty()) {
        flags += ' ';
      }
      flags += "--disable-gpu";
      qputenv("QTWEBENGINE_CHROMIUM_FLAGS", flags);
    }
  }
  QApplication app(argc, argv);
  QCommandLineParser parser;
  parser.setApplicationDescription(QStringLiteral("AnisPaper isolated renderer"));
  parser.addHelpOption();
  QCommandLineOption childOption(QStringLiteral("renderer-child"));
  QCommandLineOption typeOption(QStringLiteral("type"), QStringLiteral("renderer type"),
                                QStringLiteral("type"));
  QCommandLineOption fileOption(QStringLiteral("file"), QStringLiteral("source file"),
                                QStringLiteral("file"));
  QCommandLineOption previewOption(QStringLiteral("preview"), QStringLiteral("preview image"),
                                   QStringLiteral("file"));
  QCommandLineOption widthOption(QStringLiteral("width"), QStringLiteral("frame width"),
                                 QStringLiteral("pixels"));
  QCommandLineOption heightOption(QStringLiteral("height"), QStringLiteral("frame height"),
                                  QStringLiteral("pixels"));
  QCommandLineOption fpsOption(QStringLiteral("fps"), QStringLiteral("frame rate"),
                               QStringLiteral("fps"));
  QCommandLineOption volumeOption(QStringLiteral("volume"), QStringLiteral("volume"),
                                  QStringLiteral("value"));
  QCommandLineOption speedOption(QStringLiteral("speed"), QStringLiteral("speed"),
                                 QStringLiteral("value"));
  QCommandLineOption loopOption(QStringLiteral("loop"), QStringLiteral("loop"),
                                QStringLiteral("0|1"));
  parser.addOption(childOption);
  parser.addOption(typeOption);
  parser.addOption(fileOption);
  parser.addOption(previewOption);
  parser.addOption(widthOption);
  parser.addOption(heightOption);
  parser.addOption(fpsOption);
  parser.addOption(volumeOption);
  parser.addOption(speedOption);
  parser.addOption(loopOption);
  parser.process(app);

  RendererSpec spec;
  spec.type = parser.value(typeOption).trimmed().toLower();
  spec.file = parser.value(fileOption);
  spec.preview = parser.value(previewOption);
  if (!parser.isSet(childOption) || (spec.type != QStringLiteral("video") &&
                                    spec.type != QStringLiteral("web")) ||
      spec.file.isEmpty() ||
      !boundedInteger(parser.value(widthOption), 64, 3840, &spec.width) ||
      !boundedInteger(parser.value(heightOption), 64, 2160, &spec.height) ||
      !boundedInteger(parser.value(fpsOption), 1, 60, &spec.fps) ||
      !boundedDouble(parser.value(volumeOption), 0.0, 1.0, &spec.volume) ||
      !boundedDouble(parser.value(speedOption), 0.1, 4.0, &spec.speed)) {
    const QJsonObject message{{QStringLiteral("event"), QStringLiteral("fatal")},
                              {QStringLiteral("message"),
                               QStringLiteral("invalid renderer child arguments")}};
    const QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stdout);
    fflush(stdout);
    return 2;
  }
  spec.loop = parser.value(loopOption) != QStringLiteral("0");

  if (qEnvironmentVariable("ANISPAPER_TEST_CRASH_ON_START") == QStringLiteral("1")) {
    QTimer::singleShot(0, [] { ::kill(::getpid(), SIGKILL); });
  }

  std::unique_ptr<Renderer> renderer;
  if (spec.type == QStringLiteral("video")) {
    renderer = std::make_unique<VideoRenderer>(spec);
  } else {
    renderer = std::make_unique<WebRenderer>(spec);
  }
  // Binary SHM transport replaces JPEG-over-JSON for large frames.  It is
  // disabled through the environment for protocol-level tests and direct
  // debugging invocations that expect the legacy JPEG line format.
  const bool useShmTransport =
      qEnvironmentVariable("ANISPAPER_CHILD_SHM_TRANSPORT") != QStringLiteral("0");
  ChildProtocol protocol(renderer.get(), useShmTransport);
  QString error;
  if (!renderer->start(&error)) {
    const QJsonObject message{{QStringLiteral("event"), QStringLiteral("fatal")},
                              {QStringLiteral("message"), error}};
    const QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stdout);
    fflush(stdout);
    return 2;
  }
  protocol.enableTransport(spec.width, spec.height);
  protocol.publishReady();
  const int result = app.exec();
  benchReportRenderFrames("final");
  renderer->stop();
  return result;
}
