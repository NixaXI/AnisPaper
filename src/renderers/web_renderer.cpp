#include "web_renderer.h"

#include "static_image_renderer.h"

#include <QDateTime>
#include <QFileInfo>
#include <QWebEnginePage>
#include <QUrl>
#include <QWebEngineSettings>
#include <QWebEngineView>

WebRenderer::WebRenderer(RendererSpec spec, QObject *parent)
    : Renderer(std::move(spec), parent) {
  frameTimer_.setTimerType(Qt::PreciseTimer);
  connect(&frameTimer_, &QTimer::timeout, this, &WebRenderer::captureFrame);
}

WebRenderer::~WebRenderer() { stop(); }

bool WebRenderer::start(QString *error) {
  if (running_) {
    return true;
  }
  if (!QFileInfo(spec_.file).isFile()) {
    if (error) {
      *error = QStringLiteral("web source is unavailable");
    }
    return false;
  }

  view_ = std::make_unique<QWebEngineView>();
  view_->resize(spec_.width, spec_.height);
  view_->setAttribute(Qt::WA_DontShowOnScreen, true);
  auto *settings = view_->settings();
  settings->setAttribute(QWebEngineSettings::JavascriptEnabled, true);
  settings->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, false);
  settings->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls,
                         true);
  settings->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls,
                         true);
  connect(view_.get(), &QWebEngineView::loadFinished, this,
          [this](bool ok) {
            loaded_ = ok;
            if (!ok) {
              activateFallback(QStringLiteral("web load failed"));
              return;
            }
            if (view_ && view_->page()) {
              view_->page()->setAudioMuted(spec_.volume <= 0.0);
              view_->page()->runJavaScript(
                  QStringLiteral("document.querySelectorAll('audio,video').forEach(e=>{e.volume=%1;e.play().catch(()=>{})})")
                      .arg(QString::number(spec_.volume, 'f', 3)));
            }
          });
  view_->load(QUrl::fromLocalFile(QFileInfo(spec_.file).absoluteFilePath()));
  view_->show();
  view_->page()->setAudioMuted(spec_.volume <= 0.0);

  running_ = true;
  paused_ = false;
  loaded_ = false;
  fallback_ = false;
  frameCount_ = 0;
  fpsEpochMs_ = QDateTime::currentMSecsSinceEpoch();
  // An immediate deterministic frame prevents a blank preview while Chromium
  // initializes, and is also the explicit fallback for a cross-origin iframe
  // failure that does not take down the local document.
  frame_ = StaticImageRenderer::fallbackFrame(QStringLiteral("ANISPAPER WEB"),
                                               spec_.width, spec_.height);
  // Defer so the parent protocol can publish its "ready" message before the
  // first frame (emitting synchronously here would reverse their order).
  QTimer::singleShot(0, this, [this] { emit frameReady(frame_); });
  frameTimer_.start(qMax(1, 1000 / qBound(1, spec_.fps, 60)));
  return true;
}

void WebRenderer::stop() {
  frameTimer_.stop();
  running_ = false;
  paused_ = false;
  if (view_) {
    view_->close();
    view_.reset();
  }
}

void WebRenderer::pause() {
  if (!running_ || paused_) {
    return;
  }
  paused_ = true;
  frameTimer_.stop();
  if (view_ && view_->page()) {
    view_->page()->runJavaScript(
        QStringLiteral("document.querySelectorAll('audio,video').forEach(e=>e.pause())"));
  }
}

void WebRenderer::resume() {
  if (!running_ || !paused_) {
    return;
  }
  paused_ = false;
  if (view_ && view_->page() && spec_.volume > 0.0) {
    view_->page()->setAudioMuted(false);
    view_->page()->runJavaScript(
        QStringLiteral("document.querySelectorAll('audio,video').forEach(e=>{e.volume=%1;e.play().catch(()=>{})})")
            .arg(QString::number(spec_.volume, 'f', 3)));
  }
  frameTimer_.start(qMax(1, 1000 / qBound(1, spec_.fps, 60)));
}

QImage WebRenderer::lastFrame() const { return frame_; }

QString WebRenderer::rendererName() const { return QStringLiteral("web"); }

bool WebRenderer::isRunning() const { return running_; }

bool WebRenderer::isFallback() const { return fallback_; }

double WebRenderer::frameRate() const { return fps_; }

void WebRenderer::applyPlayback(int fps, double volume) {
  Renderer::applyPlayback(fps, volume);
  if (!view_ || !view_->page()) {
    return;
  }
  view_->page()->setAudioMuted(spec_.volume <= 0.0);
  if (spec_.volume > 0.0) {
    view_->page()->runJavaScript(
        QStringLiteral("document.querySelectorAll('audio,video').forEach(e=>{e.volume=%1;e.muted=false})")
            .arg(QString::number(spec_.volume, 'f', 3)));
  }
  if (running_ && !paused_) {
    frameTimer_.start(qMax(1, 1000 / qBound(1, spec_.fps, 60)));
  }
}

void WebRenderer::captureFrame() {
  if (!running_ || paused_ || !view_) {
    return;
  }
  if (loaded_) {
    const QPixmap pixmap = view_->grab();
    if (!pixmap.isNull()) {
      frame_ = pixmap.toImage().convertToFormat(QImage::Format_RGBA8888);
      fallback_ = false;
    }
  }
  if (frame_.isNull()) {
    activateFallback(QStringLiteral("web frame unavailable"));
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

void WebRenderer::activateFallback(const QString &reason) {
  fallback_ = true;
  frame_ = StaticImageRenderer::fallbackFrame(
      QStringLiteral("ANISPAPER WEB FALLBACK\n%1").arg(reason), spec_.width,
      spec_.height);
}
