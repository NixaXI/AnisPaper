#include "web_renderer.h"

#include "static_image_renderer.h"
#include "wallpaper_properties.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFramebufferObject>
#include <QPainter>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickGraphicsDevice>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QTextStream>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>
#include <QtGlobal>
#include <QtWebEngineQuick/QQuickWebEngineProfile>
#include <QtWebEngineQuick/qtwebenginequickglobal.h>

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

// Hash of every other full row, a word at a time (~1 ms at 1080p).  Sparse
// grids missed small sprites such as snow particles and froze the publish.
quint64 frameHash(const QImage &image) {
  quint64 h = 1469598103934665603ULL;
  const int words = image.width() * 4 / 8;
  for (int y = 0; y < image.height(); y += 2) {
    const auto *row = reinterpret_cast<const quint64 *>(image.constScanLine(y));
    for (int x = 0; x < words; ++x) {
      h = (h ^ row[x]) * 1099511628211ULL;
    }
  }
  return h;
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

// Runs before any page script.  The page is composited at CSS pixels, so pin
// devicePixelRatio to 1 (Spine sizes its canvas by it and would otherwise
// burn 4x raster on a DPR-2 session), and provide the Wallpaper Engine rAF
// shims plus a pause flag the renderer toggles.
const char kBootstrapScript[] = R"JS(
(function(){
  try{ Object.defineProperty(window, 'devicePixelRatio', {value: 1}); }catch(e){}
  window.__anispaperPaused = false;
  window.wallpaperRequestAnimationFrame = window.wallpaperRequestAnimationFrame
      || function(cb){ return window.requestAnimationFrame(cb); };
  window.wallpaperCancelAnimationFrame = window.wallpaperCancelAnimationFrame
      || function(id){ return window.cancelAnimationFrame(id); };
})();
)JS";

// The view and its sandbox policy.  Built as a string so the permission
// handler matches the Qt version this child was compiled against.
QString sceneQml() {
  QString qml = QStringLiteral(R"QML(
import QtQuick
import QtWebEngine

Item {
  id: root
  property url pageUrl
  property var scripts: []
  property QtObject host
  property WebEngineProfile profile
  property bool muted: false
  signal pageLoaded(bool ok)

  function run(code) { web.runJavaScript(code); }

  // Called once the item is sized and inside the window.  Pages read
  // innerWidth/innerHeight while parsing (Miku snow sizes its canvas once),
  // so loading from Component.onCompleted produced a 0x0 viewport.
  function load() {
    for (const s of root.scripts) {
      const script = WebEngine.script();
      script.name = s.name;
      script.sourceCode = s.source;
      script.injectionPoint = WebEngineScript.DocumentCreation;
      script.worldId = WebEngineScript.MainWorld;
      script.runsOnSubFrames = false;
      web.userScripts.insert(script);
    }
    // Chromium learns the view size asynchronously.  Park on about:blank
    // until it reports the real viewport, then load the wallpaper (scripts
    // were inserted first: DocumentCreation only applies to later loads).
    web.url = "about:blank";
  }

  property bool sized: false
  Timer {
    id: sizePoll
    interval: 20
    repeat: true
    onTriggered: web.runJavaScript("innerWidth * innerHeight", function(area) {
      if (root.sized || !(area > 0)) return;
      root.sized = true;
      sizePoll.stop();
      web.url = root.pageUrl;
    })
  }

  Connections {
    target: root.profile
    function onDownloadRequested(download) { download.cancel(); }
  }

  WebEngineView {
    id: web
    anchors.fill: parent
    profile: root.profile
    backgroundColor: "black"
    audioMuted: root.muted
    settings.javascriptEnabled: true
    settings.playbackRequiresUserGesture: false
    settings.localContentCanAccessRemoteUrls: true
    settings.localContentCanAccessFileUrls: true
    settings.allowRunningInsecureContent: false
    settings.javascriptCanOpenWindows: false
    settings.pluginsEnabled: false
    settings.webGLEnabled: true
    settings.accelerated2dCanvasEnabled: true
    settings.errorPageEnabled: false
    settings.autoLoadIconsForPage: false
    settings.scrollAnimatorEnabled: false
    settings.showScrollBars: false
    onLoadingChanged: function(info) {
      if (!root.sized) {
        if (info.status === WebEngineView.LoadSucceededStatus) sizePoll.start();
        return;
      }
      if (info.status === WebEngineView.LoadSucceededStatus) root.pageLoaded(true);
      else if (info.status === WebEngineView.LoadFailedStatus) root.pageLoaded(false);
    }
    onJavaScriptConsoleMessage: function(level, message, line, source) {
      if (root.host) root.host.logConsole(level, message, line, source);
    }
    onNewWindowRequested: function(request) {}
    %1
  }
}
)QML");
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
  return qml.arg(QStringLiteral(
      "onPermissionRequested: function(permission) { permission.deny(); }"));
#else
  return qml.arg(QStringLiteral(
      "onFeaturePermissionRequested: function(origin, feature) {"
      " grantFeaturePermission(origin, feature, false); }"));
#endif
}

}  // namespace

