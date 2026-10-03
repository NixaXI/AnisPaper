#pragma once

#include "renderer.h"

#include <QElapsedTimer>
#include <QTimer>

#include <memory>

class QOffscreenSurface;
class QOpenGLContext;
class QOpenGLFramebufferObject;
class QQmlEngine;
class QQuickRenderControl;
class QQuickWebEngineProfile;
class QQuickWindow;

// Isolated QtWebEngine wallpaper.  The page lives in a QtWebEngineQuick view
// inside an offscreen QQuickWindow driven by QQuickRenderControl: Chromium
// composites into our own GL framebuffer, so no on-screen (hidden) surface and
// no QWidget::grab()/canvas readback is involved.  Remote/network main frames
// are blocked; frames are read back once per changed scene.
class WebRenderer final : public Renderer {
  Q_OBJECT

 public:
  explicit WebRenderer(RendererSpec spec, QObject *parent = nullptr);
  ~WebRenderer() override;

  // Must run before the QApplication is constructed in a web child.
  static void prepareProcess();

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

  Q_INVOKABLE void logConsole(int level, const QString &message, int line,
                              const QString &source);

 private slots:
  void onPageLoaded(bool ok);

 private:
  bool createScene(QString *reason);
  void destroyScene();
  void renderFrame();
  void acceptFrame(const QImage &image);
  void activateFallback(const QString &reason);
  QImage placeholderFrame(const QString &reason) const;
  void runJavaScript(const QString &code);
  void applyMediaVolume();
  int frameIntervalMs() const;

  std::unique_ptr<QOpenGLContext> context_;
  std::unique_ptr<QOffscreenSurface> surface_;
  std::unique_ptr<QQuickRenderControl> renderControl_;
  std::unique_ptr<QQuickWindow> window_;
  std::unique_ptr<QOpenGLFramebufferObject> fbo_;
  std::unique_ptr<QQuickWebEngineProfile> profile_;
  std::unique_ptr<QQmlEngine> engine_;
  QObject *root_ = nullptr;
  QTimer frameTimer_;
  QImage frame_;
  QElapsedTimer loadClock_;
  bool running_ = false;
  bool paused_ = false;
  bool loaded_ = false;
  bool fallback_ = true;
  quint64 lastHash_ = 0;
  int frameCount_ = 0;
  qint64 fpsEpochMs_ = 0;
  double fps_ = 0.0;
};
