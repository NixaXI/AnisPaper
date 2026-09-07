#include "web_renderer.h"

#include "static_image_renderer.h"
#include "wallpaper_properties.h"

#include <QDateTime>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QPainter>
#include <QPointer>
#include <QPixmap>
#include <QtGlobal>
#include <QUrl>
#include <QWebEngineDownloadRequest>
#if QT_VERSION >= QT_VERSION_CHECK(6, 2, 0)
#include <QWebEngineNewWindowRequest>
#endif
#include <QWebEnginePage>
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
#include <QWebEnginePermission>
#endif
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineView>

namespace {

QString canonicalDirPrefix(const QString &filePath) {
  QString root = QFileInfo(filePath).absolutePath();
  const QString canonical = QFileInfo(root).canonicalFilePath();
  if (!canonical.isEmpty()) root = canonical;
  if (!root.endsWith(QLatin1Char('/'))) root += QLatin1Char('/');
  return root;
}

bool allowedLocalFile(const QUrl &url, const QString &rootPrefix) {
  if (!url.isLocalFile()) return false;
  QString path = QFileInfo(url.toLocalFile()).canonicalFilePath();
  if (path.isEmpty()) path = QFileInfo(url.toLocalFile()).absoluteFilePath();
  if (path.isEmpty() || rootPrefix.isEmpty()) return false;
  return path.startsWith(rootPrefix);
}

class SandboxInterceptor final : public QWebEngineUrlRequestInterceptor {
 public:
  explicit SandboxInterceptor(QString rootPrefix, QObject *parent = nullptr)
      : QWebEngineUrlRequestInterceptor(parent), rootPrefix_(std::move(rootPrefix)) {}

  void interceptRequest(QWebEngineUrlRequestInfo &info) override {
    const QUrl url = info.requestUrl();
    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1String("data") || scheme == QLatin1String("blob") ||
        scheme == QLatin1String("qrc") || scheme == QLatin1String("about")) {
      return;
    }
    if (scheme == QLatin1String("file") && allowedLocalFile(url, rootPrefix_)) {
      return;
    }
    info.block(true);
  }

 private:
  QString rootPrefix_;
};

bool imageHasContent(const QImage &image) {
  if (image.isNull() || image.width() < 2 || image.height() < 2) return false;
  const int stepX = qMax(1, image.width() / 12);
  const int stepY = qMax(1, image.height() / 8);
  int energy = 0;
  int samples = 0;
  for (int y = 1; y < image.height(); y += stepY) {
    for (int x = 1; x < image.width(); x += stepX) {
      const QRgb pixel = image.pixel(x, y);
      energy += qRed(pixel) + qGreen(pixel) + qBlue(pixel);
      ++samples;
    }
  }
  return samples > 0 && energy > samples * 6;
}

QImage coverExact(const QImage &source, int width, int height) {
  if (source.isNull() || width < 2 || height < 2) return {};
  QImage result(width, height, QImage::Format_RGBA8888);
  result.fill(Qt::black);
  const QImage scaled = source.scaled(QSize(width, height),
                                      Qt::KeepAspectRatioByExpanding,
                                      Qt::SmoothTransformation);
  QPainter painter(&result);
  painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
  painter.drawImage((width - scaled.width()) / 2,
                    (height - scaled.height()) / 2, scaled);
  return result;
}

QImage decodeDataUrl(const QString &dataUrl) {
  const int comma = dataUrl.indexOf(QLatin1Char(','));
  if (comma < 0 || !dataUrl.startsWith(QLatin1String("data:image/"))) return {};
  const QByteArray raw =
      QByteArray::fromBase64(dataUrl.mid(comma + 1).toLatin1());
  if (raw.isEmpty()) return {};
  QImage image;
  if (!image.loadFromData(raw)) return {};
  return image.convertToFormat(QImage::Format_RGBA8888);
}

