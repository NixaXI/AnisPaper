#include "frame_bridge.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <limits>

#include <QPainter>

namespace {
constexpr int kMinDimension = 64;
constexpr int kMaxWidth = 3840;
constexpr int kMaxHeight = 2160;

QString errnoText() { return QString::fromLocal8Bit(strerror(errno)); }

QString normalizedScaleMode(const QString &value) {
  const QString mode = value.trimmed().toLower();
  return mode == QStringLiteral("fit") || mode == QStringLiteral("stretch")
             ? mode
             : QStringLiteral("cover");
}

QImage rgbaForBridge(const QImage &input, const QSize &target,
                     const QString &scaleMode) {
  QImage source = input;
  QImage canvas(target, QImage::Format_RGBA8888);
  canvas.fill(QColor(QStringLiteral("#0A0D14")));
  if (source.isNull()) return canvas;
  source = source.convertToFormat(QImage::Format_RGBA8888);
  if (source.size() == target) return source;

  const QString mode = normalizedScaleMode(scaleMode);
  if (mode == QStringLiteral("stretch")) {
    // Stretch is never an implicit bridge fallback; it is the user's explicit
    // scaleMode selection.
    return source.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
  }

  // The bridge itself needs a concrete physical output-sized backing store.
  // Cover keeps the Wallpaper Engine default; fit keeps the #0A0D14
  // letterbox.  Both preserve source aspect ratio.
  const Qt::AspectRatioMode aspectMode =
      mode == QStringLiteral("fit") ? Qt::KeepAspectRatio
                                     : Qt::KeepAspectRatioByExpanding;
  const QSize scaledSize = source.size().scaled(target, aspectMode);
  const QImage scaled = source.scaled(scaledSize, Qt::KeepAspectRatio,
                                      Qt::SmoothTransformation);
  QPainter painter(&canvas);
  painter.drawImage((target.width() - scaled.width()) / 2,
                    (target.height() - scaled.height()) / 2, scaled);
  painter.end();
  return canvas;
}
}  // namespace

QString sanitizeBridgeOutput(const QString &output) {
  QString result;
  result.reserve(output.size());
  for (const QChar character : output.trimmed()) {
    if (character.isLetterOrNumber() || character == QLatin1Char('-') ||
        character == QLatin1Char('_') || character == QLatin1Char('.')) {
      result.append(character);
    } else {
      result.append(QLatin1Char('_'));
    }
  }
  return result.isEmpty() ? QStringLiteral("unknown") : result;
}

QString bridgeShmName(const QString &output) {
  return QStringLiteral("/anispaper-") + sanitizeBridgeOutput(output);
}

FrameBridge::~FrameBridge() { close(); }

bool FrameBridge::open(const QString &output, const QImage &fallback,
                       QString *error) {
  return open(output, fallback, fallback.size(), error);
}

bool FrameBridge::open(const QString &output, const QImage &fallback,
                       const QSize &frameSize, QString *error) {
  return open(output, fallback, frameSize, QStringLiteral("cover"), error);
}

