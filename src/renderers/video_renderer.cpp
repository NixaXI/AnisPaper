#include "video_renderer.h"

#include "../common/gpu_vendor.h"

#include <QDateTime>
#include <QFileInfo>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFramebufferObjectFormat>
#include <QOpenGLFunctions>
#include <QSurfaceFormat>

#include <mpv/client.h>
#include <mpv/render_gl.h>

#include <clocale>
#include <cstring>

namespace {
QString mpvError(int code) {
  const char *text = mpv_error_string(code);
  return QStringLiteral("libmpv: %1").arg(QString::fromUtf8(text ? text : "error"));
}
}  // namespace

VideoRenderer::VideoRenderer(RendererSpec spec, QObject *parent)
    : Renderer(std::move(spec), parent) {
  frameTimer_.setTimerType(Qt::PreciseTimer);
  // The interval timer is a liveness keepalive now, not the render clock:
  // renders are driven by mpv's decode callback (queued) and paced by the
  // deadline guard in renderFrame.  A slow tick here only publishes a matured
  // PBO readback if the event path went quiet (decoder slower than the cap).
  frameTimer_.setInterval(200);
  connect(&frameTimer_, &QTimer::timeout, this, &VideoRenderer::renderFrame);
}

VideoRenderer::~VideoRenderer() { stop(); }

