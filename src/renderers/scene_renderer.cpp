#include "scene_renderer.h"

#include "static_image_renderer.h"

SceneRenderer::SceneRenderer(RendererSpec spec, QObject *parent)
    : Renderer(std::move(spec), parent) {}

SceneRenderer::~SceneRenderer() { stop(); }

bool SceneRenderer::start(QString *error) {
  // Keep the renderer contract identical to native implementations: it emits
  // a first frame exactly once and needs no child process or GL context.
  fallback_ = std::make_unique<StaticImageRenderer>(spec_);
  if (!fallback_->start(error)) {
    return false;
  }
  frame_ = fallback_->lastFrame();
  if (frame_.isNull()) {
    if (error) *error = QStringLiteral("renderer unavailable");
    fallback_.reset();
    return false;
  }
  running_ = true;
  paused_ = false;
  emit ready();
  emit frameReady(frame_);
  return true;
}

void SceneRenderer::stop() {
  if (fallback_) fallback_->stop();
  fallback_.reset();
  running_ = false;
  paused_ = false;
}

void SceneRenderer::pause() {
  if (fallback_) fallback_->pause();
  paused_ = true;
}

void SceneRenderer::resume() {
  if (fallback_) fallback_->resume();
  paused_ = false;
}

QImage SceneRenderer::lastFrame() const { return frame_; }

QString SceneRenderer::rendererName() const { return QStringLiteral("scene-static"); }

bool SceneRenderer::isRunning() const { return running_; }

bool SceneRenderer::isFallback() const { return true; }

bool SceneRenderer::nativeSupported() {
  // Deliberately explicit until a supported libwallpaperengine ABI is present
  // and wrapped by a real offscreen implementation.
  return false;
}

QString SceneRenderer::unsupportedBadge() {
  return QStringLiteral("scene sin soporte nativo");
}

