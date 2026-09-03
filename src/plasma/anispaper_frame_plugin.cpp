#include "frame_image_provider.h"

#include "../bridge/frame_bridge.h"
#include "../bridge/frame_protocol.h"

#include <QElapsedTimer>
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
//
// Three properties of the poll matter for cost at 60 fps:
//   * the bridge descriptor is opened once and kept, so a tick is a single
//     pread instead of shm_open + pread + close (3 syscalls -> 1);
//   * the active interval oversamples the producer (8 ms vs a 16.6 ms
//     publication grid).  A poll period equal to the producer period aliases:
//     some ticks observe no new frame and the next observes two, which shows
//     up as judder even though every frame was published on time;
//   * a producer that stops publishing (paused wallpaper, Gaming Mode, daemon
//     down) drops the watcher to a 100 ms interval, and the periodic reopen
//     that comes with it is what detects a recreated bridge -- a cached
//     descriptor would otherwise pin the unlinked object forever.
class FrameWatcher : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString output READ output WRITE setOutput NOTIFY outputChanged)
  Q_PROPERTY(quint64 frameNo READ frameNo NOTIFY frameNoChanged)
  // Native bridge geometry straight from the ANIS header.  QML needs this to
  // derive its cover/fit rectangles: deriving them from the Image's implicit
  // size instead is a binding loop, because sourceClipRect *defines* the
  // implicit size.
  Q_PROPERTY(int frameWidth READ frameWidth NOTIFY frameSizeChanged)
  Q_PROPERTY(int frameHeight READ frameHeight NOTIFY frameSizeChanged)

 public:
  explicit FrameWatcher(QObject *parent = nullptr) : QObject(parent) {
    timer_.setTimerType(Qt::PreciseTimer);
    timer_.setInterval(kActiveIntervalMs);
    connect(&timer_, &QTimer::timeout, this, &FrameWatcher::poll);
    staleClock_.start();
    timer_.start();
  }

  ~FrameWatcher() override { closeBridge(); }

  QString output() const { return output_; }
  void setOutput(const QString &output) {
    if (output_ == output) {
      return;
    }
    output_ = output;
    closeBridge();
    emit outputChanged();
    updateFrameSize(0, 0);
    updateFrameNo(0);  // force one request so the provider shows its fallback
    staleClock_.restart();
    setActive(true);
    poll();
  }

  quint64 frameNo() const { return frameNo_; }
  int frameWidth() const { return frameWidth_; }
  int frameHeight() const { return frameHeight_; }

 signals:
  void outputChanged();
  void frameNoChanged(quint64 frameNo);
  void frameSizeChanged();

 private slots:
  void poll() {
    if (output_.isEmpty()) {
      return;
    }
    if (fd_ < 0 && !openBridge()) {
      // Bridge missing (daemon down/restarting): back off instead of retrying
      // an absent shm object 125 times a second.
      setActive(false);
      return;
    }
    FrameHeader header {};
    const ssize_t got = ::pread(fd_, &header, sizeof(header), 0);
    if (got != static_cast<ssize_t>(sizeof(header)) ||
        std::memcmp(header.magic, "ANIS", 4) != 0 || header.width == 0 ||
        header.height == 0) {
      closeBridge();
      return;
    }
    const quint64 published = loadFrameNo(&header);
    // Zero is the producer's in-progress marker, not a geometry change.
    if (published == 0) {
      return;
    }
    updateFrameSize(static_cast<int>(header.width),
                    static_cast<int>(header.height));
    if (published != frameNo_) {
      updateFrameNo(published);
      staleClock_.restart();
      setActive(true);
      return;
    }
    // Staleness is measured in elapsed time, not tick count: the publication
    // period is user-controlled (fpsCap goes down to 1 fps = one frame per
    // second), and a tick-count threshold would drop a legitimately slow
    // wallpaper to the idle interval mid-playback.
    if (staleClock_.elapsed() >= kStaleMsBeforeIdle) {
      staleClock_.restart();
      setActive(false);
      // Re-resolve the name on the next tick.  A restarted daemon unlinks the
      // old object and creates a new one under the same name; without this the
      // cached descriptor would keep reporting the frozen producer.
      closeBridge();
    }
  }

 private:
  static constexpr int kActiveIntervalMs = 8;
  static constexpr int kIdleIntervalMs = 100;
  // Above the slowest legitimate publication period (fpsCap == 1).
  static constexpr qint64 kStaleMsBeforeIdle = 1500;

  bool openBridge() {
    const QByteArray name = bridgeShmName(output_).toUtf8();
    fd_ = ::shm_open(name.constData(), O_RDONLY | O_CLOEXEC, 0);
    return fd_ >= 0;
  }

  void closeBridge() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = -1;
  }

  void setActive(bool active) {
    const int interval = active ? kActiveIntervalMs : kIdleIntervalMs;
    if (timer_.interval() == interval) {
      return;
    }
    timer_.setInterval(interval);
  }

  void updateFrameNo(quint64 frameNo) {
    if (frameNo_ == frameNo) {
      return;
    }
    frameNo_ = frameNo;
    emit frameNoChanged(frameNo_);
  }

  void updateFrameSize(int width, int height) {
    if (frameWidth_ == width && frameHeight_ == height) {
      return;
    }
    frameWidth_ = width;
    frameHeight_ = height;
    emit frameSizeChanged();
  }

  QTimer timer_;
  QString output_;
  quint64 frameNo_ = 0;
  int fd_ = -1;
  int frameWidth_ = 0;
  int frameHeight_ = 0;
  QElapsedTimer staleClock_;
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