bool VideoRenderer::start(QString *error) {
  if (running_) {
    return true;
  }
  if (!QFileInfo(spec_.file).isFile()) {
    if (error) {
      *error = QStringLiteral("video source is unavailable");
    }
    return false;
  }

  QSurfaceFormat format;
  format.setRenderableType(QSurfaceFormat::OpenGL);
  // Legacy 2.1 context: libmpv renders into our FBO through its own GL
  // bindings, and the async readback resolves fence/map entry points from
  // the driver directly (see initializeAsyncReadback), so no core profile is
  // required.  The VAAPI zero-copy interop needs an EGL-owned context that
  // Qt's offscreen surface cannot provide; use the -copy variant instead
  // (see recommendedVideoHwdec) and keep the copy cheap with the decode
  // scale filter below.
  format.setProfile(QSurfaceFormat::NoProfile);
  format.setVersion(2, 1);
  format.setRedBufferSize(8);
  format.setGreenBufferSize(8);
  format.setBlueBufferSize(8);
  format.setAlphaBufferSize(8);

  surface_ = std::make_unique<QOffscreenSurface>();
  surface_->setFormat(format);
  surface_->create();
  if (!surface_->isValid()) {
    if (error) {
      *error = QStringLiteral("offscreen OpenGL surface is unavailable");
    }
    stop();
    return false;
  }
  context_ = std::make_unique<QOpenGLContext>();
  context_->setFormat(surface_->format());
  if (!context_->create() || !context_->makeCurrent(surface_.get())) {
    if (error) {
      *error = QStringLiteral("offscreen OpenGL context is unavailable");
    }
    stop();
    return false;
  }

  QOpenGLFramebufferObjectFormat fboFormat;
  fboFormat.setAttachment(QOpenGLFramebufferObject::NoAttachment);
  fboFormat.setInternalTextureFormat(GL_RGBA8);
  fbo_ = std::make_unique<QOpenGLFramebufferObject>(spec_.width, spec_.height,
                                                      fboFormat);
  if (!fbo_->isValid()) {
    context_->doneCurrent();
    if (error) {
      *error = QStringLiteral("offscreen framebuffer is unavailable");
    }
    stop();
    return false;
  }

  // libmpv deliberately rejects locales that use a comma decimal separator.
  // This worker is isolated, so normalizing LC_NUMERIC here cannot affect the
  // daemon's JSON-RPC or the user's UI process.
  std::setlocale(LC_NUMERIC, "C");
  mpv_ = mpv_create();
  if (!mpv_) {
    context_->doneCurrent();
    if (error) {
      *error = QStringLiteral("libmpv initialization failed");
    }
    stop();
    return false;
  }
  const auto setOption = [this](const char *name, const char *value) {
    return mpv_set_option_string(mpv_, name, value);
  };
  int result = setOption("vo", "libmpv");
  if (result >= 0) result = setOption("terminal", "no");
  // mpv 0.41 accepts "auto" (not "yes") for this option.  Auto keeps the
  // audio stream in the same player and falls back cleanly when a file has no
  // audio track or the session has no sink.
  if (result >= 0) result = setOption("audio", "auto");
  if (result >= 0) result = setOption("audio-client-name", "AnisPaper");
  if (result >= 0) result = setOption("mute", spec_.volume <= 0.0 ? "yes" : "no");
  if (result >= 0) result = setOption("keep-open", "yes");

  // GPU decode offload.  The vendor probe picks the interop that matches the
  // machine (VAAPI on Mesa, NVDEC on discrete NVIDIA); when the method is not
  // available for a file, mpv silently falls back to software decoding, so a
  // misdetected vendor can only cost speed, never playback.  ANISPAPER_VIDEO_HWDEC
  // ("no", "vaapi", "nvdec", "auto-safe", ...) overrides the probe for testing.
  const GpuVendor vendor = detectGpuVendor();
  const QByteArray overrideValue = qgetenv("ANISPAPER_VIDEO_HWDEC").trimmed();
  const QByteArray hwdecMethod = !overrideValue.isEmpty()
                                     ? overrideValue
                                     : recommendedVideoHwdec(vendor).toLatin1();
  if (result >= 0) result = setOption("hwdec", hwdecMethod.constData());
  if (result >= 0) result = setOption("hwdec-codecs", "all");
  // 4K sources render at the output size: mpv's GL renderer scales decoded
  // frames with GPU shaders when it composites into our FBO, so no video
  // filter is needed.  (A lavfi scale filter here would run swscale on the
  // CPU -- measured 124% for one 4K->1080p stream at 30 fps.)  The vaapi
  // -copy download still moves source-sized frames, which is a plain DMA
  // transfer the memory bus absorbs easily at 30 fps.
  // The frame timer renders at the effective wallpaper fps (usually the
  // 20 fps cap), while mpv's decoder otherwise runs at the file's native
  // rate (60 for most workshop videos).  Letting the decoder skip frames
  // the render context will never show cuts the vaapi-copy GPU->RAM
  // download to the displayed rate instead of the native one.
  if (result >= 0) result = setOption("framedrop", "decoder+vo");

  if (result >= 0) result = mpv_initialize(mpv_);
  if (result < 0) {
    context_->doneCurrent();
    if (error) {
      *error = mpvError(result);
    }
    stop();
    return false;
  }

  mpv_opengl_init_params glInit{};
  glInit.get_proc_address = &VideoRenderer::getProcAddress;
  glInit.get_proc_address_ctx = context_.get();
  mpv_render_param params[] = {
      {MPV_RENDER_PARAM_API_TYPE,
       const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL)},
      {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glInit},
      {MPV_RENDER_PARAM_INVALID, nullptr},
  };
  result = mpv_render_context_create(&renderContext_, mpv_, params);
  if (result < 0) {
    context_->doneCurrent();
    if (error) {
      *error = mpvError(result);
    }
    stop();
    return false;
  }
  framePending_.store(true, std::memory_order_release);
  mpv_render_context_set_update_callback(renderContext_, &VideoRenderer::onMpvUpdate,
                                         this);

  const QByteArray source = QFileInfo(spec_.file).absoluteFilePath().toUtf8();
  const char *load[] = {"loadfile", source.constData(), "replace", nullptr};
  result = mpv_command(mpv_, load);
  if (result >= 0) {
    const QByteArray volume = QByteArray::number(spec_.volume * 100.0, 'f', 1);
    const QByteArray speed = QByteArray::number(spec_.speed, 'f', 3);
    mpv_set_property_string(mpv_, "volume", volume.constData());
    mpv_set_property_string(mpv_, "speed", speed.constData());
    mpv_set_property_string(mpv_, "loop-file", spec_.loop ? "inf" : "no");
    mpv_set_property_string(mpv_, "mute", spec_.volume <= 0.0 ? "yes" : "no");
  }

  // One-time startup diagnostics on the child's stderr (the daemon forwards it
  // to the journal): what we probed, what we got, and how we will read back.
  auto *functions = context_->functions();
  const char *glRenderer =
      reinterpret_cast<const char *>(functions->glGetString(GL_RENDERER));
  initializeAsyncReadback();
  ::fprintf(stderr,
            "anispaper video: gpu-probe=%s gl-renderer=%s hwdec=%s%s readback=%s\n",
            gpuVendorId(vendor).toLatin1().constData(),
            glRenderer ? glRenderer : "unknown", hwdecMethod.constData(),
            overrideValue.isEmpty() ? "" : " (override)",
            asyncReadback_ ? "async-pbo" : "sync");
  ::fflush(stderr);

  context_->doneCurrent();
  if (result < 0) {
    if (error) {
      *error = mpvError(result);
    }
    stop();
    return false;
  }

  running_ = true;
  paused_ = false;
  failed_ = false;
  frameCount_ = 0;
  sourceRateConfigured_ = false;
  fpsEpochMs_ = QDateTime::currentMSecsSinceEpoch();
  frameTimer_.start();  // 200 ms keepalive (see constructor)
  return true;
}

