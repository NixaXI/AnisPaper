#include "web_renderer.h"

#include "static_image_renderer.h"
#include "wallpaper_properties.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QPainter>
#include <QPointer>
#include <QPixmap>
#include <QTextStream>
#include <QtGlobal>

#include <memory>
#include <QUrl>
#include <QWebChannel>
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

// Per-PID profile/console path so concurrent web children (isolated tests
// plus the live daemon child) never interleave into one log.
inline QString profileLogPath() {
  return QStringLiteral("/tmp/anispaper-web-profile-%1.log")
      .arg(QCoreApplication::applicationPid());
}

inline QString webConsoleLogPath() {
  return QStringLiteral("/tmp/anispaper-web-console-%1.log")
      .arg(QCoreApplication::applicationPid());
}

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
    // Spine/CDN wallpapers fetch JS/atlas over https.  Keep the main document
    // on the local project; allow GET/HEAD subresources only.
    if (scheme == QLatin1String("http") || scheme == QLatin1String("https")) {
      const auto type = info.resourceType();
      if (type == QWebEngineUrlRequestInfo::ResourceTypeMainFrame ||
          type == QWebEngineUrlRequestInfo::ResourceTypeSubFrame) {
        info.block(true);
        return;
      }
      const QByteArray method = info.requestMethod().toUpper();
      if (method == "GET" || method == "HEAD") return;
    }
    info.block(true);
  }

 private:
  QString rootPrefix_;
};

// QWebEnginePage that mirrors page console output to a file when
// ANISPAPER_WEB_CONSOLE=1.  Spina/boot failures otherwise die silently (the
// child process swallows stderr), leaving only a black canvas behind.
class LoggingPage final : public QWebEnginePage {
 public:
  using QWebEnginePage::QWebEnginePage;

 protected:
  void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel level,
                                const QString &message, int lineNumber,
                                const QString &sourceID) override {
    if (qEnvironmentVariableIsSet("ANISPAPER_WEB_CONSOLE")) {
      QFile log(webConsoleLogPath());
      if (log.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        QTextStream out(&log);
        out << "[web-console] level=" << static_cast<int>(level)
            << " line=" << lineNumber << " src=" << sourceID << " msg="
            << message.left(512) << "\n";
      }
    }
    QWebEnginePage::javaScriptConsoleMessage(level, message, lineNumber,
                                             sourceID);
  }
};