// ANISPAPER_WEB_PROFILE=1 logs per-stage costs averaged every 60 published
// frames (file log: the child swallows stderr).  Stages:
//   render = polish+sync+render of the offscreen Quick scene
//   read   = glReadPixels of the composited frame into the publish image
//   emit   = frameReady emit incl. SHM publish memcpy
//   skip   = ticks with no new Chromium frame (nothing rendered)
struct WebProfiler {
  const bool enabled = qEnvironmentVariableIsSet("ANISPAPER_WEB_PROFILE");
  double renderMs = 0.0;
  double readMs = 0.0;
  double emitMs = 0.0;
  int n = 0;
  int skipped = 0;
  void report() {
    if (!enabled || n + skipped < 60) return;
    const int rendered = qMax(1, n + skipped);
    QFile log(profileLogPath());
    if (log.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
      QTextStream out(&log);
      out << "[web-profile] render=" << QString::number(renderMs / rendered, 'f', 2)
          << " read=" << QString::number(readMs / rendered, 'f', 2)
          << " emit=" << QString::number(emitMs / qMax(1, n), 'f', 2) << " ms  n=" << n
          << " skip=" << skipped << "\n";
    }
    renderMs = readMs = emitMs = 0.0;
    n = skipped = 0;
  }
};

WebProfiler g_webProfiler;

void WebRenderer::prepareProcess() {
  // QtWebEngineQuick shares GL contexts with Qt Quick; both settings must be
  // in place before the QApplication exists.
  QtWebEngineQuick::initialize();
  QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
}

WebRenderer::WebRenderer(RendererSpec spec, QObject *parent)
    : Renderer(std::move(spec), parent) {
  frameTimer_.setTimerType(Qt::PreciseTimer);
  connect(&frameTimer_, &QTimer::timeout, this, &WebRenderer::renderFrame);
}

WebRenderer::~WebRenderer() { stop(); }

int WebRenderer::frameIntervalMs() const {
  return qMax(1, 1000 / qBound(1, spec_.fps, 60));
}

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

  running_ = true;
  paused_ = false;
  loaded_ = false;
  fallback_ = true;
  lastHash_ = 0;
  frameCount_ = 0;
  fpsEpochMs_ = QDateTime::currentMSecsSinceEpoch();
  frame_ = placeholderFrame(QStringLiteral("ANISPAPER WEB"));
  QTimer::singleShot(0, this, [this] { emit frameReady(frame_); });

  QString reason;
  if (!createScene(&reason)) {
    // No GL in this session: keep publishing the preview instead of dying,
    // the same contract the old capture path had.
    destroyScene();
    activateFallback(reason);
    return true;
  }
  frameTimer_.start(frameIntervalMs());
  return true;
}

