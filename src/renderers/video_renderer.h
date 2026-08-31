#pragma once

#include "renderer.h"
#include "shm_frame_transport.h"

#include <QByteArray>
#include <QSize>
#include <QTimer>

#include <qopengl.h>

#include <atomic>
#include <memory>

class QOffscreenSurface;
class QOpenGLContext;
class QOpenGLFramebufferObject;
class QOpenGLFunctions;
struct mpv_handle;
struct mpv_render_context;

// This class is constructed only by the isolated --renderer-child entrypoint.
// libmpv renders into an OpenGL FBO (with GPU decoding offloaded through the
// detected vendor decoder), then the worker reads RGBA pixels back through a
// small PBO ring so the GPU pipeline is never stalled by a synchronous
// glReadPixels at 4K.  Frames leave the child either through the binary SHM
// transport (see ChildProtocol) or, as a legacy fallback, JPEG over JSON.
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
  void applyPlayback(int fps, double volume) override;
  // Direct-publish hook: when the child protocol owns a ShmFrameTransport
  // matching this renderer's geometry, readbacks land straight in the
  // transport slot (PBO map -> slot memcpy, GL bottom-up rows flipped in the
  // same pass) and the per-frame frame_ QImage copy disappears.
  void setFrameTransport(ShmFrameTransport *transport);

 private:
  static void *getProcAddress(void *context, const char *name);
  static void onMpvUpdate(void *context);
  void pumpEvents();
  bool flipFrameInPlace();
  void renderFrame();
  int effectiveFps() const;
  void fail(const QString &reason);

  // Asynchronous GPU->CPU readback ring.  Submitting a read after each mpv
  // render and consuming the fence-signalled oldest one on the next tick keeps
  // one frame of latency but zero pipeline stalls.
  bool initializeAsyncReadback();
  void submitAsyncReadback();
  bool consumeOldestReadback();
  void releaseReadbackResources();

  std::unique_ptr<QOffscreenSurface> surface_;
  std::unique_ptr<QOpenGLContext> context_;
  std::unique_ptr<QOpenGLFramebufferObject> fbo_;
  QOpenGLFunctions *gl_ = nullptr;
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
  int nativeFps_ = 60;

  // GL 3.2+ fence/mmap entry points, resolved once from the context.  The
  // offscreen surface requests a 2.1 profile, so the versioned Qt function
  // classes cannot be relied on to expose them.
  struct SyncApi {
    void *(*fenceSync)(unsigned int, unsigned int) = nullptr;
    unsigned int (*clientWaitSync)(void *, unsigned int, quint64) = nullptr;
    void (*deleteSync)(void *) = nullptr;
    void *(*mapBufferRange)(unsigned int, qint64, qint64, unsigned int) = nullptr;
    unsigned char (*unmapBuffer)(unsigned int) = nullptr;
    bool valid() const {
      return fenceSync && clientWaitSync && deleteSync && mapBufferRange && unmapBuffer;
    }
  };
  SyncApi sync_;

  struct PackRead {
    quint32 pbo = 0;
    void *fence = nullptr;  // GLsync, kept opaque in this header
  };
  PackRead inflight_[3];
  int inflightHead_ = 0;
  int inflightCount_ = 0;
  quint32 freePbos_[3];
  int freeCount_ = 0;
  bool asyncReadback_ = false;
  QSize readbackSize_;
  // Non-owning direct-publish transport (see setFrameTransport).
  ShmFrameTransport *frameTransport_ = nullptr;
};
