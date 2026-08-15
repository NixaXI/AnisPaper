#pragma once

#include "renderer.h"

#include <QByteArray>
#include <QTimer>

#include <atomic>
#include <memory>

class QOffscreenSurface;
class QOpenGLContext;
class QOpenGLFramebufferObject;
struct mpv_handle;
struct mpv_render_context;

// This class is constructed only by the isolated --renderer-child entrypoint.
// libmpv renders into an OpenGL FBO, then the worker reads RGBA pixels back to
// QImage for the daemon-side frame bridge.
class VideoRenderer final : public Renderer {
  Q_OBJECT

 public:
  explicit VideoRenderer(RendererSpec spec, QObject *parent = nullptr);
  ~VideoRenderer() override;

  bool start(QString *error) override;
  void stop() override;
  void pause() override;
  void resume() override;
  QImage lastFrame() const override;
  QString rendererName() const override;
  bool isRunning() const override;
  double frameRate() const override;

 private:
  static void *getProcAddress(void *context, const char *name);
  static void onMpvUpdate(void *context);
  void pumpEvents();
  bool flipFrameInPlace();
  void renderFrame();
  void fail(const QString &reason);

  std::unique_ptr<QOffscreenSurface> surface_;
  std::unique_ptr<QOpenGLContext> context_;
  std::unique_ptr<QOpenGLFramebufferObject> fbo_;
  mpv_handle *mpv_ = nullptr;
  mpv_render_context *renderContext_ = nullptr;
  QTimer frameTimer_;
  QImage frame_;
  QByteArray flipScratch_;
  bool running_ = false;
  bool paused_ = false;
  bool failed_ = false;
  std::atomic_bool framePending_{true};
  int frameCount_ = 0;
  qint64 fpsEpochMs_ = 0;
  double fps_ = 0.0;
  bool sourceRateConfigured_ = false;
};