const char kBootstrapScript[] = R"JS(
(function(){
  function wrap(proto){
    if(!proto || proto.__anispaperWrapped) return;
    const orig = proto.getContext;
    if(typeof orig !== 'function') return;
    proto.getContext = function(type, attrs){
      const t = String(type || '').toLowerCase();
      if(t.indexOf('webgl') >= 0){
        attrs = Object.assign({}, attrs || {}, {preserveDrawingBuffer: true});
      }
      return orig.call(this, type, attrs);
    };
    proto.__anispaperWrapped = true;
  }
  wrap(window.HTMLCanvasElement && HTMLCanvasElement.prototype);
  try{ wrap(window.OffscreenCanvas && OffscreenCanvas.prototype); }catch(e){}
  window.wallpaperRequestAnimationFrame = window.wallpaperRequestAnimationFrame
      || function(cb){ return window.requestAnimationFrame(cb); };
  window.wallpaperCancelAnimationFrame = window.wallpaperCancelAnimationFrame
      || function(id){ return window.cancelAnimationFrame(id); };
  window.__anispaperCapture = function(){
    try{
      const canvases = document.querySelectorAll('canvas');
      let best = null, area = 0;
      for(let i = 0; i < canvases.length; ++i){
        const c = canvases[i];
        const a = (c.width || 0) * (c.height || 0);
        if(a > area){ area = a; best = c; }
      }
      if(!best || area < 4) return '';
      return best.toDataURL('image/jpeg', 0.82);
    }catch(e){ return ''; }
  };
})();
)JS";

}  // namespace

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

  profile_ = std::make_unique<QWebEngineProfile>();
  profile_->setPersistentCookiesPolicy(QWebEngineProfile::NoPersistentCookies);
  profile_->setHttpCacheType(QWebEngineProfile::NoCache);
  profile_->setSpellCheckEnabled(false);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
  profile_->setPushServiceEnabled(false);
#endif
  auto *interceptor =
      new SandboxInterceptor(canonicalDirPrefix(spec_.file), profile_.get());
  profile_->setUrlRequestInterceptor(interceptor);
  QObject::connect(profile_.get(), &QWebEngineProfile::downloadRequested, this,
                   [](QWebEngineDownloadRequest *download) {
                     if (download) download->cancel();
                   });

  view_ = std::make_unique<QWebEngineView>();
  auto *page = new QWebEnginePage(profile_.get(), view_.get());
  view_->setPage(page);
  view_->resize(spec_.width, spec_.height);
  view_->setAttribute(Qt::WA_DontShowOnScreen, true);
  page->setBackgroundColor(Qt::black);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
  QObject::connect(page, &QWebEnginePage::permissionRequested, this,
                   [](QWebEnginePermission permission) { permission.deny(); });
#else
  QObject::connect(
      page, &QWebEnginePage::featurePermissionRequested, this,
      [page](const QUrl &securityOrigin, QWebEnginePage::Feature feature) {
        page->setFeaturePermission(securityOrigin, feature,
                                   QWebEnginePage::PermissionDeniedByUser);
      });
#endif
#if QT_VERSION >= QT_VERSION_CHECK(6, 2, 0)
  QObject::connect(page, &QWebEnginePage::newWindowRequested, this,
                   [](QWebEngineNewWindowRequest &) {});
#endif

  auto *settings = view_->settings();
  settings->setAttribute(QWebEngineSettings::JavascriptEnabled, true);
  settings->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, false);
  settings->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls,
                         false);
  settings->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls,
                         true);
  settings->setAttribute(QWebEngineSettings::AllowRunningInsecureContent, false);
  settings->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, false);
  settings->setAttribute(QWebEngineSettings::PluginsEnabled, false);
  settings->setAttribute(QWebEngineSettings::WebGLEnabled, true);
  settings->setAttribute(QWebEngineSettings::Accelerated2dCanvasEnabled, true);
  settings->setAttribute(QWebEngineSettings::ErrorPageEnabled, false);
  settings->setAttribute(QWebEngineSettings::AutoLoadIconsForPage, false);
  settings->setAttribute(QWebEngineSettings::ScrollAnimatorEnabled, false);

  injectScripts();

  connect(view_.get(), &QWebEngineView::loadFinished, this, [this](bool ok) {
    loaded_ = ok;
    if (!ok) {
      activateFallback(QStringLiteral("web load failed"));
      return;
    }
    applyMediaVolume();
    const QString late =
        WallpaperProperties::applyUserPropertiesScript(spec_.properties);
    if (view_ && view_->page() && !late.isEmpty()) {
      view_->page()->runJavaScript(late);
    }
    if (view_ && view_->page()) {
      view_->page()->runJavaScript(
          QStringLiteral(
              "(function(w,h){document.querySelectorAll('canvas').forEach("
              "function(c){if(c.width<2||c.height<2){c.width=w;c.height=h;}});})(%1,%2)")
              .arg(spec_.width)
              .arg(spec_.height));
    }
  });
  view_->load(QUrl::fromLocalFile(QFileInfo(spec_.file).absoluteFilePath()));
  view_->show();
  applyMediaVolume();

  running_ = true;
  paused_ = false;
  loaded_ = false;
  fallback_ = true;
  captureInFlight_ = false;
  frameCount_ = 0;
  fpsEpochMs_ = QDateTime::currentMSecsSinceEpoch();
  frame_ = placeholderFrame(QStringLiteral("ANISPAPER WEB"));
  QTimer::singleShot(0, this, [this] { emit frameReady(frame_); });
  frameTimer_.start(qMax(1, 1000 / qBound(1, spec_.fps, 60)));
  return true;
}

