#include "../src/bridge/frame_bridge.h"

#include <QCoreApplication>
#include <QImage>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {
constexpr int kWidth = 64;
constexpr int kHeight = 64;
constexpr quint32 kStride = kWidth * 4;
constexpr quint32 kBuffers = 3;

bool require(bool value, const char *message) {
  if (!value) std::fprintf(stderr, "scene_transport_bridge_test: %s\n", message);
  return value;
}

QImage solid(QRgb pixel) {
  QImage image(kWidth, kHeight, QImage::Format_RGBA8888);
  image.fill(pixel);
  return image;
}

struct SourceTransport {
  std::vector<uchar> bytes;
  SceneTransportHeader *header = nullptr;
  uchar *slotData = nullptr;

  SourceTransport()
      : bytes(sizeof(SceneTransportHeader) + size_t(kStride) * kHeight * kBuffers, 0) {
    header = reinterpret_cast<SceneTransportHeader *>(bytes.data());
    slotData = bytes.data() + sizeof(SceneTransportHeader);
    std::memcpy(header->magic, kSceneTransportMagic, 4);
    header->version = kSceneTransportVersion;
    header->width = kWidth;
    header->height = kHeight;
    header->stride = kStride;
    header->format = kSceneTransportFormatRgba8888;
    header->buffers = kBuffers;
    __atomic_store_n(&header->frameNo, 0ULL, __ATOMIC_RELEASE);
    __atomic_store_n(&header->writeIndex, 0U, __ATOMIC_RELEASE);
  }

  SceneTransportView view() const {
    return {header, slotData, size_t(kStride) * kHeight, kBuffers, kWidth, kHeight,
            kStride, kSceneTransportFormatRgba8888};
  }

  void publish(quint64 sequence, QRgb pixel) {
    const quint32 index = static_cast<quint32>(sequence % kBuffers);
    std::memset(slotData + size_t(index) * kStride * kHeight,
                qRed(pixel), size_t(kStride) * kHeight);
    // Fill exactly RGBA, rather than rely on an endian-specific packed QRgb.
    uchar *slot = slotData + size_t(index) * kStride * kHeight;
    for (size_t offset = 0; offset < size_t(kStride) * kHeight; offset += 4) {
      slot[offset] = static_cast<uchar>(qRed(pixel));
      slot[offset + 1] = static_cast<uchar>(qGreen(pixel));
      slot[offset + 2] = static_cast<uchar>(qBlue(pixel));
      slot[offset + 3] = static_cast<uchar>(qAlpha(pixel));
    }
    __atomic_store_n(&header->writeIndex, index, __ATOMIC_RELEASE);
    __atomic_store_n(&header->frameNo, sequence, __ATOMIC_RELEASE);
  }
};