bool imageHasContent(const QImage &image) {
  if (image.isNull() || image.width() < 2 || image.height() < 2) return false;
  const int stepX = qMax(1, image.width() / 12);
  const int stepY = qMax(1, image.height() / 8);
  int lit = 0;
  int samples = 0;
  for (int y = 1; y < image.height(); y += stepY) {
    for (int x = 1; x < image.width(); x += stepX) {
      const QRgb pixel = image.pixel(x, y);
      // Dark Spine/night scenes are mostly near-black with a lit character in
      // the middle.  The old >25% lit test rejected them as "empty" and pinned
      // the wallpaper on its static preview.  >10% barely-lit pixels counts.
      if (qRed(pixel) + qGreen(pixel) + qBlue(pixel) > 12) ++lit;
      ++samples;
    }
  }
  return samples > 0 && lit * 10 > samples;
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
  // The hidden child runs at Wayland DPR 2 while the wallpaper is displayed
  // at CSS pixels: wallpaper libs (Spine) size their canvas by
  // window.devicePixelRatio, burning 4x raster for pixels that are
  // downscaled away.  Pin it to 1 before any page script runs.
  try{ Object.defineProperty(window, 'devicePixelRatio', {value: 1}); }catch(e){}
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
  function fixBg(){
    try{
      if(!document.body) return;
      // Never override the author's own background (e.g. Spine wallpapers
      // ship `background: url(background.png)` in style.css).  Only supply
      // the conventional fallback when the page has no background at all;
      // image/bg.png does not even exist in most projects and forcing it
      // produced a black page with a 404 behind the canvas.
      const cur = getComputedStyle(document.body).backgroundImage || '';
      if(cur && cur !== 'none') return;
      const img = new Image();
      img.onload = function(){
        const now = getComputedStyle(document.body).backgroundImage || '';
        if(now && now !== 'none') return;
        document.body.style.backgroundImage = 'url('+img.src+')';
        document.body.style.backgroundSize = 'cover';
        document.body.style.backgroundPosition = 'center';
        document.body.style.backgroundRepeat = 'no-repeat';
      };
      img.src = 'image/bg.png';
    }catch(e){}
  }
  if(document.readyState === 'loading'){
    document.addEventListener('DOMContentLoaded', fixBg);
  } else {
    fixBg();
  }
  window.wallpaperRequestAnimationFrame = window.wallpaperRequestAnimationFrame
      || function(cb){ return window.requestAnimationFrame(cb); };
  window.wallpaperCancelAnimationFrame = window.wallpaperCancelAnimationFrame
      || function(id){ return window.cancelAnimationFrame(id); };
  window.__anispaperCapture = function(src){
    try{
      if(src === 'poll'){
        try{ window.__anispaperLastPoll = performance.now(); }catch(e){}
      }
      if(!window.__anispaperBg){
        const candidates = [];
        const bg = document.body ? getComputedStyle(document.body).backgroundImage : '';
        const m = bg && bg.match(/url\(["']?([^"')]+)["']?\)/);
        if(m) candidates.push(m[1]);
        candidates.unshift('image/bg.png');
        candidates.push('background.png','bg.png');
        window.__anispaperBg = new Image();
        window.__anispaperBg.src = candidates[0];
        window.__anispaperBg.onerror = function(){
          const next = candidates.find(function(u){ return u !== window.__anispaperBg.src; });
          if(next){ window.__anispaperBg.onerror=null; window.__anispaperBg.src=next; }
        };
      }
      const canvases = document.querySelectorAll('canvas');
      let best = null, area = 0;
      for(let i = 0; i < canvases.length; ++i){
        const c = canvases[i];
        const a = (c.width || 0) * (c.height || 0);
        if(a > area){ area = a; best = c; }
      }
      // Spine wallpapers also layer a <video> under/over the WebGL canvas
      // (e.g. Nikke: <video id="video2"> + #player-container).  Capturing
      // only the canvas dropped the video layer entirely.
      let bestVideo = null, videoArea = 0;
      try{
        const videos = document.querySelectorAll('video');
        for(let i = 0; i < videos.length; ++i){
          const v = videos[i];
          const vw = v.videoWidth || v.clientWidth || 0;
          const vh = v.videoHeight || v.clientHeight || 0;
          const a = vw * vh;
          if(v.readyState >= 2 && a > videoArea){ videoArea = a; bestVideo = v; }
        }
      }catch(e){}
      // Single canvas layer and no bg image or video (the common Spine
      // case): read the canvas back directly.  drawImage() of a WebGL
      // canvas into a 2D scratch loses the frame (drawing-buffer rules),
      // while the canvas's own toDataURL stays readable — measured
      // 209 KB detailed vs 6 KB black on the same frame.
      const bgEl = window.__anispaperBg;
      const boolHasBg = !!(bgEl && bgEl.complete && bgEl.naturalWidth);
      if(best && area >= 4 && !boolHasBg && !bestVideo){
        try{ return best.toDataURL('image/jpeg', 0.88); }catch(e){}
      }
      // Multi-layer pages (bg image and/or video under the canvas): composite
      // everything through the scratch canvas.  Proven for 2D sources.
      const tw = Math.max(2, window.innerWidth || 1920);
      const th = Math.max(2, window.innerHeight || 1080);
      if(!window.__anispaperScratch){
        window.__anispaperScratch = document.createElement('canvas');
      }
      const o = window.__anispaperScratch;
      if(o.width !== tw) o.width = tw;
      if(o.height !== th) o.height = th;
      // willReadFrequently keeps the scratch in CPU RAM for cheap readback
      // instead of a GPU-backed canvas (a full GPU->CPU stall per capture
      // under SwiftShader).
      const ctx = o.getContext('2d', {alpha: false, willReadFrequently: true});
      ctx.fillStyle = '#000';
      ctx.fillRect(0, 0, tw, th);
      const bgImg = window.__anispaperBg;
      if(bgImg && bgImg.complete && bgImg.naturalWidth){
        const bs = Math.max(tw / bgImg.naturalWidth, th / bgImg.naturalHeight);
        const bdw = bgImg.naturalWidth * bs, bdh = bgImg.naturalHeight * bs;
        ctx.drawImage(bgImg, (tw - bdw) / 2, (th - bdh) / 2, bdw, bdh);
      }
      if(bestVideo && videoArea >= 4){
        try{
          const vs = Math.max(tw / (bestVideo.videoWidth || tw),
                              th / (bestVideo.videoHeight || th));
          const vdw = (bestVideo.videoWidth || tw) * vs,
                vdh = (bestVideo.videoHeight || th) * vs;
          ctx.drawImage(bestVideo, (tw - vdw) / 2, (th - vdh) / 2, vdw, vdh);
        }catch(e){}
      }
      if(best && area >= 4){
        const s = Math.max(tw / best.width, th / best.height);
        const dw = best.width * s, dh = best.height * s;
        ctx.drawImage(best, (tw - dw) / 2, (th - dh) / 2, dw, dh);
      }
      return o.toDataURL('image/jpeg', 0.88);
    }catch(e){ return ''; }
  };
})();
)JS";

