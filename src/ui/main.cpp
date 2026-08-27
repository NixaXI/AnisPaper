#include "rpc_client.h"

#include <QApplication>
#include <QBuffer>
#include <QByteArray>
#include <QCache>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QImage>
#include <QImageReader>
#include <QMimeDatabase>
#include <QMutex>
#include <QMutexLocker>
#include <QSize>
#include <QUrl>
#include <QUrlQuery>
#include <QWebChannel>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestJob>
#include <QWebEngineUrlScheme>
#include <QWebEngineUrlSchemeHandler>
#include <QWebEngineView>

namespace {

class PreviewSchemeHandler : public QWebEngineUrlSchemeHandler {
 public:
  explicit PreviewSchemeHandler(QObject *parent = nullptr)
      : QWebEngineUrlSchemeHandler(parent) {}

  void requestStarted(QWebEngineUrlRequestJob *job) override {
    if (job->requestUrl().host() != QLatin1String("preview")) {
      job->fail(QWebEngineUrlRequestJob::UrlInvalid);
      return;
    }
    const QUrlQuery query(job->requestUrl());
    QString path = job->requestUrl().path();
    if (!QFileInfo::exists(path)) {
      path = query.queryItemValue(QStringLiteral("p"), QUrl::FullyDecoded);
    }
    const QFileInfo info(path);
    const auto suffix = info.suffix().toLower();
    const auto mimeName = QMimeDatabase().mimeTypeForFile(info).name();
    const bool image = mimeName.startsWith(QLatin1String("image/")) ||
                       suffix == QLatin1String("jpg") || suffix == QLatin1String("jpeg") ||
                       suffix == QLatin1String("png") || suffix == QLatin1String("webp") ||
                       suffix == QLatin1String("gif") || suffix == QLatin1String("bmp");
    if (!info.isFile() || !image) {
      job->fail(QWebEngineUrlRequestJob::UrlNotFound);
      return;
    }
    bool ok = false;
    const int requested = query.queryItemValue(QStringLiteral("w")).toInt(&ok);
    const int maxEdge = qBound(160, ok && requested > 0 ? requested : 256, 1280);
    const int quality = maxEdge >= 900 ? 82 : 72;
    const QString cacheKey = info.canonicalFilePath() + QLatin1Char('@') + QString::number(maxEdge);
    static QMutex cacheMutex;
    static QCache<QString, QByteArray> previewCache(24 * 1024 * 1024);
    QByteArray jpeg;
    {
      QMutexLocker lock(&cacheMutex);
      if (const QByteArray *hit = previewCache.object(cacheKey)) jpeg = *hit;
    }
    if (jpeg.isEmpty()) {
      QImageReader reader(info.canonicalFilePath());
      reader.setAutoTransform(true);
      QSize size = reader.size();
      if (size.isValid() && (size.width() > maxEdge || size.height() > maxEdge)) {
        size.scale(maxEdge, maxEdge, Qt::KeepAspectRatio);
        reader.setScaledSize(size);
      }
      QImage frame = reader.read();
      if (frame.isNull()) {
        job->fail(QWebEngineUrlRequestJob::RequestFailed);
        return;
      }
      if (frame.width() > maxEdge || frame.height() > maxEdge) {
        frame = frame.scaled(maxEdge, maxEdge, Qt::KeepAspectRatio, Qt::FastTransformation);
      }
      QBuffer encode;
      encode.open(QIODevice::WriteOnly);
      if (!frame.save(&encode, "JPEG", quality)) {
        job->fail(QWebEngineUrlRequestJob::RequestFailed);
        return;
      }
      jpeg = encode.data();
      auto *stored = new QByteArray(jpeg);
      QMutexLocker lock(&cacheMutex);
      previewCache.insert(cacheKey, stored, stored->size());
    }
    auto *buffer = new QBuffer(job);
    buffer->setData(jpeg);
    buffer->open(QIODevice::ReadOnly);
    job->reply(QByteArrayLiteral("image/jpeg"), buffer);
  }
};

void registerPreviewScheme() {
  QWebEngineUrlScheme scheme(QByteArrayLiteral("anispaper"));
  scheme.setSyntax(QWebEngineUrlScheme::Syntax::Host);
  // Do not set LocalScheme: Chromium then treats anispaper:// like file:// and
  // blocks qrc:/ui/index.html from loading thumbs ("Not allowed to load local resource").
  scheme.setFlags(QWebEngineUrlScheme::SecureScheme | QWebEngineUrlScheme::CorsEnabled |
                  QWebEngineUrlScheme::ContentSecurityPolicyIgnored |
                  QWebEngineUrlScheme::FetchApiAllowed);
  QWebEngineUrlScheme::registerScheme(scheme);
}

}  // namespace