bool FrameBridge::open(const QString &output, const QImage &fallback,
                       const QSize &frameSize, const QString &scaleMode,
                       QString *error) {
  close();
  scaleMode_ = normalizedScaleMode(scaleMode);
  name_ = bridgeShmName(output);
  const QByteArray nativeName = name_.toUtf8();
  // Replacing a stale object is deliberate: after a daemon restart readers
  // reopen the same name and see a clean frame sequence rather than stale RAM.
  ::shm_unlink(nativeName.constData());
  fd_ = ::shm_open(nativeName.constData(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC,
                   S_IRUSR | S_IWUSR);
  if (fd_ < 0) {
    if (error) *error = QStringLiteral("shm_open failed: ") + errnoText();
    name_.clear();
    return false;
  }
  ownsName_ = true;
  const QSize requested(qBound(kMinDimension, frameSize.width(), kMaxWidth),
                        qBound(kMinDimension, frameSize.height(), kMaxHeight));
  if (!mapForSize(requested, error)) {
    close();
    return false;
  }
  return publish(fallback, error);
}

bool FrameBridge::mapForSize(const QSize &size, QString *error) {
  if (fd_ < 0 || size.width() <= 0 || size.height() <= 0) {
    if (error) *error = QStringLiteral("bridge is not open");
    return false;
  }
  const quint64 payload = static_cast<quint64>(size.width()) *
                          static_cast<quint64>(size.height()) * 4ULL;
  const quint64 total = sizeof(FrameHeader) + payload;
  if (payload == 0 || total > static_cast<quint64>(std::numeric_limits<qsizetype>::max())) {
    if (error) *error = QStringLiteral("invalid bridge frame size");
    return false;
  }
  if (::ftruncate(fd_, static_cast<off_t>(total)) != 0) {
    if (error) *error = QStringLiteral("ftruncate failed: ") + errnoText();
    return false;
  }
  void *mapped = ::mmap(nullptr, static_cast<size_t>(total), PROT_READ | PROT_WRITE,
                        MAP_SHARED, fd_, 0);
  if (mapped == MAP_FAILED) {
    if (error) *error = QStringLiteral("mmap failed: ") + errnoText();
    return false;
  }
  mapping_ = mapped;
  mappingSize_ = static_cast<qsizetype>(total);
  header_ = static_cast<FrameHeader *>(mapping_);
  size_ = size;
  stride_ = static_cast<quint32>(size.width() * 4);
  nextFrameNo_ = 0;
  std::memset(mapping_, 0, static_cast<size_t>(mappingSize_));
  header_->width = static_cast<quint32>(size.width());
  header_->height = static_cast<quint32>(size.height());
  header_->stride = stride_;
  std::memcpy(header_->magic, "ANIS", 4);
  return true;
}

bool FrameBridge::publish(const QImage &image, QString *error) {
  if (!header_ || !mapping_) {
    if (error) *error = QStringLiteral("bridge is not open");
    return false;
  }
  const QImage rgba = rgbaForBridge(image, size_, scaleMode_);
  const uchar *source = rgba.constBits();
  uchar *destination = reinterpret_cast<uchar *>(mapping_) + sizeof(FrameHeader);
  const qsizetype rowBytes = static_cast<qsizetype>(stride_);
  for (int row = 0; row < size_.height(); ++row) {
    std::memcpy(destination + row * rowBytes,
                source + row * rgba.bytesPerLine(),
                static_cast<size_t>(rowBytes));
  }
  header_->timestampNs = monotonicNs();
  storeFrameNo(header_, ++nextFrameNo_);
  return true;
}

void FrameBridge::close() {
  if (mapping_) {
    ::munmap(mapping_, static_cast<size_t>(mappingSize_));
  }
  mapping_ = nullptr;
  mappingSize_ = 0;
  header_ = nullptr;
  if (fd_ >= 0) {
    ::close(fd_);
  }
  fd_ = -1;
  if (ownsName_ && !name_.isEmpty()) {
    ::shm_unlink(name_.toUtf8().constData());
  }
  ownsName_ = false;
  name_.clear();
  size_ = {};
  stride_ = 0;
  nextFrameNo_ = 0;
  scaleMode_ = QStringLiteral("cover");
}

bool FrameBridge::isOpen() const { return header_ != nullptr && fd_ >= 0; }

QString FrameBridge::name() const { return name_; }

quint64 FrameBridge::frameNo() const { return loadFrameNo(header_); }

QSize FrameBridge::frameSize() const { return size_; }

quint32 FrameBridge::stride() const { return stride_; }

QJsonObject FrameBridge::status() const {
  return {{QStringLiteral("active"), isOpen()},
          {QStringLiteral("name"), name_},
          {QStringLiteral("frameNo"), static_cast<qint64>(frameNo())},
          {QStringLiteral("width"), size_.width()},
          {QStringLiteral("height"), size_.height()},
          {QStringLiteral("stride"), static_cast<int>(stride_)},
          {QStringLiteral("scaleMode"), scaleMode_}};
}

quint64 FrameBridge::monotonicNs() {
  timespec stamp{};
  if (::clock_gettime(CLOCK_MONOTONIC, &stamp) != 0) {
    return 0;
  }
  return static_cast<quint64>(stamp.tv_sec) * 1000000000ULL +
         static_cast<quint64>(stamp.tv_nsec);
}

bool FrameBridgeManager::ensure(const QString &output, const QImage &fallback,
                                QString *error) {
  return ensure(output, fallback, fallback.size(), error);
}

bool FrameBridgeManager::ensure(const QString &output, const QImage &fallback,
                                const QSize &frameSize, QString *error) {
  return ensure(output, fallback, frameSize, QStringLiteral("cover"), error);
}

bool FrameBridgeManager::ensure(const QString &output, const QImage &fallback,
                                const QSize &frameSize, const QString &scaleMode,
                                QString *error) {
  stop(output);
  auto *bridge = new FrameBridge;
  if (!bridge->open(output, fallback, frameSize, scaleMode, error)) {
    delete bridge;
    return false;
  }
  bridges_.insert(output, bridge);
  return true;
}

bool FrameBridgeManager::publish(const QString &output, const QImage &frame,
                                 QString *error) {
  FrameBridge *bridge = bridges_.value(output, nullptr);
  if (!bridge) {
    if (error) *error = QStringLiteral("bridge is not active");
    return false;
  }
  return bridge->publish(frame, error);
}

FrameBridgeManager::~FrameBridgeManager() {
  const auto values = bridges_.values();
  qDeleteAll(values);
  bridges_.clear();
}

void FrameBridgeManager::stop(const QString &output) { delete bridges_.take(output); }

QJsonObject FrameBridgeManager::statusFor(const QString &output) const {
  const auto it = bridges_.constFind(output);
  if (it == bridges_.cend() || !it.value()) {
    return {{QStringLiteral("active"), false},
            {QStringLiteral("name"), bridgeShmName(output)},
            {QStringLiteral("frameNo"), 0}};
  }
  return it.value()->status();
}