const char kChannelPumpScript[] = R"JS(
(function(){
  function pump(){
    if(window.__anispaperPaused){
      requestAnimationFrame(pump);
      return;
    }
    if(!window.QWebChannel || !window.qt || !qt.webChannelTransport){
      setTimeout(pump, 50);
      return;
    }
    new QWebChannel(qt.webChannelTransport, function(ch){
      function loop(t){
        // rAF tick counter for ANISPAPER_WEB_PROFILE page-rate measurement.
        window.__anispaperRaf = (window.__anispaperRaf || 0) + 1;
        if(window.__anispaperPaused){
          requestAnimationFrame(loop);
          return;
        }
        // NOTE: no capture push here.  C++ grab-first covers every frame;
        // pushing a full composite+JPEG per rAF burned ~65 ms of renderer
        // main-thread per tick for payloads C++ discarded, starving the
        // page itself (measured page rAF collapsed to ~8 Hz).  The QWebChannel
        // bridge stays registered so ingestCapturedFrame remains available
        // as a rescue path if explicitly invoked.
        requestAnimationFrame(loop);
      }
      requestAnimationFrame(loop);
    });
  }
  if(document.readyState === 'loading'){
    document.addEventListener('DOMContentLoaded', pump);
  } else {
    pump();
  }
})();
)JS";

}  // namespace

// ANISPAPER_WEB_PROFILE=1 logs per-stage capture costs averaged every 60
// accepted frames (file log: the child swallows stderr).  Stages:
//   tick  = QTimer inter-arrival (16.7ms ideal @60fps request)
//   grab  = view_->grab() compositor snapshot
//   conv  = QPixmap->QImage + RGBA convert (CPU copies)
//   check = imageHasContent scan
//   cover = acceptFrame rescale (0 when already native size)
//   js    = runJavaScript roundtrip (fallback path only)
//   dec   = dataURL base64+JPEG decode (fallback path only)
//   emit  = frameReady emit incl. SHM publish memcpy
struct WebProfiler {
  const bool enabled = qEnvironmentVariableIsSet("ANISPAPER_WEB_PROFILE");
  double tickMs = 0.0;
  double grabMs = 0.0;
  double convMs = 0.0;
  double checkMs = 0.0;
  double coverMs = 0.0;
  double jsMs = 0.0;
  double decMs = 0.0;
  double emitMs = 0.0;
  qint64 lastTickMs = 0;
  int tickN = 0;
  int n = 0;
  int jpgN = 0;
  int grabN = 0;
  void report() {
    if (!enabled || n < 60) return;
    // NOTE: qWarning/stderr is swallowed in the QtWebEngine child process
    // (even Chromium --enable-logging never surfaces), so append to a file.
    // Per-PID path: daemon web children share the env and would otherwise
    // interleave lines into one log.
    QFile log(profileLogPath());
    if (log.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
      QTextStream out(&log);
      out << "[web-profile] tick=" << QString::number(
                   tickN ? tickMs / tickN : 0, 'f', 1)
          << " grab=" << QString::number(grabMs / n, 'f', 1)
          << " conv=" << QString::number(convMs / n, 'f', 1)
          << " check=" << QString::number(checkMs / n, 'f', 1)
          << " cover=" << QString::number(coverMs / n, 'f', 1)
          << " js=" << QString::number(jsMs / n, 'f', 1)
          << " dec=" << QString::number(decMs / n, 'f', 1)
          << " emit=" << QString::number(emitMs / n, 'f', 1) << " ms  n="
          << n << " jpg=" << jpgN << " grabN=" << grabN
          << "\n";
    }
    tickMs = grabMs = convMs = checkMs = coverMs = 0.0;
    jsMs = decMs = emitMs = 0.0;
    tickN = 0;
    n = jpgN = grabN = 0;
  }
};

