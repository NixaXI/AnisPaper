#include "frame_image_provider.h"

#include <QQmlEngine>
#include <QQmlExtensionPlugin>
#include <qqml.h>

class AnisPaperFramePlugin final : public QQmlExtensionPlugin {
  Q_OBJECT
  Q_PLUGIN_METADATA(IID QQmlExtensionInterface_iid)
  Q_INTERFACES(QQmlExtensionInterface)

 public:
  void registerTypes(const char *uri) override {
    Q_ASSERT(QLatin1String(uri) == QLatin1String("org.anispaper.frame"));
    // This module intentionally exports only a QQuickImageProvider. Register
    // the URI explicitly so Qt still considers the plugin-only module valid.
    qmlRegisterModule(uri, 1, 0);
  }

  void initializeEngine(QQmlEngine *engine, const char *uri) override {
    QQmlExtensionPlugin::initializeEngine(engine, uri);
    if (!engine->imageProvider(QStringLiteral("anispaper"))) {
      engine->addImageProvider(QStringLiteral("anispaper"),
                               new FrameImageProvider());
    }
  }
};

#include "anispaper_frame_plugin.moc"