bool isSolid(const QImage &image, QRgb pixel) {
  if (image.size() != QSize(kWidth, kHeight) || image.format() != QImage::Format_RGBA8888) {
    return false;
  }
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      if (image.pixel(x, y) != pixel) return false;
    }
  }
  return true;
}
}  // namespace

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  bool ok = true;
  ok = require(sizeof(FrameHeader) == 32, "public FrameHeader size changed") && ok;
  ok = require(offsetof(FrameHeader, frameNo) == 12, "public FrameHeader frameNo offset changed") && ok;

  FrameBridge first;
  QString error;
  ok = require(first.open(QStringLiteral("scene-transport-test-one"), solid(qRgba(1, 2, 3, 255)),
                          QSize(kWidth, kHeight), &error),
               error.toUtf8().constData()) && ok;
  SourceTransport source;
  quint64 last = 0;
  source.publish(1, qRgba(220, 30, 40, 255));
  ok = require(first.publishSceneTransport(source.view(), &last) ==
                   SceneTransportPublishResult::Published,
               "stable direct source did not publish") && ok;
  ok = require(last == 1 && isSolid(first.snapshot(), qRgba(220, 30, 40, 255)),
               "stable direct source pixels are not independent/complete") && ok;
  ok = require(first.publishSceneTransport(source.view(), &last) ==
                   SceneTransportPublishResult::NoNewFrame,
               "duplicate source sequence was republished") && ok;

  // A changed writer index cannot be used as a stable source.  The following
  // valid publication proves recovery, while the prior bridge snapshot proves
  // no mismatched slot bytes were accepted.
  source.header->writeIndex = 0;
  __atomic_store_n(&source.header->frameNo, 2ULL, __ATOMIC_RELEASE);
  ok = require(first.publishSceneTransport(source.view(), &last) ==
                   SceneTransportPublishResult::Raced,
               "mismatched sequence/index was accepted") && ok;
  ok = require(isSolid(first.snapshot(), qRgba(220, 30, 40, 255)),
               "raced source replaced the last verified bridge image") && ok;
  source.publish(2, qRgba(20, 180, 70, 255));
  ok = require(first.publishSceneTransport(source.view(), &last) ==
                   SceneTransportPublishResult::Published &&
                   isSolid(first.snapshot(), qRgba(20, 180, 70, 255)),
               "direct path did not recover after a bounded retry") && ok;

  // Deterministically overwrite the same triple-buffer slot halfway through
  // the direct copy.  The first attempt therefore has red/blue mixed bytes in
  // the public payload while frameNo is busy; the post-copy source sequence
  // check must reject it and the bounded retry must publish only solid blue.
  source.publish(1, qRgba(210, 25, 25, 255));
  last = 0;
  bool hookRan = false;
  first.setSceneCopyHookForTesting([&] {
    if (hookRan) return;
    hookRan = true;
    source.publish(4, qRgba(25, 25, 210, 255));  // 1 % 3 == 4 % 3
  });
  ok = require(first.publishSceneTransport(source.view(), &last) ==
                   SceneTransportPublishResult::Published && hookRan && last == 4 &&
                   isSolid(first.snapshot(), qRgba(25, 25, 210, 255)),
               "mid-copy source sequence change yielded a stable mixed frame") && ok;
  first.setSceneCopyHookForTesting({});

  // Sequence modulo is deterministic at the unsigned wrap boundary; 0 stays
  // reserved as unpublished and is never supplied as a source frame.
  source.publish(std::numeric_limits<quint64>::max(), qRgba(40, 70, 230, 255));
  ok = require(first.publishSceneTransport(source.view(), &last) ==
                   SceneTransportPublishResult::Published &&
                   isSolid(first.snapshot(), qRgba(40, 70, 230, 255)),
               "wrap-boundary source sequence was not copied from its deterministic slot") && ok;

  SceneTransportView mismatched = source.view();
  ++mismatched.width;
  ok = require(first.publishSceneTransport(mismatched, &last) ==
                   SceneTransportPublishResult::Ineligible,
               "geometry mismatch did not select the normal-image fallback") && ok;
  SceneTransportView invalid = source.view();
  invalid.buffers = 0;
  ok = require(first.publishSceneTransport(invalid, &last) ==
                   SceneTransportPublishResult::Ineligible,
               "invalid metadata did not select the normal-image fallback") && ok;

  FrameBridge second;
  error.clear();
  ok = require(second.open(QStringLiteral("scene-transport-test-two"), solid(qRgba(4, 5, 6, 255)),
                           QSize(kWidth, kHeight), &error),
               error.toUtf8().constData()) && ok;
  SourceTransport secondSource;
  quint64 secondLast = 0;
  secondSource.publish(1, qRgba(90, 200, 240, 255));
  ok = require(second.publishSceneTransport(secondSource.view(), &secondLast) ==
                   SceneTransportPublishResult::Published &&
                   isSolid(first.snapshot(), qRgba(40, 70, 230, 255)) &&
                   isSolid(second.snapshot(), qRgba(90, 200, 240, 255)),
               "two output bridges are not pattern-independent") && ok;

  // The on-demand snapshot follows the same busy-marker rule as Plasma: a
  // zero marker is never returned as a stable image.
  const QByteArray bridgeName = first.name().toUtf8();
  const int fd = ::shm_open(bridgeName.constData(), O_RDWR, 0);
  if (fd >= 0) {
    void *mapping = ::mmap(nullptr, sizeof(FrameHeader), PROT_READ | PROT_WRITE,
                           MAP_SHARED, fd, 0);
    if (mapping != MAP_FAILED) {
      auto *header = static_cast<FrameHeader *>(mapping);
      storeFrameNo(header, 0);
      ok = require(first.snapshot().isNull(), "busy marker was accepted as a stable snapshot") && ok;
      ::munmap(mapping, sizeof(FrameHeader));
    } else {
      ok = require(false, "could not map bridge header for busy-marker test") && ok;
    }
    ::close(fd);
  } else {
    ok = require(false, "could not open bridge header for busy-marker test") && ok;
  }

  if (ok) std::puts("scene_transport_bridge_test: PASS");
  return ok ? 0 : 1;
}