WebProfiler g_webProfiler;

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
  auto *page = new LoggingPage(profile_.get(), view_.get());
  view_->setPage(page);
  auto *channel = new QWebChannel(page);
  channel->registerObject(QStringLiteral("anispaper"), this);
  page->setWebChannel(channel);
  view_->resize(spec_.width, spec_.height);
  view_->setAttribute(Qt::WA_DontShowOnScreen, true);
  page->setBackgroundColor(Qt::black);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
  QObject::connect(
      page, &QWebEnginePage::lifecycleStateChanged, page,
      [page](QWebEnginePage::LifecycleState) {
        if (page->lifecycleState() != QWebEnginePage::LifecycleState::Active) {
          page->setLifecycleState(QWebEnginePage::LifecycleState::Active);
        }
      });
  page->setLifecycleState(QWebEnginePage::LifecycleState::Active);
#endif
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
                         true);
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
    if (g_webProfiler.enabled) {
      QFile log(profileLogPath());
      if (log.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        QTextStream out(&log);
        out << "[web-load] finished ok=" << ok << " t="
            << QDateTime::currentMSecsSinceEpoch() << "\n";
      }
    }
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
  });
  view_->load(QUrl::fromLocalFile(QFileInfo(spec_.file).absoluteFilePath()));
  view_->show();
  applyMediaVolume();

  running_ = true;
  paused_ = false;
  loaded_ = false;
  fallback_ = true;
  grabWorks_ = false;
  jsInFlight_ = false;
  lastPushMs_ = 0;
  frameCount_ = 0;
  fpsEpochMs_ = QDateTime::currentMSecsSinceEpoch();
  frame_ = placeholderFrame(QStringLiteral("ANISPAPER WEB"));
  QTimer::singleShot(0, this, [this] { emit frameReady(frame_); });
  frameTimer_.start(qMax(1, 1000 / qBound(1, spec_.fps, 60)));
  if (g_webProfiler.enabled) {
    // Page-rate probe: reads the pump loop's rAF tick counter every 5 s.
    // Tells whether the page itself renders at 60 Hz or is throttled.
    auto *rafProbe = new QTimer(this);
    rafProbe->setInterval(5000);
    auto lastRaf = std::make_shared<qint64>(0);
    auto lastT = std::make_shared<qint64>(0);
    connect(rafProbe, &QTimer::timeout, this, [this, lastRaf, lastT] {
      if (!running_ || !view_ || !view_->page()) return;
      QPointer<WebRenderer> self(this);
      view_->page()->runJavaScript(QStringLiteral("window.__anispaperRaf || 0"),
                                   [self, lastRaf, lastT](const QVariant &r) {
                                     if (!self) return;
                                     const qint64 now =
                                         QDateTime::currentMSecsSinceEpoch();
                                     const qint64 raf = r.toLongLong();
                                     QFile log(profileLogPath());
                                     if (*lastT != 0 &&
                                         log.open(QIODevice::WriteOnly |
                                                  QIODevice::Append |
                                                  QIODevice::Text)) {
                                       QTextStream out(&log);
                                       const double dt =
                                           (now - *lastT) / 1000.0;
                                       out << "[web-page] rafHz="
                                           << QString::number(
                                                  (raf - *lastRaf) / dt, 'f', 1)
                                           << "\n";
                                     }
                                     *lastRaf = raf;
                                     *lastT = now;
                                   });
    });
    rafProbe->start();
  }
  return true;
}

