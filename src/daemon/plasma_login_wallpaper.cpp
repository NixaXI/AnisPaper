#include "plasma_login_wallpaper.h"

#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QTimer>
#include <QThread>
#include <QVariant>

#include <KAuth/Action>
#include <KAuth/ExecuteJob>
#include <qdbusunixfiledescriptor.h>

void installPlasmaLoginWallpaperAsync(const QString &pngPath, QObject *context,
                                       PlasmaLoginWallpaperCallback callback)
{
  auto finish = [context, callback = std::move(callback)](bool ok,
                                                           const QString &message) mutable {
    if (!context) return;
    if (QThread::currentThread() == context->thread()) {
      callback(ok, message);
      return;
    }
    QMetaObject::invokeMethod(
        context,
        [callback = std::move(callback), ok, message]() mutable {
          callback(ok, message);
        },
        Qt::QueuedConnection);
  };
  const QFileInfo info(pngPath);
  if (!info.isFile() || !info.isReadable()) {
    finish(false, QStringLiteral("Plasma Login wallpaper capture is not readable"));
    return;
  }

  auto *image = new QFile(pngPath);
  if (!image->open(QIODevice::ReadOnly)) {
    const QString message = QStringLiteral("Plasma Login wallpaper capture could not be opened: %1")
                                .arg(image->errorString());
    delete image;
    finish(false, message);
    return;
  }

  // This is the exact config shape used by Plasma Login's KCM:
  // Greeter/Wallpaper/<plugin>/General/Image.  The helper copies the file to
  // the plasmalogin service user's wallpaper directory and the configured URI
  // points there, so the greeter can read it before any user logs in.
  QString config;
  QFile installedConfig(QStringLiteral("/etc/plasmalogin.conf"));
  if (installedConfig.exists() && installedConfig.size() > 1024 * 1024) {
    delete image;
    finish(false, QStringLiteral("Plasma Login configuration exceeds the 1 MiB KAuth limit"));
    return;
  }
  if (installedConfig.open(QIODevice::ReadOnly | QIODevice::Text) &&
      installedConfig.size() <= 1024 * 1024) {
    config = QString::fromUtf8(installedConfig.readAll());
    if (!config.isEmpty() && !config.endsWith(QLatin1Char('\n'))) {
      config += QLatin1Char('\n');
    }
  }
  config += QStringLiteral(
      "[Greeter]\n"
      "WallpaperPluginId=org.kde.image\n"
      "[Greeter][Wallpaper][org.kde.image][General]\n"
      "Image=file:///var/lib/plasmalogin/wallpapers/anispaper.png\n");

  QVariantMap args;
  args.insert(QStringLiteral("config"), config);
  args.insert(QStringLiteral("wallpapers"), QStringList{QStringLiteral("anispaper.png")});
  args.insert(QStringLiteral("_fd_anispaper.png"),
              QVariant::fromValue(QDBusUnixFileDescriptor(image->handle())));

  KAuth::Action action(QStringLiteral("org.kde.kcontrol.kcmplasmalogin.save"));
  action.setHelperId(QStringLiteral("org.kde.kcontrol.kcmplasmalogin"));
  action.setArguments(args);
  KAuth::ExecuteJob *job = action.execute();
  if (!job) {
    delete image;
    finish(false, QStringLiteral("Plasma Login KAuth save failed: KAuth job unavailable"));
    return;
  }
  image->setParent(job);
  auto *timeout = new QTimer(job);
  timeout->setSingleShot(true);
  timeout->setInterval(30'000);
  QObject::connect(timeout, &QTimer::timeout, job, [job] {
    job->kill(KJob::EmitResult);
  });
  QObject::connect(job, &KAuth::ExecuteJob::result, context,
                   [job, finish = std::move(finish)](KJob *) mutable {
                     const bool ok = job->error() == 0;
                     const QString detail = job->errorString();
                     finish(ok, ok ? QString() :
                                      QStringLiteral("Plasma Login KAuth save failed: %1")
                                          .arg(detail.isEmpty() ? QStringLiteral("authorization failed")
                                                                : detail));
                     job->deleteLater();
                   });
  timeout->start();
  job->start();
}
