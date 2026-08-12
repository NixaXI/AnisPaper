#include "frame_image_provider.h"

#include "../bridge/frame_bridge.h"
#include "../bridge/frame_protocol.h"

#include <QFileInfo>
#include <QImageReader>
#include <QPainter>
#include <QStandardPaths>
#include <QUrlQuery>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>

namespace {
constexpr quint32 kMaxWidth = 7680;
constexpr quint32 kMaxHeight = 4320;

struct MappedRegion {
  int fd = -1;
  void *data = nullptr;
  size_t size = 0;
  ~MappedRegion() {
    if (data && data != MAP_FAILED) ::munmap(data, size);
    if (fd >= 0) ::close(fd);
  }
};

quint64 requestedFrameNo(const QString &id) {
  const int question = id.indexOf(QLatin1Char('?'));
  if (question < 0) return 0;
  QUrlQuery query(QStringLiteral("?") + id.mid(question + 1));
  bool ok = false;
  const quint64 value = query.queryItemValue(QStringLiteral("f")).toULongLong(&ok);
  return ok ? value : 0;
}

QString outputName(const QString &id) {
  const int question = id.indexOf(QLatin1Char('?'));
  return question < 0 ? id : id.left(question);
}
}  // namespace

FrameImageProvider::FrameImageProvider()
    : QQuickImageProvider(QQuickImageProvider::Image) {}

QImage FrameImageProvider::requestImage(const QString &id, QSize *size,
                                        const QSize &requestedSize) {
  const QString output = outputName(id).trimmed();
  const quint64 expected = requestedFrameNo(id);
  QMutexLocker locker(&mutex_);
  quint64 actual = 0;
  QImage current = readFrame(output, expected, &actual);
  CachedFrame &cached = cache_[output];

  // A lower sequence means a new daemon has recreated the bridge.  Never
  // return bytes from the old mapping as if they belonged to the new writer.
  // One fallback tick lets the next request consume its fully published frame.
  if (actual != 0 && cached.frameNo != 0 && actual < cached.frameNo) {
    cached.frameNo = actual;
    cached.image = fallback(requestedSize);
    cached.resetPending = true;
  } else if (!current.isNull()) {
    if (cached.resetPending && actual == cached.frameNo) {
      // The recreated shm object has published its initial fallback only. Do
      // one stable fallback tick, then accept the first newer sequence value.
    } else if (cached.resetPending && actual > cached.frameNo) {
      cached.image = current;
      cached.frameNo = actual;
      cached.resetPending = false;
    } else if (actual == cached.frameNo && !cached.image.isNull()) {
      // Fast path: no new frame published since the last request.  Serve the
      // cached copy instead of memcpy'ing the same multi-megabyte payload
      // again for every redundant poll tick (QML polls at up to 60 Hz).
    } else if (expected != 0 && expected <= cached.frameNo && actual < expected &&
               !cached.image.isNull()) {
      // A caller that knows a prior producer sequence asked for a newer one;
      // retain the last verified pixels instead of flashing a partial frame.
    } else {
      cached.image = current;
      cached.frameNo = actual;
    }
  } else if (cached.image.isNull()) {
    cached.image = fallback(requestedSize);
  }

  if (size) *size = cached.image.size();
  return cached.image;
}

QImage FrameImageProvider::readFrame(const QString &output, quint64 expectedFrame,
                                     quint64 *actualFrame) const {
  Q_UNUSED(expectedFrame);
  if (actualFrame) *actualFrame = 0;
  if (output.isEmpty()) return {};
  const QByteArray name = bridgeShmName(output).toUtf8();
  MappedRegion region;
  region.fd = ::shm_open(name.constData(), O_RDONLY | O_CLOEXEC, 0);
  if (region.fd < 0) return {};
  struct stat statBuffer {};
  if (::fstat(region.fd, &statBuffer) != 0 ||
      statBuffer.st_size < static_cast<off_t>(sizeof(FrameHeader))) {
    return {};
  }
  region.size = static_cast<size_t>(statBuffer.st_size);
  region.data = ::mmap(nullptr, region.size, PROT_READ, MAP_SHARED, region.fd, 0);
  if (region.data == MAP_FAILED) {
    region.data = nullptr;
    return {};
  }

  // Retry once if the producer publishes a new frame while we are copying.
  for (int attempt = 0; attempt < 2; ++attempt) {
    const auto *header = static_cast<const FrameHeader *>(region.data);
    if (std::memcmp(header->magic, "ANIS", 4) != 0 ||
        header->width == 0 || header->height == 0 ||
        header->width > kMaxWidth || header->height > kMaxHeight ||
        header->stride < header->width * 4U) {
      return {};
    }
    const quint64 payload = static_cast<quint64>(header->stride) * header->height;
    if (payload > region.size - sizeof(FrameHeader)) return {};
    const quint64 before = loadFrameNo(header);
    if (before == 0) return {};
    const uchar *pixels = reinterpret_cast<const uchar *>(region.data) +
                          sizeof(FrameHeader);
    QImage view(pixels, static_cast<int>(header->width),
                static_cast<int>(header->height), static_cast<int>(header->stride),
                QImage::Format_RGBA8888);
    const QImage copy = view.copy();
    const quint64 after = loadFrameNo(header);
    if (before == after && std::memcmp(header->magic, "ANIS", 4) == 0) {
      if (actualFrame) *actualFrame = after;
      return copy;
    }
  }
  return {};
}

QImage FrameImageProvider::fallback(const QSize &requestedSize) const {
  const QString installed = QStandardPaths::locate(
      QStandardPaths::GenericDataLocation, QStringLiteral("anispaper/fallback.jpg"),
      QStandardPaths::LocateFile);
  QImage image;
  if (!installed.isEmpty() && QFileInfo::exists(installed)) {
    QImageReader reader(installed);
    image = reader.read();
  }
  const QSize target = requestedSize.isValid()
                           ? requestedSize.boundedTo(QSize(1920, 1080))
                           : QSize(640, 360);
  if (image.isNull()) {
    image = QImage(qMax(64, target.width()), qMax(64, target.height()),
                   QImage::Format_RGBA8888);
    image.fill(QColor(QStringLiteral("#101622")));
    QPainter painter(&image);
    painter.setPen(QColor(QStringLiteral("#FFD000")));
    QFont font = painter.font();
    font.setBold(true);
    font.setPointSize(qMax(12, image.height() / 16));
    painter.setFont(font);
    painter.drawText(image.rect(), Qt::AlignCenter,
                     QStringLiteral("ANISPAPER BRIDGE"));
  }
  return image.convertToFormat(QImage::Format_RGBA8888);
}