void VideoRenderer::stop() {
  frameTimer_.stop();
  running_ = false;
  paused_ = false;
  framePending_.store(false, std::memory_order_release);
  if (renderContext_) {
    // Unregister before touching the GL context or mpv handle.  The update
    // callback may run on an mpv worker thread and must never outlive this
    // renderer instance, even if making the context current fails.
    mpv_render_context_set_update_callback(renderContext_, nullptr, nullptr);
  }
  if (context_ && surface_ && context_->makeCurrent(surface_.get())) {
    releaseReadbackResources();
    if (renderContext_) {
      mpv_render_context_free(renderContext_);
      renderContext_ = nullptr;
    }
    fbo_.reset();
    context_->doneCurrent();
  } else {
    renderContext_ = nullptr;
    fbo_.reset();
  }
  if (mpv_) {
    mpv_terminate_destroy(mpv_);
    mpv_ = nullptr;
  }
  context_.reset();
  surface_.reset();
}

void VideoRenderer::pause() {
  if (!running_ || paused_) {
    return;
  }
  paused_ = true;
  // mpv freezes on its pause property; the frame timer stays armed so the
  // watchdog still observes renderer liveness while nothing is decoded.
  mpv_set_property_string(mpv_, "pause", "yes");
}

void VideoRenderer::resume() {
  if (!running_ || !paused_) {
    return;
  }
  paused_ = false;
  framePending_.store(true, std::memory_order_release);
  mpv_set_property_string(mpv_, "pause", "no");
}

int VideoRenderer::effectiveFps() const {
  return sourceRateConfigured_ ? qMin(spec_.fps, nativeFps_)
                               : qBound(1, spec_.fps, 60);
}

QImage VideoRenderer::lastFrame() const { return frame_; }

QString VideoRenderer::rendererName() const { return QStringLiteral("video"); }

bool VideoRenderer::isRunning() const { return running_; }

double VideoRenderer::frameRate() const { return fps_; }

void VideoRenderer::applyPlayback(int fps, double volume) {
  Renderer::applyPlayback(fps, volume);
  if (mpv_) {
    const QByteArray value = QByteArray::number(spec_.volume * 100.0, 'f', 1);
    mpv_set_property_string(mpv_, "volume", value.constData());
    mpv_set_property_string(mpv_, "mute", spec_.volume <= 0.0 ? "yes" : "no");
  }
  if (running_ && !paused_) {
    frameTimer_.start();  // keepalive; pacing lives in renderFrame's guard
  }
}

void *VideoRenderer::getProcAddress(void *context, const char *name) {
  auto *glContext = static_cast<QOpenGLContext *>(context);
  const QFunctionPointer pointer = glContext->getProcAddress(name);
  return reinterpret_cast<void *>(pointer);
}