void WebRenderer::stop() {
  frameTimer_.stop();
  running_ = false;
  paused_ = false;
  loaded_ = false;
  grabWorks_ = false;
  jsInFlight_ = false;
  lastPushMs_ = 0;
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
        QStringLiteral("window.__anispaperPaused=true;"
                       "document.querySelectorAll('audio,video').forEach(e=>e.pause())"));
  }
}

void WebRenderer::resume() {
  if (!running_ || !paused_) {
    return;
  }
  paused_ = false;
  if (view_ && view_->page()) {
    view_->page()->runJavaScript(QStringLiteral("window.__anispaperPaused=false"));
  }
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
  if (g_webProfiler.enabled) {
    const qint64 t = QDateTime::currentMSecsSinceEpoch();
    if (g_webProfiler.lastTickMs != 0) {
      g_webProfiler.tickMs += static_cast<double>(t - g_webProfiler.lastTickMs);
      ++g_webProfiler.tickN;
    }
    g_webProfiler.lastTickMs = t;
  }
  if (!loaded_) {
    emit frameReady(frame_);
    return;
  }
  // Hidden QWebEngineView::grab() at 1080p only paints a corner on Wayland.
  // Compose CSS/bg.png + Spine in-page at native innerWidth/innerHeight so
  // HDMI matches the preview tab.  The ceiling is the requested fps, not a
  // hardcoded 32 ms (~31 fps): at fps=60 the slot grid is ~16.6 ms.
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  const int minInterval = qMax(1, 1000 / qBound(1, spec_.fps, 60));
  // 2 ms early tolerance: QTimer + ms-clock jitter would otherwise lock a
  // 33 ms grid 0.1 ms early and drop every other tick (see advancePushGrid).
  if (lastPushMs_ != 0 && now - lastPushMs_ < minInterval - 2) {
    return;
  }
  // QWidget::grab() first: a single synchronous compositor readback with full
  // page fidelity (DOM + video + every canvas + CSS), lossless into SHM.
  // The async canvas readback below only fires when the grab paints garbage
  // (the old Wayland corner-only symptom), so its Chromium-side cost stays
  // off the hot path.
  if (tryGrab()) {
    return;
  }
  if (jsInFlight_) {
    return;
  }
  jsInFlight_ = true;
  jsClock_.restart();
  QPointer<WebRenderer> self(this);
  view_->page()->runJavaScript(
      QStringLiteral("window.__anispaperCapture ? window.__anispaperCapture('poll') : ''"),
      [self](const QVariant &result) {
        if (!self) return;
        self->jsInFlight_ = false;
        if (g_webProfiler.enabled) g_webProfiler.jsMs += self->jsClock_.elapsed();
        self->onJsCapture(result);
      });
}

// Advances the publication grid after a published frame.  Stays on the grid
// (no drift) when on time; snaps to now only after a real stall (>2 slots)
// so missed slots never burst.  Snapping on every publish re-phases the grid
// by the grab cost each cycle and re-locks the halving (measured 15.2 fps at
// a 30 fps cap: publish stamped tick+8ms, next 33ms tick landed inside the
// fresh window, every other tick dropped).
void WebRenderer::advancePushGrid(qint64 now) {
  const int minInterval = qMax(1, 1000 / qBound(1, spec_.fps, 60));
  if (lastPushMs_ == 0 || now - lastPushMs_ > 2 * minInterval) {
    lastPushMs_ = now;
  } else {
    lastPushMs_ += minInterval;
  }
}

// Synchronous compositor snapshot.  Returns true when a usable frame was
// published (throttle grid updated too).
bool WebRenderer::tryGrab() {
  if (!running_ || paused_ || !view_) {
    return false;
  }
  QElapsedTimer tGrab, tConv, tCheck;
  if (g_webProfiler.enabled) tGrab.start();
  const QPixmap pixmap = view_->grab();
  if (g_webProfiler.enabled) {
    g_webProfiler.grabMs += tGrab.elapsed();
    tConv.start();
  }
  if (pixmap.isNull()) {
    return false;
  }
  const QImage grabbed =
      pixmap.toImage().convertToFormat(QImage::Format_RGBA8888);
  if (g_webProfiler.enabled) {
    g_webProfiler.convMs += tConv.elapsed();
    tCheck.start();
  }
  const bool ok = imageHasContent(grabbed);
  if (g_webProfiler.enabled) g_webProfiler.checkMs += tCheck.elapsed();
  if (!ok) {
    return false;
  }
  grabWorks_ = true;
  ++g_webProfiler.grabN;
  advancePushGrid(QDateTime::currentMSecsSinceEpoch());
  acceptFrame(grabbed);
  return true;
}

