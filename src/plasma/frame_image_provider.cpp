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

struct FrameImageProvider::MappedRegion {
  int fd = -1;
  void *data = nullptr;
  size_t size = 0;
  ~MappedRegion() {
    if (data && data != MAP_FAILED) ::munmap(data, size);
    if (fd >= 0) ::close(fd);
  }
};

namespace {
constexpr quint32 kMaxWidth = 7680;
constexpr quint32 kMaxHeight = 4320;

quint64 requestedFrameNo(const QString &id) {
  const int question = id.indexOf(QLatin1Char('?'));
  if (question < 0) return 0;
  QUrlQuery query(id.mid(question + 1));
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
  CachedFrame &cached = cache_[output];
  // FrameWatcher reports a lower sequence when the daemon recreated the
  // bridge.  Drop the old mmap before reading so a new bridge with the same
  // output name can never be mistaken for the old producer.
  if (expected != 0 && cached.frameNo != 0 && expected < cached.frameNo) {
    cached.mapping.reset();
  }
  quint64 actual = 0;
  QImage current = readFrame(output, expected, cached, &actual);

  // A lower sequence means a new daemon has recreated the bridge.  Never
  // return bytes from the old mapping as if they belonged to the new writer.
  // Keep the last verified pixels until the new producer publishes a newer
  // sequence; no fallback/black intermediary is exposed.
  if (actual != 0 && cached.frameNo != 0 && actual < cached.frameNo) {
    cached.frameNo = actual;
    cached.resetPending = true;
  } else if (!current.isNull()) {
    if (cached.resetPending && actual == cached.frameNo) {
      // The recreated shm object has published its initial frame only. Keep
      // the previous image for this tick, then accept the first newer value.
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

  // QML supplies the actual item size for image providers.  Keep the bridge
  // at physical output resolution, but avoid uploading a larger texture than
  // the WallpaperItem can display.  This is an aspect-preserving, smooth
  // downsample; cover/fit source-rectangle math in QML still operates on the
  // same aspect ratio and explicit stretch remains the only distortion mode.
  if (requestedSize.isValid() && requestedSize.width() > 0 &&
      requestedSize.height() > 0 && !cached.image.isNull()) {
    const QSize target = requestedSize;
    if (cached.requestedSize != target ||
        (cached.image.width() > target.width() &&
         cached.image.height() > target.height())) {
      if (cached.image.width() > target.width() ||
          cached.image.height() > target.height()) {
        cached.image = cached.image.scaled(target, Qt::KeepAspectRatio,
                                           Qt::SmoothTransformation);
      }
      cached.requestedSize = target;
    }
  }

  if (size) *size = cached.image.size();
  return cached.image;
}

QImage FrameImageProvider::readFrame(const QString &output, quint64 expectedFrame,
                                     CachedFrame &cached,
                                     quint64 *actualFrame) {
  if (actualFrame) *actualFrame = 0;
  if (output.isEmpty()) return {};

  const auto mapBridge = [&cached, &output]() -> bool {
    const QByteArray name = bridgeShmName(output).toUtf8();
    auto region = std::make_shared<MappedRegion>();
    region->fd = ::shm_open(name.constData(), O_RDONLY | O_CLOEXEC, 0);
    if (region->fd < 0) return false;
    struct stat statBuffer {};
    if (::fstat(region->fd, &statBuffer) != 0 ||
        statBuffer.st_size < static_cast<off_t>(sizeof(FrameHeader))) {
      return false;
    }
    region->size = static_cast<size_t>(statBuffer.st_size);
    region->data = ::mmap(nullptr, region->size, PROT_READ, MAP_SHARED,
                          region->fd, 0);
    if (region->data == MAP_FAILED) {
      region->data = nullptr;
      return false;
    }
    cached.mapping = std::move(region);
    return true;
  };

  bool remappedForExpectation = false;
  // Retry once if the producer publishes a new frame while we are copying or
  // if the first persistent mapping was the object unlinked during a daemon
  // recreation.
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (!cached.mapping && !mapBridge()) return {};
    const MappedRegion &region = *cached.mapping;
    const auto *header = static_cast<const FrameHeader *>(region.data);
    if (std::memcmp(header->magic, "ANIS", 4) != 0 ||
        header->width == 0 || header->height == 0 ||
        header->width > kMaxWidth || header->height > kMaxHeight ||
        header->stride < header->width * 4U) {
      cached.mapping.reset();
      return {};
    }
    const quint64 payload = static_cast<quint64>(header->stride) * header->height;
    if (payload > region.size - sizeof(FrameHeader)) return {};
    const quint64 before = loadFrameNo(header);
    if (before == 0) return {};
    if (expectedFrame != 0 && before < expectedFrame) {
      // The old object can remain mapped after shm_unlink.  A producer
      // sequence below the requested one is evidence that this mapping may be
      // stale; reopen once against the current bridge name. A freshly
      // recreated bridge can still be below the requested QML sequence, so
      // accept that value on the second attempt and let requestImage retain
      // the previous verified image until a newer value arrives.
      if (!remappedForExpectation) {
        cached.mapping.reset();
        remappedForExpectation = true;
        continue;
      }
    }
    const uchar *pixels = reinterpret_cast<const uchar *>(region.data) +
                          sizeof(FrameHeader);
    QImage view(pixels, static_cast<int>(header->width),
                static_cast<int>(header->height), static_cast<int>(header->stride),
                // Wallpaper frames are opaque.  Supplying Qt Quick's native
                // premultiplied format avoids a second format conversion in
                // the scene graph texture upload while preserving the RGBA
                // byte layout and the fixed bridge ABI.
                QImage::Format_RGBA8888_Premultiplied);
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
