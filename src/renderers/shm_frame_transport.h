#pragma once

#include "../bridge/frame_protocol.h"

#include <QImage>
#include <QString>
#include <QtGlobal>

#include <cstddef>
#include <ctime>

namespace anispaper {
inline quint64 monotonicNsHeader() {
  timespec stamp{};
  if (::clock_gettime(CLOCK_MONOTONIC, &stamp) != 0) return 0;
  return static_cast<quint64>(stamp.tv_sec) * 1000000000ULL +
         static_cast<quint64>(stamp.tv_nsec);
}
}  // namespace anispaper

// Child-side (single writer) frame transport for the isolated video/web
// renderer workers.  Wire ABI is identical to the scene engine transport
// (SceneTransportHeader followed by kBuffers RGBA8888 slots, latest-frame-wins
// with release-ordered publication), so the daemon consumes both through the
// same validated read path in IsolatedRenderer/FrameBridge.
//
// Replaces the legacy JPEG/base64-over-JSON frame path at 1080p+ resolutions:
// a 3840x2160 RGBA frame is a single memcpy into the next slot instead of a
// JPEG encode + base64 inflate + pipe write + parent decode per frame.
class ShmFrameTransport {
 public:
  static constexpr quint32 kBuffers = 3;

  // Creates `/anispaper-scene-<pid>` sized for width x height RGBA frames.
  // The "scene" namespace is shared on purpose: daemon boot cleanup and the
  // parent's unlink guard already own that prefix for renderer children.
  bool create(int width, int height);

  // Copies `frame` into the next slot and publishes it.  Returns false (and
  // keeps the previous frame published) when the frame does not match the
  // transport geometry, letting the caller fall back to the JSON path.
  bool publish(const QImage &frame);

  // Zero-copy publish path: hands the caller the raw next slot and only
  // flips the publication sequence once `writer` returns true.  `writer`
  // receives (slotDestination, slotStride) and must fill width x height
  // RGBA pixels.  This exists for renderers that can land their readback
  // directly in the final buffer (PBO map -> slot), skipping the transient
  // per-frame QImage entirely.
  template <typename Writer>
  bool publishWith(Writer &&writer) {
    if (!header_ || width_ <= 0 || height_ <= 0) return false;
    const quint64 next = frameNo_ + 1;
    uchar *slot = slotBase_ + static_cast<size_t>(next % kBuffers) * slotBytes_;
    if (!writer(slot, static_cast<qsizetype>(stride_))) return false;
    header_->timestampNs = anispaper::monotonicNsHeader();
    __atomic_store_n(&header_->writeIndex, static_cast<quint32>(next % kBuffers),
                     __ATOMIC_RELEASE);
    __atomic_store_n(&header_->frameNo, next, __ATOMIC_RELEASE);
    frameNo_ = next;
    return true;
  }

  // Geometry the writer must produce (exposed for renderers that verify
  // their output size against the transport before submitting).
  int slotWidth() const { return width_; }
  int slotHeight() const { return height_; }
  bool matchesGeometry(int width, int height) const {
    return mapping_ != nullptr && width == width_ && height == height_;
  }

  QString name() const { return name_; }
  quint64 frameNo() const { return frameNo_; }
  int width() const { return width_; }
  int height() const { return height_; }
  quint32 stride() const { return stride_; }
  bool isActive() const { return mapping_ != nullptr; }

  void destroy();
  ~ShmFrameTransport();

 private:
  QString name_;
  int fd_ = -1;
  void *mapping_ = nullptr;
  size_t mappingSize_ = 0;
  SceneTransportHeader *header_ = nullptr;
  uchar *slotBase_ = nullptr;
  size_t slotBytes_ = 0;
  int width_ = 0;
  int height_ = 0;
  quint32 stride_ = 0;
  quint64 frameNo_ = 0;

 public:
  ShmFrameTransport() = default;
  ShmFrameTransport(const ShmFrameTransport &) = delete;
  ShmFrameTransport &operator=(const ShmFrameTransport &) = delete;
};
