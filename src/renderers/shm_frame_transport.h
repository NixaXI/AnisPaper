#pragma once

#include "../bridge/frame_protocol.h"

#include <QImage>
#include <QString>
#include <QtGlobal>

#include <cstddef>

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
