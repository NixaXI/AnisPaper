#pragma once

#include "renderer.h"

#include <QElapsedTimer>
#include <QTimer>
#include <QVariant>

#include <memory>

class QWebEngineProfile;
class QWebEngineView;

// Isolated QtWebEngine wallpaper.  Remote/network URLs are blocked; frames come
// from a canvas readback (WebGL/2D) with QWidget::grab as a local-HTML fallback.
class WebRenderer final : public Renderer {
  Q_OBJECT

 public:
  explicit WebRenderer(RendererSpec spec, QObject *parent = nullptr);
  ~WebRenderer() override;

  bool start(QString *error) override;
  void stop() override;
  void pause() override;
  void resume() override;
  QImage lastFrame() const override;
  QString rendererName() const override;
  bool isRunning() const override;
  bool isFallback() const override;
  double frameRate() const override;
  void applyPlayback(int fps, double volume) override;

 public slots:
  void ingestCapturedFrame(const QString &dataUrl);

 private:
  void captureFrame();
  bool tryGrab();
  void advancePushGrid(qint64 now);
  void onJsCapture(const QVariant &result);
  void acceptFrame(const QImage &image);
  void activateFallback(const QString &reason);
  QImage placeholderFrame(const QString &reason) const;
  void injectScripts();
  void applyMediaVolume();

  std::unique_ptr<QWebEngineProfile> profile_;
  std::unique_ptr<QWebEngineView> view_;
  QTimer frameTimer_;
  QImage frame_;
  bool running_ = false;
  bool paused_ = false;
  bool loaded_ = false;
  bool fallback_ = true;
  bool grabWorks_ = false;
  bool jsInFlight_ = false;
  QElapsedTimer jsClock_;
  qint64 lastPushMs_ = 0;
  int frameCount_ = 0;
  qint64 fpsEpochMs_ = 0;
  double fps_ = 0.0;
};