bool WebRenderer::createScene(QString *reason) {
  const QSize size(spec_.width, spec_.height);

  context_ = std::make_unique<QOpenGLContext>();
  QSurfaceFormat format = QSurfaceFormat::defaultFormat();
  format.setDepthBufferSize(24);
  format.setStencilBufferSize(8);
  context_->setFormat(format);
  context_->setShareContext(QOpenGLContext::globalShareContext());
  if (!context_->create()) {
    *reason = QStringLiteral("web GL context unavailable");
    return false;
  }
  surface_ = std::make_unique<QOffscreenSurface>();
  surface_->setFormat(context_->format());
  surface_->create();
  if (!context_->makeCurrent(surface_.get())) {
    *reason = QStringLiteral("web GL context cannot be made current");
    return false;
  }

  renderControl_ = std::make_unique<QQuickRenderControl>();
  window_ = std::make_unique<QQuickWindow>(renderControl_.get());
  window_->setGraphicsDevice(
      QQuickGraphicsDevice::fromOpenGLContext(context_.get()));
  window_->resize(size);
  window_->setColor(Qt::black);
  if (!renderControl_->initialize()) {
    *reason = QStringLiteral("web render control unavailable");
    return false;
  }
  fbo_ = std::make_unique<QOpenGLFramebufferObject>(
      size, QOpenGLFramebufferObject::CombinedDepthStencil);
  QQuickRenderTarget target =
      QQuickRenderTarget::fromOpenGLTexture(fbo_->texture(), size);
  // Render upside down so glReadPixels yields top-down rows with no CPU flip.
  target.setMirrorVertically(true);
  window_->setRenderTarget(target);
  profile_ = std::make_unique<QQuickWebEngineProfile>();
  profile_->setOffTheRecord(true);
  profile_->setPersistentCookiesPolicy(QQuickWebEngineProfile::NoPersistentCookies);
  profile_->setHttpCacheType(QQuickWebEngineProfile::NoCache);
  profile_->setSpellCheckEnabled(false);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
  profile_->setPushServiceEnabled(false);
#endif
  profile_->setUrlRequestInterceptor(
      new SandboxInterceptor(canonicalDirPrefix(spec_.file), profile_.get()));

  QVariantList scripts;
  auto addScript = [&scripts](const QString &name, const QString &source) {
    if (source.isEmpty()) return;
    scripts.append(QVariantMap{{QStringLiteral("name"), name},
                               {QStringLiteral("source"), source}});
  };
  addScript(QStringLiteral("anispaper-we-bootstrap"),
            QString::fromUtf8(kBootstrapScript));
  addScript(QStringLiteral("anispaper-we-properties"),
            WallpaperProperties::applyUserPropertiesScript(spec_.properties));

  engine_ = std::make_unique<QQmlEngine>();
  QQmlComponent component(engine_.get());
  component.setData(sceneQml().toUtf8(), QUrl());
  root_ = component.createWithInitialProperties(
      {{QStringLiteral("pageUrl"),
        QUrl::fromLocalFile(QFileInfo(spec_.file).absoluteFilePath())},
       {QStringLiteral("scripts"), scripts},
       {QStringLiteral("host"), QVariant::fromValue<QObject *>(this)},
       {QStringLiteral("profile"),
        QVariant::fromValue<QObject *>(profile_.get())},
       {QStringLiteral("muted"), spec_.volume <= 0.0},
       {QStringLiteral("width"), size.width()},
       {QStringLiteral("height"), size.height()}});
  auto *item = qobject_cast<QQuickItem *>(root_);
  if (!item) {
    *reason = QStringLiteral("web scene failed: %1").arg(component.errorString());
    return false;
  }
  item->setParentItem(window_->contentItem());
  item->setSize(size);
  connect(root_, SIGNAL(pageLoaded(bool)), this, SLOT(onPageLoaded(bool)));
  QMetaObject::invokeMethod(root_, "load");
  return true;
}

void WebRenderer::destroyScene() {
  if (context_ && surface_) context_->makeCurrent(surface_.get());
  delete root_;
  root_ = nullptr;
  engine_.reset();
  window_.reset();
  renderControl_.reset();
  fbo_.reset();
  if (context_) context_->doneCurrent();
  // The page holds a reference to its profile; drop the profile last.
  profile_.reset();
  context_.reset();
  surface_.reset();
}

void WebRenderer::onPageLoaded(bool ok) {
  if (g_webProfiler.enabled) {
    QFile log(profileLogPath());
    if (log.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
      QTextStream out(&log);
      out << "[web-load] finished ok=" << ok << " t="
          << QDateTime::currentMSecsSinceEpoch() << "\n";
    }
  }
  if (!ok) {
    loaded_ = false;
    activateFallback(QStringLiteral("web load failed"));
    emit frameReady(frame_);
    return;
  }
  loaded_ = true;
  loadClock_.start();
  applyMediaVolume();
  // Late delivery after `load`: pages such as Miku snow build their scene in
  // <body onload> and throw if applyUserProperties lands before it, so the
  // DocumentCreation deliveries alone can all miss on a slow start.
  runJavaScript(WallpaperProperties::applyUserPropertiesScript(spec_.properties));
  if (paused_) {
    runJavaScript(QStringLiteral(
        "window.__anispaperPaused=true;"
        "document.querySelectorAll('audio,video').forEach(e=>e.pause())"));
  }
}

void WebRenderer::stop() {
  frameTimer_.stop();
  running_ = false;
  paused_ = false;
  loaded_ = false;
  destroyScene();
}

