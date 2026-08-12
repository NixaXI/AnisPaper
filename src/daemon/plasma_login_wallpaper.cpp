#include "plasma_login_wallpaper.h"

#include <QFile>
#include <QFileInfo>
#include <QVariant>

#include <KAuth/Action>
#include <KAuth/ExecuteJob>
#include <qdbusunixfiledescriptor.h>

bool installPlasmaLoginWallpaper(const QString &pngPath, QString *error)
{
  const QFileInfo info(pngPath);
  if (!info.isFile() || !info.isReadable()) {
    if (error) *error = QStringLiteral("Plasma Login wallpaper capture is not readable");
    return false;
  }

  QFile image(pngPath);
  if (!image.open(QIODevice::ReadOnly)) {
    if (error) *error = QStringLiteral("Plasma Login wallpaper capture could not be opened: %1")
        .arg(image.errorString());
    return false;
  }

  // This is the exact config shape used by Plasma Login's KCM:
  // Greeter/Wallpaper/<plugin>/General/Image.  The helper copies the file to
  // the plasmalogin service user's wallpaper directory and the configured URI
  // points there, so the greeter can read it before any user logs in.
  QString config;
  QFile installedConfig(QStringLiteral("/etc/plasmalogin.conf"));
  if (installedConfig.exists() && installedConfig.size() > 1024 * 1024) {
    if (error) *error = QStringLiteral("Plasma Login configuration exceeds the 1 MiB KAuth limit");
    return false;
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
              QVariant::fromValue(QDBusUnixFileDescriptor(image.handle())));

  KAuth::Action action(QStringLiteral("org.kde.kcontrol.kcmplasmalogin.save"));
  action.setHelperId(QStringLiteral("org.kde.kcontrol.kcmplasmalogin"));
  action.setArguments(args);
  std::unique_ptr<KAuth::ExecuteJob> job(action.execute());
  if (!job || !job->exec()) {
    if (error) {
      const QString detail = job ? job->errorString() : QStringLiteral("KAuth job unavailable");
      *error = QStringLiteral("Plasma Login KAuth save failed: %1").arg(detail);
    }
    return false;
  }
  return true;
}