void WebRenderer::ingestCapturedFrame(const QString &dataUrl) {
  // The QWebChannel rAF pump and the frameTimer race each other: without the
  // same throttle the same transport frame was published twice (observed as
  // back-to-back duplicate seqs at ~7 unique fps).  Share the slot grid.
  // Grab-first here too: the pushed dataURL is only decoded when the live
  // compositor snapshot fails.
  if (!running_ || paused_ || dataUrl.isEmpty()) {
    return;
  }
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  const int minInterval = qMax(1, 1000 / qBound(1, spec_.fps, 60));
  if (lastPushMs_ != 0 && now - lastPushMs_ < minInterval - 2) {
    return;
  }
  if (tryGrab()) {
    return;
  }
  if (jsInFlight_) {
    return;
  }
  onJsCapture(dataUrl);
}

void WebRenderer::onJsCapture(const QVariant &result) {
  if (!running_ || paused_) {
    return;
  }
  // Grab-first: full-page lossless snapshot; canvas readback below is the
  // fallback for setups where the hidden-view grab paints garbage.
  if (tryGrab()) {
    return;
  }
  // Canvas readback fallback for setups where the hidden-view grab paints
  // garbage (the old Wayland corner-only symptom).
  QElapsedTimer decClock;
  if (g_webProfiler.enabled) decClock.start();
  QImage decoded;
  if (result.typeId() == QMetaType::QString) {
    decoded = decodeDataUrl(result.toString());
    if (!decoded.isNull()) ++g_webProfiler.jpgN;
  }
  if (g_webProfiler.enabled) g_webProfiler.decMs += decClock.elapsed();
  if (imageHasContent(decoded)) {
    advancePushGrid(QDateTime::currentMSecsSinceEpoch());
    acceptFrame(decoded);
    return;
  }
  if (frame_.isNull()) {
    activateFallback(QStringLiteral("web frame unavailable"));
  }
  emit frameReady(frame_);
}

void WebRenderer::acceptFrame(const QImage &image) {
  QImage frame = image;
  QElapsedTimer coverClock;
  if (g_webProfiler.enabled) coverClock.start();
  if (frame.size() != QSize(spec_.width, spec_.height)) {
    const QImage covered = coverExact(frame, spec_.width, spec_.height);
    frame = covered.isNull() ? frame.convertToFormat(QImage::Format_RGBA8888)
                             : covered;
  } else if (frame.format() != QImage::Format_RGBA8888) {
    frame = frame.convertToFormat(QImage::Format_RGBA8888);
  }
  if (g_webProfiler.enabled) g_webProfiler.coverMs += coverClock.elapsed();
  frame_ = frame;
  fallback_ = false;
  ++frameCount_;
  ++g_webProfiler.n;
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  if (now - fpsEpochMs_ >= 1000) {
    fps_ = static_cast<double>(frameCount_) * 1000.0 /
           static_cast<double>(now - fpsEpochMs_);
    fpsEpochMs_ = now;
    frameCount_ = 0;
    g_webProfiler.report();
  }
  QElapsedTimer emitClock;
  if (g_webProfiler.enabled) emitClock.start();
  emit frameReady(frame_);
  if (g_webProfiler.enabled) g_webProfiler.emitMs += emitClock.elapsed();
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
  QFile channelFile(QStringLiteral(":/qtwebchannel/qwebchannel.js"));
  if (channelFile.open(QIODevice::ReadOnly)) {
    insert(QStringLiteral("anispaper-qwebchannel"),
           QString::fromUtf8(channelFile.readAll()));
  }
  insert(QStringLiteral("anispaper-we-pump"),
         QString::fromUtf8(kChannelPumpScript));
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
