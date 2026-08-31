#include "shm_frame_transport.h"

#include <QDir>

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
quint64 monotonicNs() {
  timespec stamp{};
  if (::clock_gettime(CLOCK_MONOTONIC, &stamp) != 0) return 0;
  return static_cast<quint64>(stamp.tv_sec) * 1000000000ULL +
         static_cast<quint64>(stamp.tv_nsec);
}
}  // namespace

bool ShmFrameTransport::create(int width, int height) {
  destroy();
  if (width < 64 || height < 64) return false;
  // Same physical-mode ceiling as the bridge; oversized requests would only
  // allocate RAM the daemon can never republish.
  if (width > 3840 || height > 2160) return false;

  slotBytes_ = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
  mappingSize_ = sizeof(SceneTransportHeader) + slotBytes_ * kBuffers;
  name_ = QStringLiteral("/anispaper-scene-") + QString::number(::getpid());

  const QByteArray native = name_.toLocal8Bit();
  fd_ = ::shm_open(native.constData(), O_RDWR | O_CREAT | O_EXCL, 0600);
  if (fd_ < 0 && errno == EEXIST) {
    // Stale object from a crashed previous child with a recycled pid.
    ::shm_unlink(native.constData());
    fd_ = ::shm_open(native.constData(), O_RDWR | O_CREAT | O_EXCL, 0600);
  }
  if (fd_ < 0 || ::ftruncate(fd_, static_cast<off_t>(mappingSize_)) != 0) {
    destroy();
    return false;
  }
  mapping_ = ::mmap(nullptr, mappingSize_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
  if (mapping_ == MAP_FAILED) {
    mapping_ = nullptr;
    destroy();
    return false;
  }
  header_ = static_cast<SceneTransportHeader *>(mapping_);
  std::memset(header_, 0, sizeof(*header_));
  std::memcpy(header_->magic, kSceneTransportMagic, 4);
  header_->version = kSceneTransportVersion;
  header_->width = static_cast<quint32>(width);
  header_->height = static_cast<quint32>(height);
  header_->stride = static_cast<quint32>(width) * 4;
  header_->format = kSceneTransportFormatRgba8888;
  header_->buffers = kBuffers;
  __atomic_store_n(&header_->frameNo, 0ULL, __ATOMIC_RELEASE);
  __atomic_store_n(&header_->writeIndex, 0U, __ATOMIC_RELEASE);
  header_->timestampNs = monotonicNs();
  slotBase_ = static_cast<uchar *>(mapping_) + sizeof(SceneTransportHeader);
  width_ = width;
  height_ = height;
  stride_ = header_->stride;
  frameNo_ = 0;
  return true;
}

bool ShmFrameTransport::publish(const QImage &frame) {
  if (!header_ || frame.isNull()) return false;
  if (frame.format() != QImage::Format_RGBA8888 ||
      frame.width() != width_ || frame.height() != height_) {
    return false;
  }
  // RGBA8888 rows are 4-byte aligned by definition, but QImage may still pad
  // the very last row's line length on exotic allocations; copy row-wise from
  // the image's own stride.
  const quint64 next = frameNo_ + 1;
  uchar *slot = slotBase_ + static_cast<size_t>(next % kBuffers) * slotBytes_;
  const uchar *source = frame.constBits();
  const qsizetype sourceStride = frame.bytesPerLine();
  for (int row = 0; row < height_; ++row) {
    std::memcpy(slot + static_cast<size_t>(row) * stride_,
                source + static_cast<size_t>(row) * sourceStride, stride_);
  }
  header_->timestampNs = monotonicNs();
  // Publication order matches the scene engine writer: index first, then the
  // sequence, both release, so readers that acquire frameNo always see the
  // completed slot it points at.
  __atomic_store_n(&header_->writeIndex, static_cast<quint32>(next % kBuffers),
                   __ATOMIC_RELEASE);
  __atomic_store_n(&header_->frameNo, next, __ATOMIC_RELEASE);
  frameNo_ = next;
  return true;
}

void ShmFrameTransport::destroy() {
  if (mapping_) {
    ::munmap(mapping_, mappingSize_);
    mapping_ = nullptr;
  }
  header_ = nullptr;
  slotBase_ = nullptr;
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  if (!name_.isEmpty()) {
    ::shm_unlink(name_.toLocal8Bit().constData());
    name_.clear();
  }
  mappingSize_ = 0;
  slotBytes_ = 0;
  width_ = 0;
  height_ = 0;
  stride_ = 0;
  frameNo_ = 0;
}

ShmFrameTransport::~ShmFrameTransport() { destroy(); }
