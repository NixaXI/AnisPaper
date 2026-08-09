#include "renderer_child.h"

#include "renderer.h"
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

#include <memory>
#include <cmath>

#include <csignal>

#include <unistd.h>

namespace {
class ChildProtocol final : public QObject {
 public:
  explicit ChildProtocol(Renderer *renderer, QObject *parent = nullptr)
      : QObject(parent), renderer_(renderer), stdinNotifier_(STDIN_FILENO,
                                                            QSocketNotifier::Read,
                                                            this) {
    connect(&stdinNotifier_, &QSocketNotifier::activated, this,
            &ChildProtocol::readCommands);
    connect(renderer_, &Renderer::frameReady, this,
            [this](const QImage &image) { publishFrame(image); });
    connect(renderer_, &Renderer::fatal, this, [this](const QString &reason) {
      publish({{QStringLiteral("event"), QStringLiteral("fatal")},
               {QStringLiteral("message"), reason}});
      QTimer::singleShot(0, qApp, [] { QCoreApplication::exit(2); });
    });
  }

  void publishReady() {
    publish({{QStringLiteral("event"), QStringLiteral("ready")},
             {QStringLiteral("renderer"), renderer_->rendererName()}});
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
  ChildProtocol protocol(renderer.get());
  QString error;
  if (!renderer->start(&error)) {
    const QJsonObject message{{QStringLiteral("event"), QStringLiteral("fatal")},
                              {QStringLiteral("message"), error}};
    const QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stdout);
    fflush(stdout);
    return 2;
  }
  protocol.publishReady();
  const int result = app.exec();
  renderer->stop();
  return result;
}