void VideoRenderer::onMpvUpdate(void *context) {
  auto *renderer = static_cast<VideoRenderer *>(context);
  if (!renderer) {
    return;
  }
  renderer->framePending_.store(true, std::memory_order_release);
  // mpv calls this from its own thread; queue the render on the child's event
  // loop so a finished decode is shown immediately instead of waiting for the
  // next timer tick.  The frame timer becomes a rate ceiling only (see
  // renderFrame's pacing guard), which removes the beat/jitter between the
  // decoder clock and a fixed 16 ms Qt timer.
  QMetaObject::invokeMethod(renderer, "renderFrame", Qt::QueuedConnection);
}

bool VideoRenderer::initializeAsyncReadback() {
  // Fence/mapped-readback entry points come from the driver, not from a
  // versioned Qt function class: the offscreen surface only requests a 2.1
  // profile while the fence API arrived in GL 3.2.  When any entry point is
  // missing the synchronous readback path below stays available.
  gl_ = context_->functions();
  const auto resolve = [this](const char *name) -> void * {
    return reinterpret_cast<void *>(context_->getProcAddress(name));
  };
  sync_.fenceSync =
      reinterpret_cast<decltype(sync_.fenceSync)>(resolve("glFenceSync"));
  sync_.clientWaitSync =
      reinterpret_cast<decltype(sync_.clientWaitSync)>(resolve("glClientWaitSync"));
  sync_.deleteSync =
      reinterpret_cast<decltype(sync_.deleteSync)>(resolve("glDeleteSync"));
  sync_.mapBufferRange =
      reinterpret_cast<decltype(sync_.mapBufferRange)>(resolve("glMapBufferRange"));
  sync_.unmapBuffer =
      reinterpret_cast<decltype(sync_.unmapBuffer)>(resolve("glUnmapBuffer"));
  if (!sync_.valid() || !gl_) {
    return false;
  }
  const size_t bytes = static_cast<size_t>(spec_.width) * static_cast<size_t>(spec_.height) * 4;
  freeCount_ = 0;
  for (int i = 0; i < 3; ++i) {
    gl_->glGenBuffers(1, &freePbos_[i]);
    gl_->glBindBuffer(GL_PIXEL_PACK_BUFFER, freePbos_[i]);
    gl_->glBufferData(GL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(bytes), nullptr,
                      GL_STREAM_READ);
    freeCount_++;
  }
  gl_->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  inflightHead_ = 0;
  inflightCount_ = 0;
  readbackSize_ = QSize(spec_.width, spec_.height);
  // The async path always reads four-byte RGBA; allocate the destination once.
  frame_ = QImage(spec_.width, spec_.height, QImage::Format_RGBA8888);
  asyncReadback_ = !frame_.isNull();
  return asyncReadback_;
}

