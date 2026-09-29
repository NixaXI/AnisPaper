#pragma once

#include <QHash>
#include <QElapsedTimer>
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

  struct ProfileCounters {
    quint64 requests = 0;
    quint64 newSequences = 0;
    quint64 repeatedFrames = 0;
    quint64 lastFrameNo = 0;
    qint64 windowStartMs = -1;
    qint64 totalRequestNs = 0;
    qint64 maxRequestNs = 0;
  };

  struct CachedFrame {
    QImage image;
    quint64 frameNo = 0;
    bool resetPending = false;
    std::shared_ptr<MappedRegion> mapping;
  };

  QImage fallback(const QSize &requestedSize) const;
  QImage readFrame(const QString &output, quint64 expectedFrame,
                   CachedFrame &cached, quint64 *actualFrame);
  void updateProfilingState();
  void recordProfile(const QString &output, quint64 returnedFrameNo,
                     qint64 requestNs);

  mutable QMutex mutex_;
  QHash<QString, CachedFrame> cache_;
  QHash<QString, ProfileCounters> profileCounters_;
  QElapsedTimer profileClock_;
  QElapsedTimer profileToggleClock_;
  bool profilingEnabled_ = false;
};
