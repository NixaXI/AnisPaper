#pragma once

#include <QHash>
#include <QImage>
#include <QMutex>
#include <QQuickImageProvider>

#include <memory>

class FrameImageProvider final : public QQuickImageProvider {
 public:
  FrameImageProvider();
  QImage requestImage(const QString &id, QSize *size,
                      const QSize &requestedSize) override;

 private:
  struct MappedRegion;

  struct CachedFrame {
    QImage image;
    quint64 frameNo = 0;
    bool resetPending = false;
    std::shared_ptr<MappedRegion> mapping;
    QSize requestedSize;
  };

  QImage fallback(const QSize &requestedSize) const;
  QImage readFrame(const QString &output, quint64 expectedFrame,
                   CachedFrame &cached, quint64 *actualFrame);

  mutable QMutex mutex_;
  QHash<QString, CachedFrame> cache_;
};