int main(int argc, char *argv[]) {
  qunsetenv("QTWEBENGINE_REMOTE_DEBUGGING");
  qunsetenv("QTWEBENGINE_REMOTE_DEBUGGING_PORT");
  qputenv("QTWEBENGINE_CHROMIUM_FLAGS",
          QByteArrayLiteral("--disable-features=Translate,BackForwardCache,MediaRouter,"
                            "AutofillServerCommunication --disable-background-networking "
                            "--disable-sync --disable-component-update "
                            "--renderer-process-limit=1 --disable-extensions "
                            "--js-flags=--max-old-space-size=192 --disk-cache-size=16777216 "
                            "--disable-breakpad --disable-hang-monitor "
                            "--num-raster-threads=1 --disable-gpu-shader-disk-cache "
                            "--force-gpu-mem-available-mb=128"));
  registerPreviewScheme();

  QApplication app(argc, argv);
  app.setApplicationName(QStringLiteral("AnisPaper"));
  app.setOrganizationName(QStringLiteral("AnisPaper"));
  app.setDesktopFileName(QStringLiteral("org.anispaper.ui"));
  app.setWindowIcon(QIcon(QStringLiteral(":/anis-star.png")));

  auto *profile = QWebEngineProfile::defaultProfile();
  profile->installUrlSchemeHandler("anispaper", new PreviewSchemeHandler(profile));
  profile->setHttpCacheMaximumSize(16 * 1024 * 1024);
  profile->setPersistentCookiesPolicy(QWebEngineProfile::NoPersistentCookies);
  profile->setSpellCheckEnabled(false);

  RpcClient client;

  auto *view = new QWebEngineView;
  view->setWindowTitle(QStringLiteral("ANISPAPER · STARLIGHT"));
  view->resize(1320, 860);
  view->setMinimumSize(1080, 720);
  const QIcon appIcon(QStringLiteral(":/anis-star.png"));
  view->setWindowIcon(appIcon);

  auto *settings = view->settings();
  settings->setAttribute(QWebEngineSettings::JavascriptEnabled, true);
  settings->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, true);
  settings->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, true);
  settings->setAttribute(QWebEngineSettings::AllowRunningInsecureContent, true);
  settings->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, false);
  settings->setAttribute(QWebEngineSettings::ScrollAnimatorEnabled, false);
  settings->setAttribute(QWebEngineSettings::AutoLoadIconsForPage, false);
  settings->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, false);
  settings->setAttribute(QWebEngineSettings::PluginsEnabled, false);
  settings->setAttribute(QWebEngineSettings::WebGLEnabled, false);
  settings->setAttribute(QWebEngineSettings::Accelerated2dCanvasEnabled, false);
  settings->setAttribute(QWebEngineSettings::ErrorPageEnabled, false);

  auto *channel = new QWebChannel(view);
  channel->registerObject(QStringLiteral("client"), &client);
  view->page()->setWebChannel(channel);
  QObject::connect(view, &QWebEngineView::iconChanged, view, [view, appIcon](const QIcon &) {
    view->setWindowIcon(appIcon);
  });
  view->setContextMenuPolicy(Qt::NoContextMenu);
  view->load(QUrl(QStringLiteral("qrc:/ui/index.html")));
  view->setAttribute(Qt::WA_DeleteOnClose);
  view->show();
  return app.exec();
}
