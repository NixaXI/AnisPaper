#pragma once

#include "renderer.h"

#include <QElapsedTimer>
#include <QProcess>

// Parent-side adapter.  The implementation process is the same executable in
// --renderer-child mode, so a libmpv or Chromium failure cannot terminate the
// daemon that owns the Unix JSON-RPC socket.
class IsolatedRenderer final : public Renderer {
  Q_OBJECT

 public:
  explicit IsolatedRenderer(RendererSpec spec, QObject *parent = nullptr);
  ~IsolatedRenderer() override;

  bool start(QString *error) override;
  void stop() override;
  void pause() override;
  void resume() override;
  QImage lastFrame() const override;
  QString rendererName() const override;
  bool isRunning() const override;
  bool isFallback() const override;
  qint64 processId() const override;
  double frameRate() const override;

 private:
  void sendCommand(const QString &command);
  void readFrames();
  void parseLine(const QByteArray &line);
  void reportFatal(const QString &reason);
  void cleanupProcessGroup();

  QProcess process_;
  QByteArray input_;
  QImage frame_;
  QElapsedTimer fpsClock_;
  int frameCount_ = 0;
  double fps_ = 0.0;
  bool running_ = false;
  bool stopRequested_ = false;
  bool fallback_ = false;
  bool fatalReported_ = false;
  QString childFailure_;
  qint64 childPid_ = 0;
};