void WebRenderer::stop() {
  frameTimer_.stop();
  running_ = false;
  paused_ = false;
  loaded_ = false;
  captureInFlight_ = false;
  if (view_) {
    view_->close();
    view_.reset();
  }
  profile_.reset();
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
  applyMediaVolume();
  frameTimer_.start(qMax(1, 1000 / qBound(1, spec_.fps, 60)));
}

QImage WebRenderer::lastFrame() const { return frame_; }

QString WebRenderer::rendererName() const { return QStringLiteral("web"); }

bool WebRenderer::isRunning() const { return running_; }

bool WebRenderer::isFallback() const { return fallback_; }

double WebRenderer::frameRate() const { return fps_; }

void WebRenderer::applyPlayback(int fps, double volume) {
  Renderer::applyPlayback(fps, volume);
  applyMediaVolume();
  if (running_ && !paused_) {
    frameTimer_.start(qMax(1, 1000 / qBound(1, spec_.fps, 60)));
  }
}

void WebRenderer::captureFrame() {
  if (!running_ || paused_ || !view_ || !view_->page()) {
    return;
  }
  if (!loaded_) {
    emit frameReady(frame_);
    return;
  }
  if (captureInFlight_) {
    return;
  }
  captureInFlight_ = true;
  QPointer<WebRenderer> self(this);
  view_->page()->runJavaScript(
      QStringLiteral("window.__anispaperCapture ? window.__anispaperCapture() : ''"),
      [self](const QVariant &result) {
        if (self) self->onJsCapture(result);
      });
}

void WebRenderer::onJsCapture(const QVariant &result) {
  captureInFlight_ = false;
  if (!running_ || paused_) {
    return;
  }
  const QImage decoded = decodeDataUrl(result.toString());
  if (imageHasContent(decoded)) {
    acceptFrame(decoded);
    return;
  }
  if (view_) {
    const QPixmap pixmap = view_->grab();
    if (!pixmap.isNull()) {
      const QImage grabbed =
          pixmap.toImage().convertToFormat(QImage::Format_RGBA8888);
      if (imageHasContent(grabbed)) {
        acceptFrame(grabbed);
        return;
      }
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

void WebRenderer::acceptFrame(const QImage &image) {
  QImage frame = image;
  if (frame.size() != QSize(spec_.width, spec_.height)) {
    frame = frame.scaled(spec_.width, spec_.height, Qt::IgnoreAspectRatio,
                         Qt::SmoothTransformation)
                .convertToFormat(QImage::Format_RGBA8888);
  } else if (frame.format() != QImage::Format_RGBA8888) {
    frame = frame.convertToFormat(QImage::Format_RGBA8888);
  }
  frame_ = frame;
  fallback_ = false;
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
  frame_ = placeholderFrame(reason);
}

QImage WebRenderer::placeholderFrame(const QString &reason) const {
  if (!spec_.preview.isEmpty() && QFileInfo(spec_.preview).isFile()) {
    QImageReader reader(spec_.preview);
    reader.setAutoTransform(true);
    const QImage preview = reader.read();
    if (!preview.isNull()) {
      const QImage covered = coverExact(preview, spec_.width, spec_.height);
      if (!covered.isNull()) return covered;
    }
  }
  return StaticImageRenderer::fallbackFrame(
      QStringLiteral("ANISPAPER WEB FALLBACK\n%1").arg(reason), spec_.width,
      spec_.height);
}

void WebRenderer::injectScripts() {
  if (!view_ || !view_->page()) return;
  auto insert = [this](const QString &name, const QString &source) {
    if (source.isEmpty()) return;
    QWebEngineScript script;
    script.setName(name);
    script.setInjectionPoint(QWebEngineScript::DocumentCreation);
    script.setWorldId(QWebEngineScript::MainWorld);
    script.setRunsOnSubFrames(false);
    script.setSourceCode(source);
    view_->page()->scripts().insert(script);
  };
  insert(QStringLiteral("anispaper-we-bootstrap"),
         QString::fromUtf8(kBootstrapScript));
  insert(QStringLiteral("anispaper-we-properties"),
         WallpaperProperties::applyUserPropertiesScript(spec_.properties));
}

void WebRenderer::applyMediaVolume() {
  if (!view_ || !view_->page()) return;
  view_->page()->setAudioMuted(spec_.volume <= 0.0);
  if (spec_.volume > 0.0) {
    view_->page()->runJavaScript(
        QStringLiteral("document.querySelectorAll('audio,video').forEach(e=>{"
                       "e.volume=%1;e.muted=false;e.play().catch(()=>{})})")
            .arg(QString::number(spec_.volume, 'f', 3)));
  }
}
