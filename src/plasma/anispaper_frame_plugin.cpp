#include "frame_image_provider.h"

#include "../bridge/frame_bridge.h"
#include "../bridge/frame_protocol.h"

#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <QTimer>
#include <qqml.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>

// FrameWatcher pushes real frame publications to QML: it polls only the
// 32-byte ANIS header each tick (header-only pread, no pixel payload), so
// QML rebuilds its Image source strictly when the daemon published a new
// frame sequence.  This removes the fixed-rate QML timer whose every tick
// forced a full QQuickImage request + texture rebuild (~8 MB alloc/copy and
// one 1080p upload per tick even when nothing had changed).
class FrameWatcher : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString output READ output WRITE setOutput NOTIFY outputChanged)
  Q_PROPERTY(quint64 frameNo READ frameNo NOTIFY frameNoChanged)

 public:
  explicit FrameWatcher(QObject *parent = nullptr) : QObject(parent) {
    timer_.setInterval(1000 / 60);  // ~60 Hz ceiling; cheap header-only polls
    connect(&timer_, &QTimer::timeout, this, &FrameWatcher::poll);
    timer_.start();
  }

  QString output() const { return output_; }
  void setOutput(const QString &output) {
    if (output_ == output) {
      return;
    }
    output_ = output;
    emit outputChanged();
    updateFrameNo(0);  // force one request so the provider shows its fallback
    poll();
  }

  quint64 frameNo() const { return frameNo_; }

  signals:
    void outputChanged();
    void frameNoChanged(quint64 frameNo);

 private slots:
  void poll() {
    if (output_.isEmpty()) {
      return;
    }
    const QByteArray name = bridgeShmName(output_).toUtf8();
    const int fd = ::shm_open(name.constData(), O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0) {
      // Bridge missing (daemon down/restarting): no updates until it appears.
      return;
    }
    FrameHeader header {};
    const ssize_t got = ::pread(fd, &header, sizeof(header), 0);
    ::close(fd);
    if (got != static_cast<ssize_t>(sizeof(header)) ||
        std::memcmp(header.magic, "ANIS", 4) != 0 || header.width == 0 ||
        header.height == 0) {
      return;
    }
    const quint64 published = loadFrameNo(&header);
    if (published != 0) {
      updateFrameNo(published);
    }
  }

 private:
  void updateFrameNo(quint64 frameNo) {
    if (frameNo_ == frameNo) {
      return;
    }
    frameNo_ = frameNo;
    emit frameNoChanged(frameNo_);
  }

  QTimer timer_;
  QString output_;
  quint64 frameNo_ = 0;
};

class AnisPaperFramePlugin final : public QQmlExtensionPlugin {
  Q_OBJECT
  Q_PLUGIN_METADATA(IID QQmlExtensionInterface_iid)
  Q_INTERFACES(QQmlExtensionInterface)

 public:
  void registerTypes(const char *uri) override {
    Q_ASSERT(QLatin1String(uri) == QLatin1String("org.anispaper.frame"));
    qmlRegisterType<FrameWatcher>(uri, 1, 0, "FrameWatcher");
    // Register the URI explicitly so Qt still considers the plugin module
    // valid even though the QQuickImageProvider is installed separately below.
    qmlRegisterModule(uri, 1, 0);
  }

  void initializeEngine(QQmlEngine *engine, const char *uri) override {
    QQmlExtensionPlugin::initializeEngine(engine, uri);
    if (!engine->imageProvider(QStringLiteral("anispaper"))) {
      engine->addImageProvider(QStringLiteral("anispaper"),
                               new FrameImageProvider());
    }
  }
};

#include "anispaper_frame_plugin.moc"
