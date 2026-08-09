#pragma once

#include "renderer.h"

#include <QTimer>

#include <memory>

class QWebEngineView;

// QtWebEngine renderer.  It runs inside the isolated worker and samples the
// widget through QWidget::grab at the configured fps (normally 30).
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

 private:
  void captureFrame();
  void activateFallback(const QString &reason);

  std::unique_ptr<QWebEngineView> view_;
  QTimer frameTimer_;
  QImage frame_;
  bool running_ = false;
  bool paused_ = false;
  bool loaded_ = false;
  bool fallback_ = false;
  int frameCount_ = 0;
  qint64 fpsEpochMs_ = 0;
  double fps_ = 0.0;
};