void VideoRenderer::submitAsyncReadback() {
  if (!asyncReadback_ || freeCount_ == 0) {
    // Consumer side is more than three frames behind: drop this capture
    // instead of stalling; the next tick retries with the newest content.
    return;
  }
  const quint32 pbo = freePbos_[--freeCount_];
  gl_->glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
  gl_->glPixelStorei(GL_PACK_ALIGNMENT, 4);
  gl_->glPixelStorei(GL_PACK_ROW_LENGTH, 0);
  gl_->glPixelStorei(GL_PACK_SKIP_ROWS, 0);
  gl_->glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
  gl_->glReadPixels(0, 0, spec_.width, spec_.height, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  gl_->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  void *fence = sync_.fenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  if (!fence) {
    freePbos_[freeCount_++] = pbo;
    return;
  }
  const int slot = (inflightHead_ + inflightCount_) % 3;
  inflight_[slot] = PackRead{pbo, fence};
  inflightCount_++;
}

void VideoRenderer::setFrameTransport(ShmFrameTransport *transport) {
  frameTransport_ = transport;
}

bool VideoRenderer::consumeOldestReadback() {
  if (inflightCount_ == 0) {
    return false;
  }
  PackRead front = inflight_[inflightHead_];
  const unsigned int status = sync_.clientWaitSync(front.fence, 0, 0);
  if (status == GL_TIMEOUT_EXPIRED) {
    return false;
  }
  inflightHead_ = (inflightHead_ + 1) % 3;
  inflightCount_--;
  if (status == GL_WAIT_FAILED) {
    sync_.deleteSync(front.fence);
    freePbos_[freeCount_++] = front.pbo;
    return false;
  }
  sync_.deleteSync(front.fence);
  bool copied = false;
  const size_t bytes = static_cast<size_t>(spec_.width) * static_cast<size_t>(spec_.height) * 4;
  gl_->glBindBuffer(GL_PIXEL_PACK_BUFFER, front.pbo);
  const uchar *mapped = static_cast<const uchar *>(
      sync_.mapBufferRange(GL_PIXEL_PACK_BUFFER, 0, static_cast<qint64>(bytes),
                           GL_MAP_READ_BIT));
  const int width = spec_.width;
  const int height = spec_.height;
  const size_t glRowBytes = static_cast<size_t>(width) * 4;
  if (mapped && frameTransport_ && frameTransport_->isActive() &&
      frameTransport_->matchesGeometry(width, height)) {
    // Direct publish: the PBO copy lands straight in the transport slot,
    // flipping GL's bottom-up rows in the same pass.  The transient
    // per-frame frame_ QImage disappears from the hot path.
    copied = frameTransport_->publishWith([&](uchar *slot, qsizetype slotStride) {
      for (int row = 0; row < height; ++row) {
        std::memcpy(slot + static_cast<qsizetype>(row) * slotStride,
                    mapped + static_cast<size_t>(height - 1 - row) * glRowBytes,
                    glRowBytes);
      }
      return true;
    });
  } else if (mapped && !frame_.isNull()) {
    // GL content is bottom-up.  Copying with reversed source rows lands the
    // frame top-down in one pass, replacing the separate flip loop.
    uchar *destination = frame_.bits();
    const qsizetype rowBytes = frame_.bytesPerLine();
    for (int row = 0; row < height; ++row) {
      std::memcpy(destination + row * rowBytes,
                  mapped + static_cast<size_t>(height - 1 - row) * glRowBytes,
                  glRowBytes);
    }
    copied = true;
  }
  sync_.unmapBuffer(GL_PIXEL_PACK_BUFFER);
  gl_->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  freePbos_[freeCount_++] = front.pbo;
  return copied;
}

void VideoRenderer::releaseReadbackResources() {
  if (!gl_) {
    asyncReadback_ = false;
    return;
  }
  for (int i = 0; i < inflightCount_; ++i) {
    const int slot = (inflightHead_ + i) % 3;
    if (inflight_[slot].fence) {
      sync_.deleteSync(inflight_[slot].fence);
    }
    if (inflight_[slot].pbo) {
      gl_->glDeleteBuffers(1, &inflight_[slot].pbo);
    }
    inflight_[slot] = PackRead{};
  }
  inflightHead_ = 0;
  inflightCount_ = 0;
  for (int i = 0; i < freeCount_; ++i) {
    if (freePbos_[i]) {
      gl_->glDeleteBuffers(1, &freePbos_[i]);
    }
    freePbos_[i] = 0;
  }
  freeCount_ = 0;
  asyncReadback_ = false;
}

void VideoRenderer::renderFrame() {
  if (!running_ || paused_ || failed_ || !context_ || !surface_ || !fbo_ ||
      !renderContext_) {
    return;
  }
  // Rate ceiling with absolute-deadline pacing.  Renders are event-driven:
  // mpv's update callback queues this slot the moment a frame is decoded.
  // Each render anchors the NEXT one to now + interval (a fixed grid), so a
  // late callback is postponed to the exact grid slot instead of dropping
  // (which collapsed the rate to timer multiples) or firing early (jitter).
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  const qint64 intervalMs =
      qMax<qint64>(1, qRound(1000.0 / qBound(1, effectiveFps(), 60)));
  const qint64 sinceLast = now - lastRenderMs_;
  if (sinceLast >= 0 && sinceLast < intervalMs) {
    // Too soon: re-schedule at the exact remainder (the grid slot), never
    // drop — dropping collapses the rate; the repost keeps the cadence.
    QTimer::singleShot(static_cast<int>(intervalMs - sinceLast), this,
                       [this] { renderFrame(); });
    return;
  }
  lastRenderMs_ = now;
  // The exchange stays as the decode gate for the heavy path: without a new
  // mpv frame there is nothing to render, but the PBO ring may hold a matured
  // readback worth publishing (see the !haveFrame branch below).
  const bool newFrame = framePending_.exchange(false, std::memory_order_acq_rel);
  if (!context_->makeCurrent(surface_.get())) {
    fail(QStringLiteral("unable to make video OpenGL context current"));
    return;
  }
  pumpEvents();
  const uint64_t updateFlags = mpv_render_context_update(renderContext_);
  bool haveFrame = (updateFlags & MPV_RENDER_UPDATE_FRAME) != 0;
  if (!haveFrame || !newFrame) {
    // No new mpv frame yet.  The PBO ring may still hold an unconsumed
    // (fence-signalled) read from the previous tick; publish it now instead of
    // waiting for the next decoded frame.
    if (asyncReadback_ && consumeOldestReadback()) {
      ++frameCount_;
      emit frameReady(frame_);
    }
    context_->doneCurrent();
    return;
  }
  auto *gl = context_->functions();
  fbo_->bind();
  gl->glViewport(0, 0, spec_.width, spec_.height);
  gl->glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
  gl->glClear(GL_COLOR_BUFFER_BIT);

  mpv_opengl_fbo fbo{static_cast<int>(fbo_->handle()), spec_.width, spec_.height,
                     GL_RGBA8};
  int flipY = 1;
  mpv_render_param params[] = {
      {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
      {MPV_RENDER_PARAM_FLIP_Y, &flipY},
      {MPV_RENDER_PARAM_INVALID, nullptr},
  };
  mpv_render_context_render(renderContext_, params);

  // libmpv owns OpenGL state while it renders.  In particular it may leave a
  // different read framebuffer or PACK parameters bound; reading immediately
  // afterwards produced sparse horizontal rows at a 1920x1080 physical FBO.
  // Rebind our target and reset every packing field that affects tightly
  // packed destination rows before taking the pixels back.
  fbo_->bind();

  if (asyncReadback_) {
    // Queue this frame's read (no CPU wait; the GPU executes it in order while
    // we continue), then publish the previous frame's completed readback.
    submitAsyncReadback();
    const bool consumed = consumeOldestReadback();
    fbo_->release();
    context_->doneCurrent();
    if (!consumed) {
      return;
    }
  } else {
    gl->glPixelStorei(GL_PACK_ALIGNMENT, 1);
    gl->glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    gl->glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    gl->glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    const QSize targetSize(spec_.width, spec_.height);
    const qsizetype compactRgbStride = static_cast<qsizetype>(spec_.width) * 3;
    const bool knownReadbackFormat =
        frame_.format() == QImage::Format_RGB888 ||
        frame_.format() == QImage::Format_RGBA8888;
    if (frame_.size() != targetSize || !knownReadbackFormat) {
      frame_ = QImage(spec_.width, spec_.height, QImage::Format_RGB888);
      // QImage may pad RGB888 rows.  OpenGL can write the compact form only
      // when the Qt stride matches exactly; otherwise retain the established
      // RGBA path, whose four-byte stride is always compact for our outputs.
      if (frame_.bytesPerLine() != compactRgbStride) {
        frame_ = QImage(spec_.width, spec_.height, QImage::Format_RGBA8888);
      }
    }
    if (frame_.isNull()) {
      fbo_->release();
      context_->doneCurrent();
      fail(QStringLiteral("unable to allocate video readback image"));
      return;
    }
    const GLenum readFormat = frame_.format() == QImage::Format_RGB888 ? GL_RGB : GL_RGBA;
    gl->glReadPixels(0, 0, spec_.width, spec_.height, readFormat,
                     GL_UNSIGNED_BYTE, frame_.bits());
    fbo_->release();
    context_->doneCurrent();
    // Do not use QImage::flip() here: Qt allocates a new QImageData block on
    // every call for this shared frame path.  Swap rows through one reusable
    // scratch row instead, preserving the top-down orientation without a
    // per-frame heap allocation.
    if (!flipFrameInPlace()) {
      fail(QStringLiteral("unable to flip video readback image"));
      return;
    }
  }
  ++frameCount_;
  const qint64 fpsNow = QDateTime::currentMSecsSinceEpoch();
  if (fpsNow - fpsEpochMs_ >= 1000) {
    fps_ = static_cast<double>(frameCount_) * 1000.0 /
           static_cast<double>(fpsNow - fpsEpochMs_);
    fpsEpochMs_ = fpsNow;
    frameCount_ = 0;
  }
  emit frameReady(frame_);
}

bool VideoRenderer::flipFrameInPlace() {
  if (frame_.isNull() || frame_.height() < 2) {
    return !frame_.isNull();
  }
  const qsizetype rowBytes = frame_.bytesPerLine();
  if (rowBytes <= 0) {
    return false;
  }
  if (flipScratch_.size() != rowBytes) {
    flipScratch_.resize(rowBytes);
  }
  if (flipScratch_.size() != rowBytes) {
    return false;
  }
  uchar *top = frame_.bits();
  uchar *bottom = top + rowBytes * (frame_.height() - 1);
  const int halfHeight = frame_.height() / 2;
  for (int row = 0; row < halfHeight; ++row) {
    std::memcpy(flipScratch_.data(), top, static_cast<size_t>(rowBytes));
    std::memcpy(top, bottom, static_cast<size_t>(rowBytes));
    std::memcpy(bottom, flipScratch_.constData(), static_cast<size_t>(rowBytes));
    top += rowBytes;
    bottom -= rowBytes;
  }
  return true;
}

void VideoRenderer::pumpEvents() {
  if (!mpv_) {
    return;
  }
  // libmpv's client event queue is independent of playback.  Leaving it
  // unread makes it grow for every rendered frame in a long-lived wallpaper
  // child.  Drain it without blocking; playback and the frame API continue
  // to be driven by the existing timer.
  while (true) {
    const mpv_event *event = mpv_wait_event(mpv_, 0.0);
    if (!event || event->event_id == MPV_EVENT_NONE) {
      break;
    }
    if (event->event_id == MPV_EVENT_FILE_LOADED) {
      const QByteArray volume = QByteArray::number(spec_.volume * 100.0, 'f', 1);
      mpv_set_property_string(mpv_, "volume", volume.constData());
      mpv_set_property_string(mpv_, "mute", spec_.volume <= 0.0 ? "yes" : "no");
      if (!sourceRateConfigured_) {
        double sourceFps = 0.0;
        if (mpv_get_property(mpv_, "container-fps", MPV_FORMAT_DOUBLE,
                             &sourceFps) >= 0 &&
            std::isfinite(sourceFps) && sourceFps >= 1.0 && sourceFps <= 240.0) {
          sourceRateConfigured_ = true;
          nativeFps_ = qBound(1, qRound(sourceFps), 60);
          const int requested = qBound(1, spec_.fps, 60);
          const int effective = qMin(requested, nativeFps_);
          if (effective != requested) {
            // Lower cap than requested: nothing to do here — the pacing
            // guard in renderFrame reads effectiveFps() live, and the timer
            // is only the 200 ms keepalive.
          }
        }
      }
      // Report the decoder mpv actually engaged for this file once; a
      // hardware method that failed to initialize shows up here as "no".
      char *active = mpv_get_property_string(mpv_, "hwdec-current");
      ::fprintf(stderr, "anispaper video: hwdec-current=%s\n", active ? active : "no");
      ::fflush(stderr);
      if (active) {
        mpv_free(active);
      }
    }
  }
}

void VideoRenderer::fail(const QString &reason) {
  if (failed_) {
    return;
  }
  failed_ = true;
  frameTimer_.stop();
  emit fatal(reason);
}