void WebRenderer::pause() {
  if (!running_ || paused_) {
    return;
  }
  paused_ = true;
  frameTimer_.stop();
  runJavaScript(QStringLiteral(
      "window.__anispaperPaused=true;"
      "document.querySelectorAll('audio,video').forEach(e=>e.pause())"));
}

void WebRenderer::resume() {
  if (!running_ || !paused_) {
    return;
  }
  paused_ = false;
  runJavaScript(QStringLiteral("window.__anispaperPaused=false"));
  applyMediaVolume();
  if (window_) frameTimer_.start(frameIntervalMs());
}

QImage WebRenderer::lastFrame() const { return frame_; }

QString WebRenderer::rendererName() const { return QStringLiteral("web"); }

bool WebRenderer::isRunning() const { return running_; }

bool WebRenderer::isFallback() const { return fallback_; }

double WebRenderer::frameRate() const { return fps_; }

void WebRenderer::applyPlayback(int fps, double volume) {
  Renderer::applyPlayback(fps, volume);
  applyMediaVolume();
  if (running_ && !paused_ && window_) {
    frameTimer_.start(frameIntervalMs());
  }
}

void WebRenderer::logConsole(int level, const QString &message, int line,
                             const QString &source) {
  // Spine/boot failures otherwise die silently (the child swallows stderr),
  // leaving only a black page behind.
  if (!qEnvironmentVariableIsSet("ANISPAPER_WEB_CONSOLE")) return;
  QFile log(webConsoleLogPath());
  if (log.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
    QTextStream out(&log);
    out << "[web-console] level=" << level << " line=" << line
        << " src=" << source << " msg=" << message.left(512) << "\n";
  }
}

void WebRenderer::renderFrame() {
  if (!running_ || paused_ || !window_) {
    return;
  }
  if (!context_->makeCurrent(surface_.get())) {
    return;
  }
  // Drive the scene every tick, loaded or not.  Qt WebEngine paces Chromium's
  // begin-frames and propagates the view size off the Quick frames we render:
  // without them pages parse with a 0x0 viewport and Spine loaders stall.
  // It also swaps the page texture without reliably emitting
  // sceneChanged/renderRequested, so neither signal can gate the work.
  QElapsedTimer stage;
  if (g_webProfiler.enabled) stage.start();
  renderControl_->polishItems();
  renderControl_->beginFrame();
  renderControl_->sync();
  renderControl_->render();
  renderControl_->endFrame();
  if (g_webProfiler.enabled) {
    g_webProfiler.renderMs += stage.nsecsElapsed() / 1e6;
    stage.restart();
  }
  if (!loaded_) {
    emit frameReady(frame_);
    return;
  }

  QImage image(spec_.width, spec_.height, QImage::Format_RGBA8888);
  fbo_->bind();
  context_->extraFunctions()->glReadPixels(0, 0, spec_.width, spec_.height,
                                           GL_RGBA, GL_UNSIGNED_BYTE,
                                           image.bits());
  fbo_->release();
  if (g_webProfiler.enabled) g_webProfiler.readMs += stage.nsecsElapsed() / 1e6;

  // Until the page paints something, keep the preview up: Spine projects can
  // spend seconds loading 8K atlases on a black page.  Give up waiting after
  // a while so genuinely dark wallpapers still show.
  if (fallback_ && !imageHasContent(image) && loadClock_.elapsed() < 15000) {
    return;
  }
  // Static pages (or a paused canvas) keep producing identical frames;
  // publishing them again only costs SHM copies and a Plasma texture upload.
  const quint64 hash = frameHash(image);
  if (!fallback_ && hash == lastHash_) {
    ++g_webProfiler.skipped;
    g_webProfiler.report();
    return;
  }
  lastHash_ = hash;
  acceptFrame(image);
}

void WebRenderer::acceptFrame(const QImage &image) {
  frame_ = image;
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
  if (g_webProfiler.enabled) g_webProfiler.emitMs += emitClock.nsecsElapsed() / 1e6;
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

void WebRenderer::runJavaScript(const QString &code) {
  if (!root_ || code.isEmpty()) return;
  QMetaObject::invokeMethod(root_, "run", Q_ARG(QVariant, code));
}

void WebRenderer::applyMediaVolume() {
  if (!root_) return;
  root_->setProperty("muted", spec_.volume <= 0.0);
  if (spec_.volume > 0.0) {
    runJavaScript(
        QStringLiteral("document.querySelectorAll('audio,video').forEach(e=>{"
                       "e.volume=%1;e.muted=false;e.play().catch(()=>{})})")
            .arg(QString::number(spec_.volume, 'f', 3)));
  }
}
