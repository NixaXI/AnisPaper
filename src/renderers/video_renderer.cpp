#include "video_renderer.h"

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
  if (result >= 0) result = setOption("audio", "no");
  if (result >= 0) result = setOption("hwdec", "no");
  if (result >= 0) result = setOption("keep-open", "yes");
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
  }
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
  frameTimer_.start(qMax(1, 1000 / qBound(1, spec_.fps, 60)));
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

QImage VideoRenderer::lastFrame() const { return frame_; }

QString VideoRenderer::rendererName() const { return QStringLiteral("video"); }

bool VideoRenderer::isRunning() const { return running_; }

double VideoRenderer::frameRate() const { return fps_; }

void *VideoRenderer::getProcAddress(void *context, const char *name) {
  auto *glContext = static_cast<QOpenGLContext *>(context);
  const QFunctionPointer pointer = glContext->getProcAddress(name);
  return reinterpret_cast<void *>(pointer);
}

void VideoRenderer::onMpvUpdate(void *context) {
  auto *renderer = static_cast<VideoRenderer *>(context);
  if (renderer) {
    renderer->framePending_.store(true, std::memory_order_release);
  }
}

void VideoRenderer::renderFrame() {
  if (!running_ || paused_ || failed_ || !context_ || !surface_ || !fbo_ ||
      !renderContext_) {
    return;
  }
  if (!framePending_.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  if (!context_->makeCurrent(surface_.get())) {
    fail(QStringLiteral("unable to make video OpenGL context current"));
    return;
  }
  pumpEvents();
  const uint64_t updateFlags = mpv_render_context_update(renderContext_);
  if ((updateFlags & MPV_RENDER_UPDATE_FRAME) == 0) {
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
  // Rebind our target and reset every packing field that affects QImage's
  // tightly packed RGBA destination before taking the pixels back.
  fbo_->bind();
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
  ++frameCount_;
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  if (now - fpsEpochMs_ >= 1000) {
    fps_ = static_cast<double>(frameCount_) * 1000.0 /
           static_cast<double>(now - fpsEpochMs_);
    fpsEpochMs_ = now;
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
    if (event->event_id == MPV_EVENT_FILE_LOADED && !sourceRateConfigured_) {
      double sourceFps = 0.0;
      if (mpv_get_property(mpv_, "container-fps", MPV_FORMAT_DOUBLE,
                           &sourceFps) >= 0 &&
          std::isfinite(sourceFps) && sourceFps >= 1.0 && sourceFps <= 240.0) {
        sourceRateConfigured_ = true;
        const int requested = qBound(1, spec_.fps, 60);
        const int native = qBound(1, qRound(sourceFps), 60);
        const int effective = qMin(requested, native);
        if (effective != requested) {
          frameTimer_.start(qMax(1, qRound(1000.0 / effective)));
        }
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
