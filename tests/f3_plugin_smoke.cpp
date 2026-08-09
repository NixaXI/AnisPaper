#include "../src/bridge/frame_bridge.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QQuickImageProvider>
#include <QQuickItem>
#include <QQuickView>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QPluginLoader>
#include <QPainter>
#include <QThread>
#include <QTemporaryDir>
#include <QUrl>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

namespace {
bool spin(int milliseconds) {
  const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + milliseconds;
  while (QDateTime::currentMSecsSinceEpoch() < deadline) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
    QThread::msleep(5);
  }
  return true;
}

bool require(bool condition, const char *message) {
  if (!condition) std::fprintf(stderr, "f3_plugin_smoke: %s\n", message);
  return condition;
}

QImage solid(const QColor &color) {
  QImage image(96, 64, QImage::Format_RGBA8888);
  image.fill(color);
  return image;
}

QImage wideBands() {
  QImage image(200, 100, QImage::Format_RGBA8888);
  image.fill(Qt::green);
  QPainter painter(&image);
  painter.fillRect(0, 0, 50, image.height(), Qt::red);
  painter.fillRect(150, 0, 50, image.height(), Qt::blue);
  return image;
}

bool corruptBridge() {
  const QByteArray name = bridgeShmName(QStringLiteral("F3-CORRUPT")).toUtf8();
  ::shm_unlink(name.constData());
  const int fd = ::shm_open(name.constData(), O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0) return false;
  const bool resized = ::ftruncate(fd, sizeof(FrameHeader)) == 0;
  if (resized) {
    auto *header = static_cast<FrameHeader *>(
        ::mmap(nullptr, sizeof(FrameHeader), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    if (header != MAP_FAILED) {
      std::memset(header, 0, sizeof(FrameHeader));
      std::memcpy(header->magic, "NOPE", 4);
      ::munmap(header, sizeof(FrameHeader));
    }
  }
  ::close(fd);
  return resized;
}
}  // namespace

int main(int argc, char **argv) {
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  if (qEnvironmentVariableIsEmpty("QT_QUICK_BACKEND")) {
    qputenv("QT_QUICK_BACKEND", "software");
  }
  QGuiApplication app(argc, argv);

  // The workspace filesystem is mounted noexec in CI. QML plugins need an
  // executable mapping, so mirror the built module into a disposable normal
  // filesystem before loading exactly the same qmldir and shared object.
  QTemporaryDir qmlImport;
  if (!require(qmlImport.isValid(), "could not create temporary QML import root")) {
    return 1;
  }
  const QString sourceModule = QStringLiteral(ANISPAPER_FRAME_QML_SOURCE_DIR);
  const QString targetModule = qmlImport.path() + QStringLiteral("/org/anispaper/frame");
  QDir().mkpath(targetModule);
  for (const QString &file : {QStringLiteral("qmldir"),
                              QStringLiteral("FrameBridgeSupport.qml"),
                              QStringLiteral("libanispaperframeprovider.so")}) {
    if (!QFile::copy(sourceModule + QLatin1Char('/') + file,
                     targetModule + QLatin1Char('/') + file)) {
      std::fprintf(stderr, "f3_plugin_smoke: could not copy QML module file %s\n",
                   file.toUtf8().constData());
      return 1;
    }
  }
  QPluginLoader moduleLoader(targetModule + QStringLiteral("/libanispaperframeprovider.so"));
  if (!require(moduleLoader.instance() != nullptr,
               moduleLoader.errorString().toUtf8().constData())) {
    return 1;
  }

  QString error;
  FrameBridge bridge;
  if (!require(bridge.open(QStringLiteral("F3-QML"), solid(Qt::red), &error),
               error.toUtf8().constData())) {
    return 1;
  }

  QQuickView view;
  view.engine()->addImportPath(qmlImport.path());
  QQmlComponent component(view.engine());
  const QString qml = QStringLiteral(R"QML(
import QtQuick
import org.anispaper.frame 1.0
Item {
    width: 96
    height: 64
    property int frameNo: 1
    FrameBridgeSupport { id: support }
    Image {
        objectName: "bridgeImage"
        anchors.fill: parent
        cache: false
        source: "image://anispaper/F3-QML?f=" + parent.frameNo
    }
}
)QML");
  component.setData(qml.toUtf8(), QUrl(QStringLiteral("inmemory:/f3-smoke.qml")));
  for (int attempt = 0; component.isLoading() && attempt < 200; ++attempt) spin(25);
  if (!require(!component.isLoading(), "QML component did not finish loading")) {
    return 1;
  }
  QObject *root = component.create();
  if (!root) {
    std::fprintf(stderr, "f3_plugin_smoke: QML load failed (status=%d): %s\n",
                 static_cast<int>(component.status()),
                 component.errorString().toUtf8().constData());
    return 1;
  }
  view.setContent(QUrl(QStringLiteral("inmemory:/f3-smoke.qml")), &component, root);
  view.resize(96, 64);
  view.show();
  spin(250);

  auto *imageObject = root->findChild<QObject *>(QStringLiteral("bridgeImage"));
  auto *provider = dynamic_cast<QQuickImageProvider *>(
      view.engine()->imageProvider(QStringLiteral("anispaper")));
  QSize nativeSize;
  const QImage first = provider ? provider->requestImage(QStringLiteral("F3-QML?f=1"),
                                                          &nativeSize, {})
                                : QImage();
  bool ok = require(imageObject != nullptr, "QQuickView did not create Image") &&
            require(provider != nullptr, "QML extension did not register image provider") &&
            require(!first.isNull() && nativeSize == QSize(96, 64),
                    "provider did not map the initial bridge frame") &&
            require(first.pixelColor(2, 2).red() > 180,
                    "initial bridge frame is not the published red image");

  // The bridge's physical backing store must never silently distort a source
  // frame.  Check all three explicit scale modes through the same provider
  // the Plasma QML consumes: cover centrally crops, fit letterboxes #0A0D14,
  // and only stretch changes the source aspect ratio.
  FrameBridge coverBridge;
  FrameBridge fitBridge;
  FrameBridge stretchBridge;
  error.clear();
  ok = require(coverBridge.open(QStringLiteral("F3-SCALE-COVER"), wideBands(),
                                QSize(100, 100), QStringLiteral("cover"), &error),
               error.toUtf8().constData()) && ok;
  error.clear();
  ok = require(fitBridge.open(QStringLiteral("F3-SCALE-FIT"), wideBands(),
                              QSize(100, 100), QStringLiteral("fit"), &error),
               error.toUtf8().constData()) && ok;
  error.clear();
  ok = require(stretchBridge.open(QStringLiteral("F3-SCALE-STRETCH"), wideBands(),
                                  QSize(100, 100), QStringLiteral("stretch"), &error),
               error.toUtf8().constData()) && ok;
  const QImage cover = provider ? provider->requestImage(QStringLiteral("F3-SCALE-COVER?f=1"),
                                                           &nativeSize, {})
                                : QImage();
  const QImage fit = provider ? provider->requestImage(QStringLiteral("F3-SCALE-FIT?f=1"),
                                                         &nativeSize, {})
                              : QImage();
  const QImage stretch = provider ? provider->requestImage(QStringLiteral("F3-SCALE-STRETCH?f=1"),
                                                             &nativeSize, {})
                                  : QImage();
  const QColor fitLetterbox = fit.pixelColor(2, 2);
  ok = require(cover.size() == QSize(100, 100) &&
                   cover.pixelColor(2, 50).green() > 180 &&
                   cover.pixelColor(2, 50).red() < 40,
               "cover did not centre-crop while preserving aspect ratio") && ok;
  ok = require(fit.size() == QSize(100, 100) && fitLetterbox.red() == 10 &&
                   fitLetterbox.green() == 13 && fitLetterbox.blue() == 20 &&
                   fit.pixelColor(2, 50).red() > 180,
               "fit did not retain source aspect with the #0A0D14 letterbox") && ok;
  ok = require(stretch.size() == QSize(100, 100) &&
                   stretch.pixelColor(2, 2).red() > 180 &&
                   stretch.pixelColor(2, 2).green() < 40,
               "explicit stretch did not fill the destination") && ok;
  ok = require(coverBridge.status().value(QStringLiteral("scaleMode")).toString() ==
                   QStringLiteral("cover"),
               "bridge status did not preserve the selected scale mode") && ok;

  error.clear();
  ok = require(bridge.publish(solid(Qt::blue), &error), error.toUtf8().constData()) && ok;
  root->setProperty("frameNo", 2);
  spin(250);
  const QImage second = provider ? provider->requestImage(QStringLiteral("F3-QML?f=2"),
                                                           &nativeSize, {})
                                 : QImage();
  const QString source = imageObject ? imageObject->property("source").toUrl().toString()
                                     : QString();
  ok = require(source.contains(QStringLiteral("f=2")),
               "QML Image source did not advance at the timer boundary") && ok;
  ok = require(!second.isNull() && second.pixelColor(2, 2).blue() > 180,
               "provider did not expose the new bridge frame") && ok;

  QSize fallbackSize;
  const QImage missing = provider ? provider->requestImage(QStringLiteral("F3-MISSING?f=1"),
                                                            &fallbackSize, {})
                                  : QImage();
  ok = require(!missing.isNull(), "missing bridge did not yield a fallback image") && ok;
  ok = require(corruptBridge(), "could not create corrupt bridge fixture") && ok;
  const QImage corrupt = provider ? provider->requestImage(QStringLiteral("F3-CORRUPT?f=1"),
                                                            &fallbackSize, {})
                                  : QImage();
  ok = require(!corrupt.isNull(), "corrupt bridge did not yield a fallback image") && ok;
  ::shm_unlink(bridgeShmName(QStringLiteral("F3-CORRUPT")).toUtf8().constData());

  // Simulate daemon recreation: the provider must never emit invalid memory
  // when the sequence resets, and it must recover on the next QML tick.
  bridge.close();
  FrameBridge replacement;
  error.clear();
  ok = require(replacement.open(QStringLiteral("F3-QML"), solid(Qt::green), &error),
               error.toUtf8().constData()) && ok;
  const QImage resetTick = provider ? provider->requestImage(QStringLiteral("F3-QML?f=3"),
                                                              &nativeSize, {})
                                    : QImage();
  error.clear();
  ok = require(replacement.publish(solid(Qt::green), &error),
               error.toUtf8().constData()) && ok;
  const QImage recovered = provider ? provider->requestImage(QStringLiteral("F3-QML?f=4"),
                                                              &nativeSize, {})
                                    : QImage();
  ok = require(!resetTick.isNull() && !recovered.isNull() &&
                   recovered.pixelColor(2, 2).green() > 100,
               "sequence reset returned an invalid or stale image") && ok;

  FrameBridge other;
  error.clear();
  ok = require(other.open(QStringLiteral("F3-OTHER"), solid(Qt::yellow), &error),
               error.toUtf8().constData()) && ok;
  const QImage independent = provider ? provider->requestImage(QStringLiteral("F3-OTHER?f=1"),
                                                                &nativeSize, {})
                                        : QImage();
  ok = require(!independent.isNull() && independent.pixelColor(2, 2).green() > 180,
               "second output bridge conflicts with the first") && ok;

  if (ok) std::puts("f3_plugin_smoke: QQuickView/provider/scale-modes/fallback/reset/multi-output assertions passed");
  return ok ? 0 : 1;
}
