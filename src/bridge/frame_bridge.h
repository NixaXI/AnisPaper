#pragma once

#include "frame_protocol.h"

#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QString>

#include <memory>

// POSIX shared memory is used rather than an anonymous memfd because the
// Plasma plugin is an independent process and must be able to map the bridge
// by output name.  The stable public name is /anispaper-<sanitized-output>.
QString sanitizeBridgeOutput(const QString &output);
QString bridgeShmName(const QString &output);

class FrameBridge final {
 public:
  FrameBridge() = default;
  ~FrameBridge();

  FrameBridge(const FrameBridge &) = delete;
  FrameBridge &operator=(const FrameBridge &) = delete;

  bool open(const QString &output, const QImage &fallback, QString *error);
  // The bridge surface is allocated at the output's physical pixel size.  The
  // legacy overload above remains for the standalone provider tests.
  bool open(const QString &output, const QImage &fallback, const QSize &frameSize,
            QString *error);
  bool open(const QString &output, const QImage &fallback, const QSize &frameSize,
            const QString &scaleMode, QString *error);
  bool publish(const QImage &image, QString *error = nullptr);
  void close();

  bool isOpen() const;
  QString name() const;
  quint64 frameNo() const;
  QSize frameSize() const;
  quint32 stride() const;
  QJsonObject status() const;

 private:
  bool mapForSize(const QSize &size, QString *error);
  static quint64 monotonicNs();

  int fd_ = -1;
  void *mapping_ = nullptr;
  qsizetype mappingSize_ = 0;
  FrameHeader *header_ = nullptr;
  QString name_;
  QSize size_;
  quint32 stride_ = 0;
  quint64 nextFrameNo_ = 0;
  bool ownsName_ = false;
  QString scaleMode_ = QStringLiteral("cover");
};

class FrameBridgeManager final {
 public:
  ~FrameBridgeManager();
  bool ensure(const QString &output, const QImage &fallback, QString *error);
  bool ensure(const QString &output, const QImage &fallback, const QSize &frameSize,
              QString *error);
  bool ensure(const QString &output, const QImage &fallback, const QSize &frameSize,
              const QString &scaleMode, QString *error);
  bool publish(const QString &output, const QImage &frame, QString *error = nullptr);
  void stop(const QString &output);
  QJsonObject statusFor(const QString &output) const;

 private:
  QHash<QString, FrameBridge *> bridges_;
};
