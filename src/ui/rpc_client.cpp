#include "rpc_client.h"

#include <csignal>
#include <algorithm>
#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPixmap>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QUrl>
#include <QUrlQuery>
#include <QVariantMap>
#include <QVector>
#include <QVBoxLayout>
#include <QtGlobal>

namespace {
QString socketPath() {
  const auto runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
  if (runtime.isEmpty()) return {};
  return runtime + QStringLiteral("/anispaper.sock");
}

int parseSteamWorkshopPages(const QString &html, int currentPage, int idCount) {
  const int page = qMax(1, currentPage);
  QRegularExpression ofRe(QStringLiteral(R"(of\s+([\d,]+)\s+entries)"),
                          QRegularExpression::CaseInsensitiveOption);
  const auto ofMatch = ofRe.match(html);
  if (ofMatch.hasMatch()) {
    QString digits = ofMatch.captured(1);
    digits.remove(QLatin1Char(','));
    bool ok = false;
    const qint64 total = digits.toLongLong(&ok);
    if (ok && total > 0) return qMax(1, int((total + 29) / 30));
  }
  int maxLink = page;
  QRegularExpression pagelinkRe(QStringLiteral(R"(class=['"]pagelink['"][^>]*>\s*(\d+))"),
                                QRegularExpression::CaseInsensitiveOption);
  auto it = pagelinkRe.globalMatch(html);
  while (it.hasNext()) maxLink = qMax(maxLink, it.next().captured(1).toInt());
  if (idCount >= 30) return qMax(maxLink, page + 1);
  return qMax(maxLink, page);
}

void fillMonitors(const QJsonValue &result, QVariantList *monitors, QStringList *names) {
  monitors->clear();
  names->clear();
  if (!result.isArray()) return;
  for (const auto &value : result.toArray()) {
    const auto object = value.toObject();
    monitors->append(object.toVariantMap());
    names->append(object.value("name").toString());
  }
}

QString previewUrl(const QString &path) {
  if (path.isEmpty()) return {};
  if (path.startsWith(QLatin1String("http://")) ||
      path.startsWith(QLatin1String("https://")) ||
      path.startsWith(QLatin1String("anispaper:"))) {
    return path;
  }
  QString local = path;
  if (local.startsWith(QLatin1String("file:"))) local = QUrl(local).toLocalFile();
  const QFileInfo info(local);
  if (!info.exists()) return {};
  QUrl url;
  url.setScheme(QStringLiteral("anispaper"));
  url.setHost(QStringLiteral("preview"));
  url.setPath(info.canonicalFilePath());
  QUrlQuery query;
  query.addQueryItem(QStringLiteral("p"), info.canonicalFilePath());
  url.setQuery(query);
  return QString::fromLatin1(url.toEncoded());
}

QString steamcmdHome() {
  return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
         QStringLiteral("/anispaper/steamcmd");
}

QString steamcmdBinary() { return steamcmdHome() + QStringLiteral("/steamcmd.sh"); }

QString steamcmdScriptPath() { return steamcmdHome() + QStringLiteral("/install.script"); }

QString steamUserStore() { return steamcmdHome() + QStringLiteral("/username"); }

QString pendingWorkshopPath() {
  return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
         QStringLiteral("/anispaper/pending-workshop-id");
}

void savePendingWorkshop(const QString &id) {
  if (id.isEmpty()) return;
  QDir().mkpath(QFileInfo(pendingWorkshopPath()).absolutePath());
  QFile file(pendingWorkshopPath());
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
  file.write(id.toUtf8());
}

void clearPendingWorkshop() { QFile::remove(pendingWorkshopPath()); }

QString loadPendingWorkshop() {
  QFile file(pendingWorkshopPath());
  if (!file.open(QIODevice::ReadOnly)) return {};
  return QString::fromUtf8(file.readAll()).trimmed();
}

QString depotDownloaderBinary() {
  return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
         QStringLiteral("/anispaper/depotdownloader/DepotDownloader");
}

QString loadSavedSteamUser() {
  QFile file(steamUserStore());
  if (!file.open(QIODevice::ReadOnly)) return {};
  return QString::fromUtf8(file.readAll()).trimmed();
}

void saveSteamUser(const QString &user) {
  QDir().mkpath(steamcmdHome());
  QFile file(steamUserStore());
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
  file.write(user.toUtf8());
}

QString loginusersText() {
  const QStringList paths{
      QDir::homePath() + QStringLiteral("/.local/share/Steam/config/loginusers.vdf"),
      QDir::homePath() + QStringLiteral("/.steam/steam/config/loginusers.vdf"),
  };
  for (const auto &path : paths) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) continue;
    return QString::fromUtf8(file.readAll());
  }
  return {};
}

QString accountNameForSteamId(const QString &steamId, const QString &loginusers) {
  if (steamId.isEmpty() || loginusers.isEmpty()) return {};
  QRegularExpression block(
      QStringLiteral("\"%1\"\\s*\\{([^}]*)\\}").arg(QRegularExpression::escape(steamId)));
  const auto match = block.match(loginusers);
  if (!match.hasMatch()) return {};
  QRegularExpression nameRe(QStringLiteral("\"AccountName\"\\s+\"([^\"]+)\""));
  const auto name = nameRe.match(match.captured(1));
  return name.hasMatch() ? name.captured(1) : QString();
}

QStringList wallpaperEngineManifests() {
  QStringList acfs;
  auto add = [&acfs](const QString &path) {
    const QString clean = QDir::cleanPath(path);
    if (QFileInfo::exists(clean) && QFileInfo(clean).isFile() && !acfs.contains(clean))
      acfs << clean;
  };
  const QStringList vdfs{
      QDir::homePath() + QStringLiteral("/.local/share/Steam/steamapps/libraryfolders.vdf"),
      QDir::homePath() + QStringLiteral("/.local/share/Steam/config/libraryfolders.vdf"),
  };
  QRegularExpression rx(QStringLiteral("\\\"path\\\"\\s*\\\"((?:\\\\.|[^\\\"])*)\\\""));
  for (const auto &vdf : vdfs) {
    QFile file(vdf);
    if (!file.open(QIODevice::ReadOnly)) continue;
    const QString text = QString::fromUtf8(file.readAll());
    auto it = rx.globalMatch(text);
    while (it.hasNext()) {
      QString path = it.next().captured(1);
      path.replace(QStringLiteral("\\\\"), QStringLiteral("\\"));
      add(path + QStringLiteral("/steamapps/appmanifest_431960.acf"));
    }
  }
  add(QDir::homePath() + QStringLiteral("/.local/share/Steam/steamapps/appmanifest_431960.acf"));
  add(QDir::homePath() + QStringLiteral("/.steam/steam/steamapps/appmanifest_431960.acf"));
  const QFileInfo workshop(QDir::homePath() +
                           QStringLiteral("/.local/share/Steam/steamapps/workshop/content/431960"));
  QDir dir(workshop.canonicalFilePath());
  if (!dir.path().isEmpty() && dir.cdUp() && dir.cdUp() && dir.cdUp())
    add(dir.filePath(QStringLiteral("appmanifest_431960.acf")));
  return acfs;
}

QString wallpaperEngineOwnerSteamId() {
  QRegularExpression ownerRe(QStringLiteral("\"LastOwner\"\\s+\"(\\d+)\""));
  for (const auto &acf : wallpaperEngineManifests()) {
    QFile file(acf);
    if (!file.open(QIODevice::ReadOnly)) continue;
    const auto match = ownerRe.match(QString::fromUtf8(file.readAll()));
    if (match.hasMatch()) return match.captured(1);
  }
  return {};
}

QString recentSteamAccount() {
  const QString saved = loadSavedSteamUser();
  if (!saved.isEmpty()) return saved;
  const QString loginusers = loginusersText();
  const QString owner = accountNameForSteamId(wallpaperEngineOwnerSteamId(), loginusers);
  if (!owner.isEmpty()) return owner;
  QRegularExpression block(
      QStringLiteral("\"AccountName\"\\s+\"([^\"]+)\"[\\s\\S]*?\"AutoLogin\"\\s+\"(\\d)\""));
  QString fallback;
  auto it = block.globalMatch(loginusers);
  while (it.hasNext()) {
    const auto m = it.next();
    if (fallback.isEmpty()) fallback = m.captured(1);
    if (m.captured(2) == QLatin1String("1")) return m.captured(1);
  }
  if (!fallback.isEmpty()) return fallback;
  QRegularExpression nameRe(QStringLiteral("\"AccountName\"\\s+\"([^\"]+)\""));
  const auto first = nameRe.match(loginusers);
  return first.hasMatch() ? first.captured(1) : QString();
}

void wipeSteamcmdScript() {
  QFile file(steamcmdScriptPath());
  if (!file.exists()) return;
  if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    file.write(QByteArray(4096, '\0'));
    file.close();
  }
  file.remove();
}

QString redactSteamOutput(QString output) {
  static const QRegularExpression loginLine(
      QStringLiteral("(?m)^(.*\\blogin\\b.*)$"), QRegularExpression::CaseInsensitiveOption);
  return output.replace(loginLine, QStringLiteral("login <redacted>"));
}

QString workshopInstallRoot() {
  return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
         QStringLiteral("/anispaper/workshop");
}

QString workshopItemDir(const QString &id) {
  return workshopInstallRoot() + QStringLiteral("/steamapps/workshop/content/431960/") + id;
}

QString qrLogPath() { return steamcmdHome() + QStringLiteral("/last-qr.log"); }

QString qrStopPath() { return steamcmdHome() + QStringLiteral("/qr.stop"); }

QString qrLinkPath() { return steamcmdHome() + QStringLiteral("/steam-qr.url"); }

QString qrPngPath() { return steamcmdHome() + QStringLiteral("/steam-qr.png"); }

QString qrPidPath() { return steamcmdHome() + QStringLiteral("/qr.pid"); }

QString qrHelperPath() {
  return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
         QStringLiteral("/anispaper/depotdownloader/workshop-qr.py");
}

bool qrHelperRunning() {
  QFile file(qrPidPath());
  if (!file.open(QIODevice::ReadOnly)) return false;
  const qint64 pid = QString::fromUtf8(file.readAll()).trimmed().toLongLong();
  if (pid <= 0) return false;
  return QFileInfo::exists(QStringLiteral("/proc/%1").arg(pid));
}

QString ugcHelperPath() {
  return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
         QStringLiteral("/anispaper/steam-ugc/workshop-download.py");
}

QString ensureUgcHelper() {
  const QString dest = ugcHelperPath();
  QDir().mkpath(QFileInfo(dest).absolutePath());
  const QStringList srcs{
#ifdef ANISPAPER_SOURCE_DIR
      QStringLiteral(ANISPAPER_SOURCE_DIR "/scripts/anispaper-workshop-download.py"),
#endif
      QCoreApplication::applicationDirPath() + QStringLiteral("/anispaper-workshop-download.py"),
      QDir::currentPath() + QStringLiteral("/scripts/anispaper-workshop-download.py"),
  };
  for (const auto &src : srcs) {
    if (!QFileInfo::exists(src)) continue;
    if (QFileInfo(dest).exists() && QFileInfo(src).lastModified() <= QFileInfo(dest).lastModified())
      return dest;
    QFile::remove(dest);
    if (QFile::copy(src, dest)) {
      QFile::setPermissions(dest, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                      QFileDevice::ExeOwner | QFileDevice::ReadUser |
                                      QFileDevice::ExeUser);
      return dest;
    }
  }
  return QFileInfo::exists(dest) ? dest : QString();
}

QString ensureQrHelper() {
  const QString dest = qrHelperPath();
  QDir().mkpath(QFileInfo(dest).absolutePath());
  if (QFileInfo::exists(dest)) return dest;
  const QStringList srcs{
      QCoreApplication::applicationDirPath() + QStringLiteral("/anispaper-workshop-qr.py"),
      QDir::currentPath() + QStringLiteral("/scripts/anispaper-workshop-qr.py"),
  };
  for (const auto &src : srcs) {
    if (!QFileInfo::exists(src)) continue;
    QFile::remove(dest);
    if (QFile::copy(src, dest)) {
      QFile::setPermissions(dest, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                      QFileDevice::ExeOwner | QFileDevice::ReadUser |
                                      QFileDevice::ExeUser);
      return dest;
    }
  }
  return dest;
}

QProcessEnvironment steamcmdEnvironment() {
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  env.remove(QStringLiteral("LD_LIBRARY_PATH"));
  env.remove(QStringLiteral("LD_PRELOAD"));
  env.remove(QStringLiteral("QT_PLUGIN_PATH"));
  env.remove(QStringLiteral("QT_QPA_PLATFORM_PLUGIN_PATH"));
  env.remove(QStringLiteral("QTWEBENGINEPROCESS_PATH"));
  env.remove(QStringLiteral("QML2_IMPORT_PATH"));
  env.remove(QStringLiteral("QML_IMPORT_PATH"));
  env.remove(QStringLiteral("STEAM_COMPAT_CLIENT_INSTALL_PATH"));
  env.remove(QStringLiteral("SteamAppId"));
  env.remove(QStringLiteral("SteamGameId"));
  env.remove(QStringLiteral("SteamOverlayGameId"));
  const QString home = steamcmdHome();
  QDir().mkpath(home + QStringLiteral("/run"));
  QDir().mkpath(home + QStringLiteral("/xdg-data"));
  QDir().mkpath(home + QStringLiteral("/xdg-config"));
  env.insert(QStringLiteral("HOME"), home);
  env.insert(QStringLiteral("XDG_RUNTIME_DIR"), home + QStringLiteral("/run"));
  env.insert(QStringLiteral("XDG_DATA_HOME"), home + QStringLiteral("/xdg-data"));
  env.insert(QStringLiteral("XDG_CONFIG_HOME"), home + QStringLiteral("/xdg-config"));
  return env;
}

QSet<QString> parseWorkshopRatings(const QString &csv) {
  static const QSet<QString> known{QStringLiteral("everyone"), QStringLiteral("questionable"),
                                   QStringLiteral("mature"), QStringLiteral("adult")};
  QSet<QString> out;
  for (const auto &part : csv.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
    const QString key = part.trimmed().toLower();
    if (known.contains(key)) out.insert(key);
  }
  return out;
}

int ratingRank(const QString &rating) {
  if (rating == QLatin1String("adult")) return 3;
  if (rating == QLatin1String("mature")) return 2;
  if (rating == QLatin1String("questionable")) return 1;
  return 0;
}

QString ratingFromTagList(const QJsonArray &tags) {
  QString rating = QStringLiteral("everyone");
  int rank = 0;
  for (const auto &tagValue : tags) {
    const QString tag = (tagValue.isObject()
                             ? tagValue.toObject().value(QStringLiteral("tag")).toString()
                             : tagValue.toString())
                            .trimmed()
                            .toLower();
    const int next = ratingRank(tag);
    if (next > rank) {
      rank = next;
      rating = tag;
    }
  }
  return rating;
}

void addSteamRoot(QString steamRoot, QStringList *roots) {
  steamRoot = QDir::cleanPath(steamRoot);
  if (steamRoot.isEmpty()) return;
  const QString content = steamRoot + QStringLiteral("/steamapps/workshop/content/431960");
  const QFileInfo info(content);
  if (!info.exists() || !info.isDir()) return;
  const QString canonical = info.canonicalFilePath();
  if (!canonical.isEmpty() && !roots->contains(canonical)) roots->append(canonical);
}

QStringList steamWorkshopRoots() {
  QStringList roots;
  addSteamRoot(QDir::homePath() + QStringLiteral("/.local/share/Steam"), &roots);
  addSteamRoot(QDir::homePath() + QStringLiteral("/.steam/steam"), &roots);
  addSteamRoot(workshopInstallRoot(), &roots);
  const QStringList vdfs{
      QDir::homePath() + QStringLiteral("/.local/share/Steam/steamapps/libraryfolders.vdf"),
      QDir::homePath() + QStringLiteral("/.local/share/Steam/config/libraryfolders.vdf"),
  };
  QRegularExpression rx(QStringLiteral("\\\"path\\\"\\s*\\\"((?:\\\\.|[^\\\"])*)\\\""));
  for (const auto &vdf : vdfs) {
    QFile file(vdf);
    if (!file.open(QIODevice::ReadOnly)) continue;
    const QString text = QString::fromUtf8(file.readAll());
    auto it = rx.globalMatch(text);
    while (it.hasNext()) {
      QString path = it.next().captured(1);
      path.replace(QStringLiteral("\\\\"), QStringLiteral("\\"));
      addSteamRoot(path, &roots);
    }
  }
  return roots;
}

QString workshopDownloadDest(const QString &id) {
  const auto roots = steamWorkshopRoots();
  if (!roots.isEmpty()) return roots.at(0) + QLatin1Char('/') + id;
  return workshopItemDir(id);
}

QString findProjectJsonDir(const QString &root) {
  if (root.isEmpty() || !QFileInfo::exists(root)) return {};
  if (QFileInfo::exists(root + QStringLiteral("/project.json"))) return root;
  QDirIterator it(root, QStringList{QStringLiteral("project.json")}, QDir::Files,
                  QDirIterator::Subdirectories);
  if (!it.hasNext()) return {};
  it.next();
  return QFileInfo(it.filePath()).absolutePath();
}

QString findInstalledWorkshopItem(const QString &id) {
  QStringList candidates{workshopItemDir(id)};
  for (const auto &root : steamWorkshopRoots()) {
    const QString dir = root + QLatin1Char('/') + id;
    if (!candidates.contains(dir)) candidates << dir;
  }
  for (const auto &dir : candidates) {
    const QString found = findProjectJsonDir(dir);
    if (!found.isEmpty()) return found;
  }
  return {};
}

qint64 directoryBytes(const QString &path) {
  if (path.isEmpty() || !QFileInfo::exists(path)) return 0;
  qint64 total = 0;
  QDirIterator it(path, QDir::Files, QDirIterator::Subdirectories);
  while (it.hasNext()) {
    it.next();
    total += it.fileInfo().size();
  }
  return total;
}

void stripAnsi(QString *text) {
  static const QRegularExpression ansi(QStringLiteral("\\x1b\\[[0-9;]*m"));
  *text = text->replace(ansi, QString());
}

QString workshopDownloadsDir(const QString &contentRoot, const QString &id) {
  QDir dir(contentRoot);
  if (!dir.cdUp() || !dir.cdUp()) return {};
  return dir.filePath(QStringLiteral("downloads/431960/") + id);
}

qint64 workshopBytesOnDisk(const QString &id) {
  qint64 have = directoryBytes(workshopItemDir(id));
  for (const auto &root : steamWorkshopRoots()) {
    have = qMax(have, directoryBytes(root + QLatin1Char('/') + id));
    have = qMax(have, directoryBytes(workshopDownloadsDir(root, id)));
  }
  return have;
}

bool workshopStagingBusy() {
  for (const auto &root : steamWorkshopRoots()) {
    QDir dir(root);
    if (!dir.cdUp() || !dir.cdUp()) continue;
    QDir downloads(dir.filePath(QStringLiteral("downloads/431960")));
    if (!downloads.exists()) continue;
    if (!downloads.entryList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot).isEmpty())
      return true;
  }
  return false;
}

bool steamProcessRunning() {
  QDir proc(QStringLiteral("/proc"));
  for (const auto &entry : proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
    QFile cmd(QStringLiteral("/proc/%1/cmdline").arg(entry));
    if (!cmd.open(QIODevice::ReadOnly)) continue;
    const QByteArray raw = cmd.readAll();
    if (raw.contains("steamwebhelper") || raw.contains("ubuntu12_32/steam") ||
        raw.endsWith("\0steam") || raw.contains("/usr/bin/steam"))
      return true;
  }
  return false;
}

bool steamClientLoggedOn() {
  if (!steamProcessRunning()) return false;
  const QString logPath =
      QDir::homePath() + QStringLiteral("/.local/share/Steam/logs/connection_log.txt");
  QFile log(logPath);
  if (!log.open(QIODevice::ReadOnly)) return steamProcessRunning();
  const qint64 size = log.size();
  if (size > 16384) log.seek(size - 16384);
  const QString text = QString::fromUtf8(log.readAll());
  const int on = text.lastIndexOf(QStringLiteral("[Logged On"));
  const int off = text.lastIndexOf(QStringLiteral("[Logged Off"));
  return on > off;
}

void ensureWallpaperEngineVisibleToSteam() {
  const QStringList acfs = wallpaperEngineManifests();
  if (acfs.isEmpty()) return;
  const QFileInfo acf(acfs.first());
  const QString discoApps = acf.absolutePath();
  const QString we = discoApps + QStringLiteral("/common/wallpaper_engine");
  const QString homeApps =
      QDir::homePath() + QStringLiteral("/.local/share/Steam/steamapps");
  QDir().mkpath(homeApps + QStringLiteral("/common"));
  const QString homeWe = homeApps + QStringLiteral("/common/wallpaper_engine");
  const QString homeAcf = homeApps + QStringLiteral("/appmanifest_431960.acf");
  if (QFileInfo::exists(we) && !QFileInfo::exists(homeWe)) QFile::link(we, homeWe);
  if (acf.exists() && !QFileInfo::exists(homeAcf)) QFile::copy(acf.absoluteFilePath(), homeAcf);
  QFile homeManifest(homeAcf);
  if (homeManifest.open(QIODevice::ReadOnly)) {
    QString text = QString::fromUtf8(homeManifest.readAll());
    homeManifest.close();
    if (!text.contains(QStringLiteral("platform_override_source"))) {
      text.replace(QStringLiteral("\"language\"\t\t\"spanish\""),
                   QStringLiteral("\"language\"\t\t\"spanish\"\n\t\t\"platform_override_source\"\t\t"
                                  "\"windows\"\n\t\t\"platform_override_dest\"\t\t\"linux\""));
      if (homeManifest.open(QIODevice::WriteOnly | QIODevice::Truncate))
        homeManifest.write(text.toUtf8());
    }
  }
}

}  // namespace

QString extractQrAscii(const QString &output);
QString writeQrPng(const QString &ascii, QString *steamLink);

RpcClient::RpcClient(QObject *parent) : QObject(parent) {
  reconnect_.setInterval(1500);
  reconnect_.setSingleShot(false);
  connect(&reconnect_, &QTimer::timeout, this, &RpcClient::connectDaemon);
  settingsDebounce_.setInterval(250);
  settingsDebounce_.setSingleShot(true);
  connect(&settingsDebounce_, &QTimer::timeout, this, &RpcClient::pushSettings);
  statusPoll_.setInterval(1000);
  connect(&statusPoll_, &QTimer::timeout, this, &RpcClient::pollStatus);
  connect(&socket_, &QLocalSocket::connected, this, [this] {
    buffer_.clear();
    setOnline(true, QStringLiteral("Conectada a anis-paperd"));
    bootstrap();
    statusPoll_.start();
  });
  connect(&socket_, &QLocalSocket::readyRead, this, &RpcClient::flushLines);
  connect(&socket_, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError) {
    disconnectDaemon(socket_.errorString());
  });
  connect(&socket_, &QLocalSocket::disconnected, this,
          [this] { disconnectDaemon(QStringLiteral("anis-paperd cerró la conexión")); });
  connectDaemon();
  reconnect_.start();
  connect(&catalog_, &CatalogModel::countChanged, this, &RpcClient::catalogChanged);
  downloadPoll_.setInterval(300);
  connect(&downloadPoll_, &QTimer::timeout, this, &RpcClient::pollDownloadProgress);
}

void RpcClient::setFilter(const QString &filter) {
  if (filter_ == filter) return;
  filter_ = filter;
  catalog_.setFilter(filter_);
  emit filterChanged();
}

void RpcClient::setSelectedId(const QString &id) {
  if (selectedId_ == id) return;
  selectedId_ = id;
  emit selectedIdChanged();
}

void RpcClient::setSelectedOutput(const QString &output) {
  if (selectedOutput_ == output) return;
  selectedOutput_ = output;
  emit selectedOutputChanged();
}

void RpcClient::setVolumePercent(int percent) {
  const int bounded = qBound(0, percent, 100);
  if (volumePercent_ == bounded) return;
  volumePercent_ = bounded;
  emit volumePercentChanged();
  settingsDebounce_.start();
}

void RpcClient::setFpsCap(int fps) {
  const int bounded = qBound(1, fps, 60);
  if (fpsCap_ == bounded) return;
  fpsCap_ = bounded;
  emit fpsCapChanged();
  settingsDebounce_.start();
}

void RpcClient::pushSettings() {
  send("settings.set",
       {{"defaultVolume", volumePercent_ / 100.0}, {"fpsCap", fpsCap_}},
       [this](const QJsonValue &, const QString &error) {
         if (!error.isEmpty()) setToast(error);
       });
}

void RpcClient::pollStatus() {
  send("status.get", {}, [this](const QJsonValue &result, const QString &) {
    if (!result.isObject()) return;
    const auto object = result.toObject();
    const auto renderers = object.value("renderers").toArray();
    QStringList parts;
    QVariantList nextRenderers;
    for (const auto &value : renderers) {
      const auto row = value.toObject();
      nextRenderers.append(row.toVariantMap());
      const auto name = row.value("output").toString();
      const double fps = row.value("fps").toDouble();
      if (name.isEmpty()) continue;
      parts << QStringLiteral("%1 %2 fps").arg(name).arg(fps, 0, 'f', 0);
    }
    if (liveRenderers_ != nextRenderers) {
      liveRenderers_ = nextRenderers;
      emit liveRenderersChanged();
    }
    const auto gaming = object.value("gaming").toObject();
    const bool active = gaming.value("active").toBool();
    if (gamingActive_ != active) {
      gamingActive_ = active;
      emit gamingActiveChanged();
    }
    const QString mode = gaming.value("mode").toString(gamingMode_);
    if (!mode.isEmpty() && gamingMode_ != mode) {
      gamingMode_ = mode;
      emit gamingModeChanged();
    }
    const QString line = parts.isEmpty() ? QStringLiteral("sin renderer") : parts.join(QStringLiteral(" · "));
    if (measuredFps_ != line) {
      measuredFps_ = line;
      emit measuredFpsChanged();
    }
    setStatusLine(QStringLiteral("%1 renderer(s) · %2").arg(renderers.size()).arg(line));
  });
}

void RpcClient::applySelected() {
  if (selectedId_.isEmpty() || selectedOutput_.isEmpty()) {
    setToast(QStringLiteral("Elegí un wallpaper y un monitor."));
    return;
  }
  setApplying(true);
  send("wallpaper.apply", {{"id", selectedId_}, {"output", selectedOutput_}},
       [this](const QJsonValue &, const QString &error) {
         setApplying(false);
         if (!error.isEmpty()) {
           // Expected, non-fatal states read as guidance instead of surfacing
           // the raw renderer-unavailable RPC error on every attempt.
           if (error.startsWith(QStringLiteral("renderer unavailable: static"))) {
             setToast(QStringLiteral("Las imágenes estáticas son previews del "
                                     "catálogo, no wallpapers animados."));
           } else if (error.startsWith(QStringLiteral("renderer unavailable"))) {
             setToast(QStringLiteral("Ese wallpaper no tiene renderer disponible "
                                     "(proyecto dañado o tipo no soportado). "
                                     "Revisá el journal de anispaper."));
           } else if (error == QStringLiteral("safe mode active")) {
             setToast(QStringLiteral("Modo seguro activo: detené el wallpaper y "
                                     "volvé a aplicarlo para reintentar."));
           } else {
             setToast(error);
           }
           return;
         }
         setToast(QStringLiteral("Wallpaper aplicado en %1").arg(selectedOutput_));
         send("monitor.list", {}, [this](const QJsonValue &result, const QString &) {
           fillMonitors(result, &monitors_, &monitorNames_);
           emit monitorsChanged();
         });
       });
}

void RpcClient::stopSelected() {
  if (selectedOutput_.isEmpty()) {
    setToast(QStringLiteral("Elegí un monitor."));
    return;
  }
  send("wallpaper.stop", {{"output", selectedOutput_}},
       [this](const QJsonValue &, const QString &error) {
         setToast(error.isEmpty() ? QStringLiteral("Wallpaper detenido") : error);
       });
}

void RpcClient::refreshCatalog() {
  send("catalog.refresh", {}, [this](const QJsonValue &, const QString &error) {
    if (!error.isEmpty()) setToast(error);
    else setToast(QStringLiteral("Rescan del catálogo programado"));
  });
}

void RpcClient::dismissToast() { setToast({}); }

QVariantList RpcClient::catalogItems() const {
  QVariantList items = catalog_.allItems();
  for (auto &value : items) {
    auto row = value.toMap();
    row.insert(QStringLiteral("preview"), previewUrl(row.value("preview").toString()));
    value = row;
  }
  return items;
}

QVariantMap RpcClient::itemProperties(const QString &id) const {
  return catalog_.itemById(id).value(QStringLiteral("properties")).toObject().toVariantMap();
}

void RpcClient::applyWallpaper(const QString &id, const QString &output) {
  setSelectedId(id);
  QString out = output.trimmed();
  if (out.isEmpty() || (!monitorNames_.isEmpty() && !monitorNames_.contains(out))) {
    if (!selectedOutput_.isEmpty())
      out = selectedOutput_;
    else if (!monitorNames_.isEmpty())
      out = monitorNames_.first();
  }
  setSelectedOutput(out);
  applySelected();
}

void RpcClient::stopWallpaper(const QString &output) {
  setSelectedOutput(output);
  stopSelected();
}

void RpcClient::setGamingMode(const QString &mode) {
  const QString normalized = mode.trimmed().toLower();
  const QString next = (normalized == QStringLiteral("on") ||
                        normalized == QStringLiteral("off") ||
                        normalized == QStringLiteral("auto"))
                           ? normalized
                           : QStringLiteral("auto");
  if (gamingMode_ != next) {
    gamingMode_ = next;
    emit gamingModeChanged();
  }
  send("settings.set", {{"gamingMode", next}}, [this](const QJsonValue &, const QString &error) {
    if (!error.isEmpty()) setToast(error);
  });
}

void RpcClient::setGamingBlacklist(const QStringList &patterns) {
  QStringList normalized;
  for (const QString &raw : patterns) {
    const QString pattern = raw.trimmed().toLower();
    if (pattern.size() < 2 || pattern.size() > 128) continue;
    if (!normalized.contains(pattern)) normalized << pattern;
    if (normalized.size() >= 64) break;
  }
  if (gamingBlacklist_ != normalized) {
    gamingBlacklist_ = normalized;
    emit gamingBlacklistChanged();
  }
  QJsonArray payload;
  for (const QString &pattern : normalized) payload << pattern;
  send("settings.set", {{"gamingBlacklist", payload}}, [this](const QJsonValue &, const QString &error) {
    if (!error.isEmpty()) setToast(error);
  });
}

void RpcClient::setWallpaperProperties(const QString &id, const QVariantMap &values) {
  if (id.trimmed().isEmpty() || values.isEmpty()) return;
  send("wallpaper.setProperties",
       {{"id", id}, {"values", QJsonObject::fromVariantMap(values)}},
       [this](const QJsonValue &, const QString &error) {
         if (!error.isEmpty()) setToast(error);
       });
}

void RpcClient::setWallpaperProperty(const QString &id, const QString &key,
                                    const QVariant &value) {
  if (id.trimmed().isEmpty() || key.trimmed().isEmpty()) return;
  QVariantMap values;
  values.insert(key, value);
  setWallpaperProperties(id, values);
}

QVariantList RpcClient::wallpaperPropertyRows(const QString &id) const {
  const QJsonObject schema =
      catalog_.itemById(id).value(QStringLiteral("properties")).toObject();
  struct Row {
    int order = 0;
    QString key;
    QVariantMap map;
  };
  QVector<Row> rows;
  rows.reserve(schema.size());
  const QSet<QString> types{QStringLiteral("bool"), QStringLiteral("slider"),
                            QStringLiteral("color"), QStringLiteral("combo"),
                            QStringLiteral("textinput"), QStringLiteral("file"),
                            QStringLiteral("directory")};
  for (auto it = schema.begin(); it != schema.end(); ++it) {
    if (!it.value().isObject()) continue;
    const QJsonObject prop = it.value().toObject();
    const QString type = prop.value(QStringLiteral("type")).toString();
    if (!types.contains(type) || prop.value(QStringLiteral("hidden")).toBool()) continue;
    QVariantMap row;
    row.insert(QStringLiteral("key"), it.key());
    row.insert(QStringLiteral("type"), type);
    row.insert(QStringLiteral("text"),
               prop.value(QStringLiteral("text")).toString(it.key()));
    row.insert(QStringLiteral("value"), prop.value(QStringLiteral("value")).toVariant());
    row.insert(QStringLiteral("min"), prop.value(QStringLiteral("min")).toDouble(0));
    row.insert(QStringLiteral("max"), prop.value(QStringLiteral("max")).toDouble(100));
    const double step = prop.value(QStringLiteral("step")).toDouble(1);
    row.insert(QStringLiteral("step"), step > 0 ? step : 1.0);
    QVariantList options;
    for (const auto &optValue : prop.value(QStringLiteral("options")).toArray()) {
      if (optValue.isObject()) {
        const auto opt = optValue.toObject();
        QVariantMap mapped;
        mapped.insert(QStringLiteral("label"),
                      opt.value(QStringLiteral("label"))
                          .toString(opt.value(QStringLiteral("text")).toString()));
        mapped.insert(QStringLiteral("value"), opt.value(QStringLiteral("value")).toVariant());
        options.append(mapped);
      } else {
        QVariantMap mapped;
        mapped.insert(QStringLiteral("label"), optValue.toVariant().toString());
        mapped.insert(QStringLiteral("value"), optValue.toVariant());
        options.append(mapped);
      }
    }
    row.insert(QStringLiteral("options"), options);
    rows.push_back(Row{prop.value(QStringLiteral("order")).toInt(0), it.key(), row});
  }
  std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
    if (a.order != b.order) return a.order < b.order;
    return a.key < b.key;
  });
  QVariantList out;
  for (const auto &row : rows) out.append(row.map);
  return out;
}

void RpcClient::openExternal(const QString &url) {
  if (url.startsWith(QLatin1String("steam:"), Qt::CaseInsensitive)) {
    setToast(QStringLiteral("La suscripción es en AnisPaper, no en Steam."));
    return;
  }
  QDesktopServices::openUrl(QUrl(url));
}

void RpcClient::setWorkshopBusy(bool busy) {
  if (workshopBusy_ == busy) return;
  workshopBusy_ = busy;
  emit workshopBusyChanged();
}

void RpcClient::setWorkshopError(const QString &error) {
  if (workshopError_ == error) return;
  workshopError_ = error;
  emit workshopErrorChanged();
}

void RpcClient::subscribeWorkshop(const QString &publishedFileId) {
  const QString id = publishedFileId.trimmed();
  if (id.isEmpty() || id.size() > 20) return;
  for (const QChar ch : id) {
    if (!ch.isDigit()) return;
  }
  if (workshopInstallBusy_) {
    if (downloadId_ == id) {
      if (steamClientLoggedOn()) openSteamForWorkshop();
      return;
    }
    finishWorkshopInstall(downloadId_, false, QStringLiteral("cancel"));
  }
  workshopInstallBusy_ = true;
  downloadId_ = id;
  downloadAttempt_ = 0;
  downloadExpectedBytes_ = expectedDownloadBytes(id);
  downloadWaitUntilMs_ = 0;
  downloadLastBytes_ = 0;
  downloadBeganMs_ = 0;
  downloadError_.clear();
  licensedAttempted_ = false;
  depotAttempted_ = false;
  wipeSteamSecrets();
  setSteamAuthNeeded(false, false);
  const QString existing = findInstalledWorkshopItem(id);
  if (!existing.isEmpty() || !catalog_.itemById(QStringLiteral("steam:") + id).isEmpty()) {
    clearPendingWorkshop();
    finishWorkshopInstall(id, true, {});
    setToast(QStringLiteral("Ese wallpaper ya está en el catálogo."));
    return;
  }
  savePendingWorkshop(id);
  setDownload(true, 0, QStringLiteral("Preparando descarga…"));
  setToast(QStringLiteral("Descargando wallpaper…"));
  qrRestarts_ = 0;
  qrPromptOpened_ = false;
  lastOpenedQrLink_.clear();
  qrWanted_ = false;
  steamUseClient_ = true;
  lastSteamPokeMs_ = 0;
  fetchDownloadSize(id);
  ensureSteamcmdThenInstall(id);
}

void RpcClient::finishWorkshopInstall(const QString &id, bool ok, const QString &detail) {
  if (!workshopInstallBusy_) return;
  downloadPoll_.stop();
  workshopInstallBusy_ = false;
  downloadWaitUntilMs_ = 0;
  downloadBeganMs_ = 0;
  downloadId_.clear();
  steamAuthQr_.clear();
  steamAuthQrUrl_.clear();
  steamAuthQrLink_.clear();
  stopQrProcess();
  stopUgcProcess();
  wipeSteamSecrets();
  setSteamAuthNeeded(false, false);
  const bool cancelled = detail.contains(QStringLiteral("cancel"), Qt::CaseInsensitive);
  if (ok || cancelled) clearPendingWorkshop();
  if (!ok) {
    setDownload(false, 0, {});
    setToast(detail.isEmpty() ? QStringLiteral("No se pudo descargar el wallpaper.") : detail);
    return;
  }
  setDownload(true, 100, QStringLiteral("Listo"));
  QString folder = QFileInfo(findInstalledWorkshopItem(id)).absolutePath();
  if (folder.isEmpty()) {
    folder = workshopInstallRoot() + QStringLiteral("/steamapps/workshop/content/431960");
  }
  if (QDir(folder).exists()) {
    send("catalog.addFolder", {{"path", folder}}, {});
  }
  refreshCatalog();
  setToast(QStringLiteral("Listo — %1 ya está en el catálogo.").arg(id));
  QTimer::singleShot(1400, this, [this] { setDownload(false, 0, {}); });
}

void RpcClient::resumePendingWorkshop() {
  if (workshopInstallBusy_) return;
  const QString id = loadPendingWorkshop();
  if (id.isEmpty()) return;
  subscribeWorkshop(id);
}

void RpcClient::fetchDownloadSize(const QString &id) {
  if (downloadExpectedBytes_ > 0) return;
  QUrl url(QStringLiteral(
      "https://api.steampowered.com/ISteamRemoteStorage/GetPublishedFileDetails/v1/"));
  QUrlQuery body;
  body.addQueryItem(QStringLiteral("itemcount"), QStringLiteral("1"));
  body.addQueryItem(QStringLiteral("publishedfileids[0]"), id);
  QNetworkRequest request(url);
  request.setHeader(QNetworkRequest::ContentTypeHeader,
                    QStringLiteral("application/x-www-form-urlencoded;charset=UTF-8"));
  auto *reply = http_.post(request, body.query(QUrl::FullyEncoded).toUtf8());
  connect(reply, &QNetworkReply::finished, this, [this, reply, id] {
    reply->deleteLater();
    if (!workshopInstallBusy_ || downloadId_ != id) return;
    if (reply->error() != QNetworkReply::NoError) return;
    const auto document = QJsonDocument::fromJson(reply->readAll());
    const auto details = document.object()
                             .value(QStringLiteral("response"))
                             .toObject()
                             .value(QStringLiteral("publishedfiledetails"))
                             .toArray();
    if (details.isEmpty()) return;
    const qint64 size =
        details.at(0).toObject().value(QStringLiteral("file_size")).toVariant().toLongLong();
    if (size > 0) downloadExpectedBytes_ = size;
  });
}

void RpcClient::ensureSteamcmdThenInstall(const QString &id) {
  steamUseClient_ = true;
  qrWanted_ = false;
  setSteamAuthNeeded(false, false);
  ensureWallpaperEngineVisibleToSteam();
  startSteamClientWait(id, {});
}

void RpcClient::runSteamcmdInstall(const QString &id) {
  QDir().mkpath(workshopInstallRoot());
  downloadLog_.clear();
  downloadAttempt_++;
  setDownload(true, qMax(1, downloadPercent_),
              downloadAttempt_ > 1 ? QStringLiteral("Reintentando SteamCMD…")
                                   : QStringLiteral("Conectando a SteamCMD…"));
  downloadPoll_.start();
  auto *process = new QProcess(this);
  process->setProcessChannelMode(QProcess::MergedChannels);
  process->setWorkingDirectory(steamcmdHome());
  process->setProcessEnvironment(steamcmdEnvironment());
  connect(process, &QProcess::readyRead, this, [this, process] {
    downloadLog_.append(process->readAll());
    QString chunk = QString::fromUtf8(downloadLog_);
    stripAnsi(&chunk);
    QRegularExpression percentRe(
        QStringLiteral("progress:\\s*([0-9]+(?:\\.[0-9]+)?)|\\[\\s*([0-9]{1,3})%\\]"));
    const auto match = percentRe.match(chunk);
    if (match.hasMatch()) {
      const int percent = static_cast<int>(
          (match.captured(1).isEmpty() ? match.captured(2) : match.captured(1)).toDouble());
      if (percent >= 0 && percent <= 100)
        setDownload(true, percent, QStringLiteral("Bajando… %1%").arg(percent));
    } else if (chunk.contains(QStringLiteral("Downloading item"), Qt::CaseInsensitive)) {
      setDownload(true, qMax(downloadPercent_, 8), QStringLiteral("Bajando wallpaper…"));
    }
  });
  connect(process, &QProcess::finished, this,
          [this, process, id](int, QProcess::ExitStatus) {
            downloadLog_.append(process->readAll());
            process->deleteLater();
            if (!workshopInstallBusy_ || downloadId_ != id) return;
            QString output = QString::fromUtf8(downloadLog_);
            stripAnsi(&output);
            QFile logFile(steamcmdHome() + QStringLiteral("/last-install.log"));
            if (logFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
              logFile.write(output.toUtf8());
            }
            const bool downloaded =
                output.contains(QStringLiteral("Success. Downloaded item"), Qt::CaseInsensitive);
            const QString installed = findInstalledWorkshopItem(id);
            if (downloaded || !installed.isEmpty()) {
              finishWorkshopInstall(id, true, {});
              return;
            }
            QString reason = QStringLiteral("SteamCMD no pudo bajar ese wallpaper.");
            const auto lines = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            for (const auto &line : lines) {
              if (line.contains(QStringLiteral("Failure"), Qt::CaseInsensitive) ||
                  line.contains(QStringLiteral("ERROR"), Qt::CaseInsensitive) ||
                  line.contains(QStringLiteral("No subscription"), Qt::CaseInsensitive) ||
                  line.contains(QStringLiteral("error while loading"), Qt::CaseInsensitive)) {
                reason = line.trimmed().left(200);
                break;
              }
            }
            downloadError_ = reason;
            requestSteamAuth(id);
          });
  QStringList args{steamcmdBinary(), QStringLiteral("+@ShutdownOnFailedCommand"),
                   QStringLiteral("1"), QStringLiteral("+@NoPromptForPassword"),
                   QStringLiteral("1"), QStringLiteral("+force_install_dir"),
                   workshopInstallRoot(), QStringLiteral("+login"),
                   QStringLiteral("anonymous"), QStringLiteral("+workshop_download_item"),
                   QStringLiteral("431960"), id, QStringLiteral("+quit")};
  process->start(QStringLiteral("/bin/bash"), args);
  if (!process->waitForStarted(10000)) {
    process->deleteLater();
    requestSteamAuth(id);
  }
}

void RpcClient::setDownload(bool active, int percent, const QString &label) {
  const int clamped = percent < 0 ? -1 : qBound(0, percent, 100);
  if (downloadActive_ == active && downloadPercent_ == clamped && downloadLabel_ == label) return;
  downloadActive_ = active;
  downloadPercent_ = clamped;
  downloadLabel_ = label;
  emit downloadChanged();
}

qint64 RpcClient::expectedDownloadBytes(const QString &id) const {
  for (const auto &value : workshopItems_) {
    const auto row = value.toMap();
    if (row.value(QStringLiteral("id")).toString() != id) continue;
    const qint64 size = row.value(QStringLiteral("fileSize")).toLongLong();
    if (size > 0) return size;
  }
  return 0;
}

void RpcClient::pollDownloadProgress() {
  const QString id = downloadId_;
  if (id.isEmpty() || !downloadActive_ || !workshopInstallBusy_) return;
  const QString installed = findInstalledWorkshopItem(id);
  if (!installed.isEmpty()) {
    finishWorkshopInstall(id, true, {});
    return;
  }
  if (qrWanted_) {
    /* QR login is disabled — subscribe goes through the Steam app. */
  }
  const qint64 have = workshopBytesOnDisk(id);
  if (have > downloadLastBytes_) {
    downloadLastBytes_ = have;
    if (downloadWaitUntilMs_ > 0)
      downloadWaitUntilMs_ = QDateTime::currentMSecsSinceEpoch() + 45000;
  }
  const bool helperAlive =
      ugcProcess_ && ugcProcess_->state() != QProcess::NotRunning;
  if (helperAlive || workshopStagingBusy()) {
    downloadWaitUntilMs_ = QDateTime::currentMSecsSinceEpoch() + 5 * 60 * 1000;
  }
  if (downloadExpectedBytes_ > 0 && have > 0) {
    const int percent =
        qBound(5, static_cast<int>((have * 100) / downloadExpectedBytes_), 99);
    setDownload(true, percent,
                QStringLiteral("Steam · Manage Downloads  %1 MB")
                    .arg(have / (1024 * 1024)));
  } else if (have > 0) {
    setDownload(true, -1,
                QStringLiteral("Steam · Manage Downloads  %1 MB")
                    .arg(have / (1024 * 1024)));
  } else if (!helperAlive && downloadWaitUntilMs_ > 0) {
    const bool running = steamProcessRunning();
    const bool loggedOn = steamClientLoggedOn();
    QString label = QStringLiteral("Abrí la app de Steam");
    if (running && !loggedOn)
      label = QStringLiteral("Iniciá sesión en Steam (no en AnisPaper)");
    else if (loggedOn)
      label = QStringLiteral("Encolado en Steam · Manage Downloads");
    setDownload(true, -1, label);
    if (loggedOn) {
      const qint64 now = QDateTime::currentMSecsSinceEpoch();
      if (now - lastSteamPokeMs_ > 8000) {
        lastSteamPokeMs_ = now;
        openSteamForWorkshop();
      }
    }
  }
  if (downloadWaitUntilMs_ > 0 && QDateTime::currentMSecsSinceEpoch() > downloadWaitUntilMs_) {
    startSteamClientWait(id, downloadError_);
  }
}

void RpcClient::startSteamClientWait(const QString &id, const QString &steamcmdError) {
  Q_UNUSED(steamcmdError);
  if (downloadWaitUntilMs_ > 0 && QDateTime::currentMSecsSinceEpoch() > downloadWaitUntilMs_) {
    const bool steamBusy =
        steamClientLoggedOn() &&
        (workshopStagingBusy() ||
         (ugcProcess_ && ugcProcess_->state() != QProcess::NotRunning));
    if (steamBusy) {
      downloadWaitUntilMs_ = QDateTime::currentMSecsSinceEpoch() + 5 * 60 * 1000;
      setDownload(true, -1, QStringLiteral("Encolado en Steam · Manage Downloads"));
      return;
    }
    finishWorkshopInstall(
        id, false,
        QStringLiteral("Steam no bajó el wallpaper. Dejá la app de Steam abierta "
                       "con la cuenta que tiene Wallpaper Engine."));
    return;
  }
  if (downloadWaitUntilMs_ == 0) {
    downloadWaitUntilMs_ = QDateTime::currentMSecsSinceEpoch() + 12 * 60 * 1000;
    downloadBeganMs_ = QDateTime::currentMSecsSinceEpoch();
    downloadLastBytes_ = workshopBytesOnDisk(id);
    lastSteamPokeMs_ = QDateTime::currentMSecsSinceEpoch();
    if (steamClientLoggedOn()) openSteamForWorkshop();
  }
  const bool running = steamProcessRunning();
  const bool loggedOn = steamClientLoggedOn();
  if (!running)
    setToast(QStringLiteral("Abrí Steam. AnisPaper no pide usuario ni contraseña."));
  else if (!loggedOn)
    setToast(QStringLiteral("Iniciá sesión en Steam. AnisPaper no pide tu contraseña."));
  else
    setToast(QStringLiteral("Steam detectado — encolando en Manage Downloads…"));
  setDownload(true, -1,
              !running ? QStringLiteral("Abrí la app de Steam")
              : !loggedOn ? QStringLiteral("Iniciá sesión en Steam (no en AnisPaper)")
                          : QStringLiteral("Encolado en Steam · Manage Downloads"));
  downloadPoll_.start();
}

void RpcClient::openSteamForWorkshop() {
  if (downloadId_.isEmpty() || !steamClientLoggedOn()) return;
  startUgcDownload(downloadId_);
}

void RpcClient::stopUgcProcess() {
  if (!ugcProcess_) return;
  auto *process = ugcProcess_.data();
  ugcProcess_.clear();
  process->kill();
  process->deleteLater();
}

void RpcClient::startUgcDownload(const QString &id) {
  if (id.isEmpty()) return;
  if (ugcProcess_ && ugcProcess_->state() != QProcess::NotRunning) return;
  const QString helper = ensureUgcHelper();
  const QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
  if (helper.isEmpty() || python.isEmpty()) {
    QDesktopServices::openUrl(
        QUrl(QStringLiteral("steam://url/SubscribeToFile/%1").arg(id)));
    return;
  }
  const QString steamRoot =
      QDir::homePath() + QStringLiteral("/.local/share/Steam");
  auto *process = new QProcess(this);
  ugcProcess_ = process;
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  env.insert(QStringLiteral("SteamAppId"), QStringLiteral("431960"));
  env.insert(QStringLiteral("SteamGameId"), QStringLiteral("431960"));
  env.insert(QStringLiteral("LD_LIBRARY_PATH"),
             steamRoot + QStringLiteral("/linux64:") + env.value(QStringLiteral("LD_LIBRARY_PATH")));
  process->setProcessEnvironment(env);
  process->setWorkingDirectory(QFileInfo(helper).absolutePath());
  process->setProcessChannelMode(QProcess::MergedChannels);
  connect(process, &QProcess::readyRead, this, [this, process] {
    const QString chunk = QString::fromUtf8(process->readAll());
    QRegularExpression bytesRe(QStringLiteral("BYTES (\\d+) (\\d+)"));
    const auto match = bytesRe.match(chunk);
    if (match.hasMatch()) {
      const qint64 have = match.captured(1).toLongLong();
      const qint64 total = match.captured(2).toLongLong();
      if (total > 0 && have >= 0) {
        const int percent = qBound(1, static_cast<int>((have * 100) / total), 99);
        setDownload(true, percent,
                    QStringLiteral("Steam · Manage Downloads  %1 MB")
                        .arg(have / (1024 * 1024)));
      } else {
        setDownload(true, -1, QStringLiteral("Encolado en Steam · Manage Downloads"));
      }
    } else if (chunk.contains(QStringLiteral("QUEUED"))) {
      setDownload(true, -1, QStringLiteral("Encolado en Steam · Manage Downloads"));
    }
  });
  connect(process, &QProcess::finished, this, [this, process, id](int code, QProcess::ExitStatus) {
    process->deleteLater();
    if (ugcProcess_ == process) ugcProcess_.clear();
    if (!workshopInstallBusy_ || downloadId_ != id) return;
    if (!findInstalledWorkshopItem(id).isEmpty() || code == 0) {
      if (!findInstalledWorkshopItem(id).isEmpty()) finishWorkshopInstall(id, true, {});
    }
  });
  process->start(python, {helper, QStringLiteral("--id"), id, QStringLiteral("--timeout"),
                          QStringLiteral("900")});
  if (!process->waitForStarted(8000)) {
    process->deleteLater();
    ugcProcess_.clear();
    QDesktopServices::openUrl(
        QUrl(QStringLiteral("steam://url/SubscribeToFile/%1").arg(id)));
  }
}

void RpcClient::setSteamAuthNeeded(bool needed, bool guard) {
  steamAuthNeeded_ = needed;
  steamGuardNeeded_ = guard;
  emit steamAuthNeededChanged();
  syncNativeSteamAuth();
}

void RpcClient::syncNativeSteamAuth() {
  // One login UI only: Starlight web modal. The native QDialog duplicated it.
  if (steamAuthDialog_) steamAuthDialog_->hide();
}

void RpcClient::wipeSteamSecrets() {
  steamAuthPassword_.fill(QLatin1Char('x'));
  steamAuthPassword_.clear();
  steamAuthGuard_.fill(QLatin1Char('x'));
  steamAuthGuard_.clear();
  wipeSteamcmdScript();
}

void RpcClient::requestSteamAuth(const QString &id) {
  setSteamAuthNeeded(false, false);
  startSteamClientWait(id, {});
}

void RpcClient::startSteamQr() {
  if (!workshopInstallBusy_ || downloadId_.isEmpty()) return;
  qrWanted_ = true;
  setDownload(true, -1, QStringLiteral("Escaneá el QR de Steam Mobile"));
  startQrDownload(downloadId_);
}

void RpcClient::stopQrProcess() {
  qrWanted_ = false;
  QFile stop(qrStopPath());
  if (stop.open(QIODevice::WriteOnly | QIODevice::Truncate)) stop.write("1");
  if (qrProcess_) {
    auto *process = qrProcess_.data();
    qrProcess_.clear();
    process->kill();
    process->deleteLater();
  }
  QFile pidFile(qrPidPath());
  if (pidFile.open(QIODevice::ReadOnly)) {
    const qint64 pid = QString::fromUtf8(pidFile.readAll()).trimmed().toLongLong();
    if (pid > 1) ::kill(static_cast<pid_t>(pid), SIGTERM);
  }
}

QString extractQrAscii(const QString &output) {
  QString best;
  int bestRows = 0;
  int from = 0;
  while (true) {
    int idx = output.indexOf(QStringLiteral("sign in with this QR code:"), from,
                             Qt::CaseInsensitive);
    if (idx < 0) {
      idx = output.indexOf(QStringLiteral("The QR code has changed:"), from,
                           Qt::CaseInsensitive);
    }
    if (idx < 0) break;
    const auto lines = output.mid(idx).split(QLatin1Char('\n'));
    QStringList qr;
    for (int i = 1; i < lines.size(); ++i) {
      QString line = lines.at(i);
      line.replace(QLatin1Char('\r'), QString());
      while (line.endsWith(QLatin1Char(' '))) line.chop(1);
      bool onlyBlocks = true;
      bool hasBlock = false;
      for (const QChar ch : line) {
        if (ch == QChar(0x2588) || ch == QChar(0x2580) || ch == QChar(0x2584)) hasBlock = true;
        else if (ch != QLatin1Char(' ') && ch != QLatin1Char('\t')) {
          onlyBlocks = false;
          break;
        }
      }
      if (!onlyBlocks) break;
      if (hasBlock || !qr.isEmpty()) qr << line;
    }
    while (!qr.isEmpty() && qr.first().trimmed().isEmpty()) qr.removeFirst();
    while (!qr.isEmpty() && qr.last().trimmed().isEmpty()) qr.removeLast();
    if (qr.size() > bestRows) {
      int indent = 1000;
      for (const auto &row : qr) {
        int i = 0;
        while (i < row.size() && row.at(i) == QLatin1Char(' ')) ++i;
        if (i < row.size()) indent = qMin(indent, i);
      }
      if (indent == 1000) indent = 0;
      QStringList trimmed;
      for (const auto &row : qr) trimmed << (row.size() >= indent ? row.mid(indent) : row);
      best = trimmed.join(QLatin1Char('\n'));
      bestRows = qr.size();
    }
    from = idx + 1;
  }
  return bestRows >= 25 ? best : QString();
}

QString writeQrPng(const QString &ascii, QString *steamLink) {
  const auto lines = ascii.split(QLatin1Char('\n'));
  if (lines.isEmpty()) return {};
  int rawW = 0;
  for (const auto &line : lines) rawW = qMax(rawW, line.size());
  if (rawW < 16 || lines.size() < 16) return {};
  const int moduleW = (rawW + 1) / 2;
  const int moduleH = lines.size();
  const int scale = 8;
  const int pad = 16;
  QImage img(moduleW * scale + pad * 2, moduleH * scale + pad * 2, QImage::Format_RGB32);
  img.fill(qRgb(255, 255, 255));
  for (int y = 0; y < moduleH; ++y) {
    const QString row = lines.at(y);
    for (int mx = 0; mx < moduleW; ++mx) {
      const int cx = mx * 2;
      const bool dark = cx < row.size() && row.at(cx) != QLatin1Char(' ');
      if (!dark) continue;
      for (int dy = 0; dy < scale; ++dy) {
        for (int dx = 0; dx < scale; ++dx) {
          img.setPixel(pad + mx * scale + dx, pad + y * scale + dy, qRgb(0, 0, 0));
        }
      }
    }
  }
  const QString path = steamcmdHome() + QStringLiteral("/steam-qr.png");
  QDir().mkpath(steamcmdHome());
  if (!img.save(path, "PNG")) return {};
  QProcess zbar;
  zbar.start(QStringLiteral("zbarimg"), {QStringLiteral("-q"), path});
  if (zbar.waitForFinished(1500) && zbar.exitCode() == 0) {
    const QByteArray decoded = zbar.readAllStandardOutput().trimmed();
    const QByteArray prefix("QR-Code:");
    if (decoded.startsWith(prefix)) {
      const QString payload = QString::fromUtf8(decoded.mid(prefix.size())).trimmed();
      if (payload.startsWith(QLatin1String("https://"))) {
        if (steamLink) *steamLink = payload;
        QProcess encode;
        encode.start(QStringLiteral("qrencode"),
                     {QStringLiteral("-s"), QStringLiteral("12"), QStringLiteral("-m"),
                      QStringLiteral("4"), QStringLiteral("-o"), path, payload});
        encode.waitForFinished(1500);
      }
    }
  }
  QString url = previewUrl(path);
  if (url.isEmpty()) return {};
  url += QStringLiteral("&t=") + QString::number(QDateTime::currentMSecsSinceEpoch());
  return url;
}

void RpcClient::startQrDownload(const QString &id) {
  qrWanted_ = true;
  const QString binary = depotDownloaderBinary();
  if (!QFileInfo::exists(binary)) return;
  if (qrHelperRunning()) {
    downloadPoll_.start();
    return;
  }
  QFile::remove(qrStopPath());
  QFile::remove(qrLogPath());
  const QString dest = workshopDownloadDest(id);
  QDir().mkpath(dest);
  const QString helper = ensureQrHelper();
  if (!QFileInfo::exists(helper)) return;
  const QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
  if (python.isEmpty()) return;
  const QStringList args{helper,
                         QStringLiteral("--id"),
                         id,
                         QStringLiteral("--dest"),
                         dest,
                         QStringLiteral("--bin"),
                         binary,
                         QStringLiteral("--log"),
                         qrLogPath(),
                         QStringLiteral("--png"),
                         qrPngPath(),
                         QStringLiteral("--stop"),
                         qrStopPath(),
                         QStringLiteral("--home"),
                         steamcmdHome(),
                         QStringLiteral("--link"),
                         qrLinkPath()};
  qint64 pid = 0;
  if (!QProcess::startDetached(python, args, QFileInfo(binary).absolutePath(), &pid)) return;
  downloadPoll_.start();
}

void RpcClient::submitSteamAuth(const QString &user, const QString &password,
                               const QString &guard) {
  if (!workshopInstallBusy_ || downloadId_.isEmpty()) return;
  const QString name = user.trimmed();
  if (name.isEmpty()) {
    setToast(QStringLiteral("Falta el usuario de Steam."));
    return;
  }
  steamAuthUser_ = name;
  if (!password.isEmpty()) steamAuthPassword_ = password;
  if (steamAuthPassword_.isEmpty() && !steamGuardNeeded_) {
    setToast(QStringLiteral("Falta la contraseña de Steam."));
    return;
  }
  steamAuthGuard_ = guard.trimmed();
  saveSteamUser(name);
  stopQrProcess();
  setSteamAuthNeeded(false, false);
  runLicensedSteamcmd(downloadId_);
}

void RpcClient::cancelSteamAuth() {
  if (!workshopInstallBusy_) {
    setSteamAuthNeeded(false, false);
    return;
  }
  finishWorkshopInstall(downloadId_, false, QStringLiteral("Descarga cancelada."));
}

void RpcClient::runLicensedSteamcmd(const QString &id) {
  licensedAttempted_ = true;
  QDir().mkpath(workshopInstallRoot());
  QDir().mkpath(steamcmdHome());
  downloadLog_.clear();
  setDownload(true, qMax(8, downloadPercent_),
              QStringLiteral("Entrando a SteamCMD con tu cuenta…"));
  downloadPoll_.start();
  {
    QFile script(steamcmdScriptPath());
    if (!script.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
      runDepotDownloader(id);
      return;
    }
    script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    QByteArray body;
    body += "@ShutdownOnFailedCommand 1\n@NoPromptForPassword 1\n";
    body += "@sSteamCmdForcePlatformType windows\n";
    body += "force_install_dir " + workshopInstallRoot().toUtf8() + "\nlogin ";
    body += steamAuthUser_.toUtf8();
    if (!steamAuthPassword_.isEmpty()) {
      body += ' ';
      body += steamAuthPassword_.toUtf8();
    }
    if (!steamAuthGuard_.isEmpty()) {
      body += ' ';
      body += steamAuthGuard_.toUtf8();
    }
    body += "\nworkshop_download_item 431960 " + id.toUtf8() + "\nquit\n";
    script.write(body);
  }
  auto *process = new QProcess(this);
  process->setProcessChannelMode(QProcess::MergedChannels);
  process->setWorkingDirectory(steamcmdHome());
  process->setProcessEnvironment(steamcmdEnvironment());
  connect(process, &QProcess::readyRead, this, [this, process] {
    downloadLog_.append(process->readAll());
    QString chunk = QString::fromUtf8(downloadLog_);
    stripAnsi(&chunk);
    if (chunk.contains(QStringLiteral("Logged in OK"), Qt::CaseInsensitive) ||
        chunk.contains(QStringLiteral("Waiting for user info"), Qt::CaseInsensitive)) {
      setDownload(true, qMax(downloadPercent_, 18), QStringLiteral("Sesión Steam OK — bajando…"));
    }
    QRegularExpression percentRe(
        QStringLiteral("progress:\\s*([0-9]+(?:\\.[0-9]+)?)|\\[\\s*([0-9]{1,3})%\\]"));
    const auto match = percentRe.match(chunk);
    if (match.hasMatch()) {
      const int percent = static_cast<int>(
          (match.captured(1).isEmpty() ? match.captured(2) : match.captured(1)).toDouble());
      if (percent >= 0 && percent <= 100)
        setDownload(true, percent, QStringLiteral("Bajando… %1%").arg(percent));
    } else if (chunk.contains(QStringLiteral("Downloading item"), Qt::CaseInsensitive)) {
      setDownload(true, qMax(downloadPercent_, 22), QStringLiteral("Bajando wallpaper…"));
    }
  });
  connect(process, &QProcess::finished, this, [this, process, id](int, QProcess::ExitStatus) {
    downloadLog_.append(process->readAll());
    process->deleteLater();
    wipeSteamcmdScript();
    if (!workshopInstallBusy_ || downloadId_ != id) return;
    QString output = QString::fromUtf8(downloadLog_);
    stripAnsi(&output);
    QFile logFile(steamcmdHome() + QStringLiteral("/last-install.log"));
    if (logFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
      logFile.write(redactSteamOutput(output).toUtf8());
    }
    const bool downloaded =
        output.contains(QStringLiteral("Success. Downloaded item"), Qt::CaseInsensitive);
    if (downloaded || !findInstalledWorkshopItem(id).isEmpty()) {
      finishWorkshopInstall(id, true, {});
      return;
    }
    const bool needGuard =
        output.contains(QStringLiteral("Steam Guard"), Qt::CaseInsensitive) ||
        output.contains(QStringLiteral("two-factor"), Qt::CaseInsensitive) ||
        output.contains(QStringLiteral("auth code"), Qt::CaseInsensitive) ||
        output.contains(QStringLiteral("set_steam_guard_code"), Qt::CaseInsensitive);
    const bool badPass = output.contains(QStringLiteral("Invalid Password"), Qt::CaseInsensitive) ||
                         output.contains(QStringLiteral("Login Failure"), Qt::CaseInsensitive);
    if (needGuard) {
      steamGuardNeeded_ = true;
      requestSteamAuth(id);
      return;
    }
    if (badPass) {
      steamGuardNeeded_ = false;
      requestSteamAuth(id);
      setToast(QStringLiteral("Usuario o contraseña de Steam inválidos."));
      return;
    }
    downloadError_ = QStringLiteral("SteamCMD con cuenta no pudo bajar el ítem.");
    runDepotDownloader(id);
  });
  process->start(QStringLiteral("/bin/bash"),
                 {steamcmdBinary(), QStringLiteral("+runscript"), steamcmdScriptPath()});
  if (!process->waitForStarted(10000)) {
    process->deleteLater();
    wipeSteamcmdScript();
    runDepotDownloader(id);
  }
}

void RpcClient::runDepotDownloader(const QString &id) {
  const QString binary = depotDownloaderBinary();
  if (!QFileInfo::exists(binary) || steamAuthUser_.isEmpty()) {
    requestSteamAuth(id);
    return;
  }
  depotAttempted_ = true;
  const QString dest = workshopDownloadDest(id);
  QDir().mkpath(dest);
  downloadLog_.clear();
  setDownload(true, qMax(10, downloadPercent_), QStringLiteral("Bajando con DepotDownloader…"));
  downloadPoll_.start();
  auto *process = new QProcess(this);
  process->setProcessChannelMode(QProcess::MergedChannels);
  process->setWorkingDirectory(QFileInfo(binary).absolutePath());
  process->setProcessEnvironment(steamcmdEnvironment());
  connect(process, &QProcess::readyRead, this, [this, process] {
    downloadLog_.append(process->readAll());
    QString chunk = QString::fromUtf8(downloadLog_);
    stripAnsi(&chunk);
    QRegularExpression percentRe(
        QStringLiteral("progress:\\s*([0-9]+(?:\\.[0-9]+)?)|\\[\\s*([0-9]{1,3})%\\]"));
    const auto match = percentRe.match(chunk);
    if (match.hasMatch()) {
      const int percent = static_cast<int>(
          (match.captured(1).isEmpty() ? match.captured(2) : match.captured(1)).toDouble());
      if (percent >= 0 && percent <= 100)
        setDownload(true, percent, QStringLiteral("Bajando… %1%").arg(percent));
    }
  });
  connect(process, &QProcess::finished, this, [this, process, id](int, QProcess::ExitStatus) {
    downloadLog_.append(process->readAll());
    process->deleteLater();
    if (!workshopInstallBusy_ || downloadId_ != id) return;
    QString output = QString::fromUtf8(downloadLog_);
    stripAnsi(&output);
    QFile logFile(steamcmdHome() + QStringLiteral("/last-depot.log"));
    if (logFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
      logFile.write(redactSteamOutput(output).toUtf8());
    }
    if (!findInstalledWorkshopItem(id).isEmpty() ||
        QFileInfo::exists(workshopItemDir(id) + QStringLiteral("/project.json"))) {
      finishWorkshopInstall(id, true, {});
      return;
    }
    requestSteamAuth(id);
    setToast(QStringLiteral(
        "No bajó. Revisá usuario, contraseña y Steam Guard (código del mail)."));
  });
  QStringList args{QStringLiteral("-app"),
                   QStringLiteral("431960"),
                   QStringLiteral("-pubfile"),
                   id,
                   QStringLiteral("-username"),
                   steamAuthUser_,
                   QStringLiteral("-remember-password"),
                   QStringLiteral("-no-mobile"),
                   QStringLiteral("-os"),
                   QStringLiteral("windows"),
                   QStringLiteral("-dir"),
                   dest,
                   QStringLiteral("-loginid"),
                   QStringLiteral("4319601")};
  process->start(binary, args);
  if (!process->waitForStarted(10000)) {
    process->deleteLater();
    requestSteamAuth(id);
    return;
  }
  if (!steamAuthPassword_.isEmpty()) {
    process->write(steamAuthPassword_.toUtf8());
    process->write("\n");
  }
}

void RpcClient::searchWorkshop(const QString &query, int page, const QString &ratings) {
  ++workshopSearchGen_;
  const int gen = workshopSearchGen_;
  auto drop = [](QPointer<QNetworkReply> &slot) {
    if (!slot) return;
    QNetworkReply *old = slot.data();
    slot.clear();
    old->disconnect();
    old->abort();
    old->deleteLater();
  };
  drop(workshopBrowseReply_);
  drop(workshopDetailsReply_);

  workshopRatings_ = parseWorkshopRatings(ratings);
  workshopPage_ = qMax(1, page);
  emit workshopPageChanged();
  workshopItems_.clear();
  emit workshopItemsChanged();
  setWorkshopError({});
  if (workshopRatings_.isEmpty()) {
    setWorkshopBusy(false);
    setWorkshopError(QStringLiteral("Activá al menos un filtro de contenido."));
    return;
  }
  setWorkshopBusy(true);

  QUrl url(QStringLiteral("https://steamcommunity.com/workshop/browse/"));
  QUrlQuery queryItems;
  queryItems.addQueryItem(QStringLiteral("appid"), QStringLiteral("431960"));
  queryItems.addQueryItem(QStringLiteral("section"), QStringLiteral("readytouseitems"));
  queryItems.addQueryItem(QStringLiteral("p"), QString::number(workshopPage_));
  const QString needle = query.trimmed();
  if (needle.isEmpty()) {
    queryItems.addQueryItem(QStringLiteral("browsesort"), QStringLiteral("trend"));
  } else {
    queryItems.addQueryItem(QStringLiteral("browsesort"), QStringLiteral("textsearch"));
    queryItems.addQueryItem(QStringLiteral("searchtext"), needle);
    queryItems.addQueryItem(QStringLiteral("childpublishedfileid"), QStringLiteral("0"));
  }
  for (const auto &tag : {QStringLiteral("Everyone"), QStringLiteral("Questionable"),
                          QStringLiteral("Mature"), QStringLiteral("Adult")}) {
    if (!workshopRatings_.contains(tag.toLower())) {
      queryItems.addQueryItem(QStringLiteral("excludedtags[]"), tag);
    }
  }
  url.setQuery(queryItems);

  QNetworkRequest request(url);
  request.setHeader(QNetworkRequest::UserAgentHeader,
                    QStringLiteral("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
                                   "(KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36"));
  request.setRawHeader(QByteArrayLiteral("Accept"),
                       QByteArrayLiteral("text/html,application/xhtml+xml;q=0.9,*/*;q=0.8"));
  request.setRawHeader(QByteArrayLiteral("Accept-Language"), QByteArrayLiteral("en-US,en;q=0.9"));
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::NoLessSafeRedirectPolicy);

  auto *reply = http_.get(request);
  workshopBrowseReply_ = reply;
  connect(reply, &QNetworkReply::finished, this, [this, reply, gen] {
    if (workshopBrowseReply_ == reply) workshopBrowseReply_.clear();
    reply->deleteLater();
    if (gen != workshopSearchGen_) return;
    if (reply->error() != QNetworkReply::NoError) {
      if (reply->error() == QNetworkReply::OperationCanceledError) return;
      setWorkshopBusy(false);
      setWorkshopError(QStringLiteral("Steam Community: %1").arg(reply->errorString()));
      return;
    }
    const QString html = QString::fromUtf8(reply->readAll());
    QRegularExpression re(QStringLiteral("sharedfiles/filedetails/\\?id=(\\d+)"));
    QStringList ids;
    QSet<QString> seen;
    auto it = re.globalMatch(html);
    while (it.hasNext()) {
      const QString id = it.next().captured(1);
      if (seen.contains(id)) continue;
      seen.insert(id);
      ids << id;
      if (ids.size() >= 30) break;
    }
    workshopPageCount_ = parseSteamWorkshopPages(html, workshopPage_, ids.size());
    emit workshopPageChanged();
    if (ids.isEmpty()) {
      setWorkshopBusy(false);
      setWorkshopError(QStringLiteral("Sin resultados en esta página."));
      return;
    }
    fetchWorkshopDetails(ids, gen);
  });
}

void RpcClient::fetchWorkshopDetails(const QStringList &ids, int gen) {
  QUrl url(QStringLiteral(
      "https://api.steampowered.com/ISteamRemoteStorage/GetPublishedFileDetails/v1/"));
  QUrlQuery body;
  body.addQueryItem(QStringLiteral("itemcount"), QString::number(ids.size()));
  for (int i = 0; i < ids.size(); ++i) {
    body.addQueryItem(QStringLiteral("publishedfileids[%1]").arg(i), ids.at(i));
  }
  QNetworkRequest request(url);
  request.setHeader(QNetworkRequest::ContentTypeHeader,
                    QStringLiteral("application/x-www-form-urlencoded;charset=UTF-8"));
  request.setHeader(QNetworkRequest::UserAgentHeader,
                    QStringLiteral("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
                                   "(KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36"));

  auto *reply = http_.post(request, body.query(QUrl::FullyEncoded).toUtf8());
  workshopDetailsReply_ = reply;
  connect(reply, &QNetworkReply::finished, this, [this, reply, gen] {
    if (workshopDetailsReply_ == reply) workshopDetailsReply_.clear();
    reply->deleteLater();
    if (gen != workshopSearchGen_) return;
    setWorkshopBusy(false);
    if (reply->error() != QNetworkReply::NoError) {
      if (reply->error() == QNetworkReply::OperationCanceledError) return;
      setWorkshopError(QStringLiteral("Steam API: %1").arg(reply->errorString()));
      return;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(reply->readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
      setWorkshopError(QStringLiteral("Respuesta inválida de Steam."));
      return;
    }
    const auto details = document.object()
                             .value(QStringLiteral("response"))
                             .toObject()
                             .value(QStringLiteral("publishedfiledetails"))
                             .toArray();
    QVariantList rows;
    for (const auto &value : details) {
      const auto object = value.toObject();
      const auto id = object.value(QStringLiteral("publishedfileid")).toVariant().toString();
      if (id.isEmpty()) continue;
      QString type = QStringLiteral("static");
      QString rating = ratingFromTagList(object.value(QStringLiteral("tags")).toArray());
      for (const auto &tagValue : object.value(QStringLiteral("tags")).toArray()) {
        const QString tag = (tagValue.isObject()
                                 ? tagValue.toObject().value(QStringLiteral("tag")).toString()
                                 : tagValue.toString())
                                .toLower();
        if (tag.contains(QLatin1String("scene"))) type = QStringLiteral("scene");
        else if (tag.contains(QLatin1String("video"))) type = QStringLiteral("video");
        else if (tag.contains(QLatin1String("web"))) type = QStringLiteral("web");
      }
      if (!workshopRatings_.contains(rating)) continue;
      QVariantMap row;
      row.insert(QStringLiteral("id"), id);
      row.insert(QStringLiteral("title"),
                 object.value(QStringLiteral("title")).toString(id));
      row.insert(QStringLiteral("preview"),
                 object.value(QStringLiteral("preview_url")).toString());
      row.insert(QStringLiteral("type"), type);
      row.insert(QStringLiteral("rating"), rating);
      row.insert(QStringLiteral("fileSize"),
                 object.value(QStringLiteral("file_size")).toVariant().toLongLong());
      const bool installed = !findInstalledWorkshopItem(id).isEmpty() ||
                             !catalog_.itemById(QStringLiteral("steam:") + id).isEmpty();
      row.insert(QStringLiteral("installed"), installed);
      rows.append(row);
    }
    if (rows.isEmpty()) {
      setWorkshopError(QStringLiteral("Sin resultados con esos filtros en esta página."));
      return;
    }
    workshopItems_ = rows;
    emit workshopItemsChanged();
  });
}

void RpcClient::connectDaemon() {
  if (socket_.state() == QLocalSocket::ConnectedState ||
      socket_.state() == QLocalSocket::ConnectingState) {
    return;
  }
  const auto path = socketPath();
  if (path.isEmpty()) {
    setStatusLine(QStringLiteral("XDG_RUNTIME_DIR no está definido"));
    return;
  }
  socket_.connectToServer(path);
}

void RpcClient::disconnectDaemon(const QString &reason) {
  pending_.clear();
  bootstrapping_ = false;
  setApplying(false);
  statusPoll_.stop();
  if (socket_.state() != QLocalSocket::UnconnectedState) {
    socket_.abort();
  }
  setOnline(false, reason.isEmpty() ? QStringLiteral("Sin daemon") : reason);
}

void RpcClient::send(const QString &method, const QJsonObject &params, Reply reply) {
  if (socket_.state() != QLocalSocket::ConnectedState) {
    if (reply) reply({}, QStringLiteral("anis-paperd no está conectado"));
    return;
  }
  const qint64 id = nextId_++;
  if (reply) pending_.insert(id, std::move(reply));
  QJsonObject request{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
  if (!params.isEmpty()) request.insert("params", params);
  socket_.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
}

void RpcClient::flushLines() {
  buffer_ += socket_.readAll();
  while (true) {
    const auto newline = buffer_.indexOf('\n');
    if (newline < 0) break;
    const auto line = buffer_.left(newline);
    buffer_.remove(0, newline + 1);
    if (line.trimmed().isEmpty()) continue;
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) continue;
    handleObject(document.object());
  }
}

void RpcClient::handleObject(const QJsonObject &object) {
  if (object.contains("id") && pending_.contains(object.value("id").toInteger())) {
    const auto id = object.value("id").toInteger();
    const auto reply = pending_.take(id);
    if (object.contains("error")) {
      const auto error = object.value("error").toObject();
      reply({}, error.value("message").toString());
      return;
    }
    reply(object.value("result"), {});
    return;
  }
  const auto method = object.value("method").toString();
  if (method == "catalog.changed") {
    send("catalog.list", {}, [this](const QJsonValue &result, const QString &) {
      if (result.isArray()) catalog_.setItems(result.toArray());
    });
  } else if (method.startsWith(QLatin1String("wallpaper."))) {
    send("status.get", {}, [this](const QJsonValue &result, const QString &) {
      if (!result.isObject()) return;
      const auto renderers = result.toObject().value("renderers").toArray();
      setStatusLine(QStringLiteral("%1 renderer(s) activos").arg(renderers.size()));
    });
    send("monitor.list", {}, [this](const QJsonValue &result, const QString &) {
      fillMonitors(result, &monitors_, &monitorNames_);
      emit monitorsChanged();
    });
  }
}

void RpcClient::bootstrap() {
  if (bootstrapping_) return;
  bootstrapping_ = true;
  send("events.subscribe", {}, {});
  send("catalog.list", {}, [this](const QJsonValue &result, const QString &error) {
    bootstrapping_ = false;
    if (!error.isEmpty()) {
      setToast(error);
      return;
    }
    const auto items = result.toArray();
    catalog_.setItems(items);
    QString preferred;
    for (const auto &value : items) {
      const auto id = value.toObject().value("id").toString();
      const auto title = value.toObject().value("title").toString();
      if (id == QLatin1String("steam:3732575168") ||
          title.contains(QLatin1String("Starlight Anis"), Qt::CaseInsensitive)) {
        preferred = id;
        break;
      }
    }
    if (selectedId_.isEmpty()) {
      setSelectedId(preferred.isEmpty() && !items.isEmpty()
                        ? items.first().toObject().value("id").toString()
                        : preferred);
    }
    const QString ready = findInstalledWorkshopItem(QStringLiteral("3786237445"));
    if (!ready.isEmpty()) {
      send("catalog.addFolder", {{"path", QFileInfo(ready).absolutePath()}}, {});
    }
  });
  send("monitor.list", {}, [this](const QJsonValue &result, const QString &) {
    fillMonitors(result, &monitors_, &monitorNames_);
    emit monitorsChanged();
    if (selectedOutput_.isEmpty() && !monitorNames_.isEmpty()) {
      setSelectedOutput(monitorNames_.contains(QStringLiteral("DP-2"))
                            ? QStringLiteral("DP-2")
                            : monitorNames_.first());
    }
  });
  send("settings.get", {}, [this](const QJsonValue &result, const QString &) {
    if (!result.isObject()) return;
    const auto object = result.toObject();
    QStringList favorites;
    for (const auto &value : object.value("favorites").toArray()) {
      favorites << value.toString();
    }
    catalog_.setFavorites(favorites);
    const int volume = qBound(0, qRound(object.value("defaultVolume").toDouble() * 100.0), 100);
    if (volumePercent_ != volume) {
      volumePercent_ = volume;
      emit volumePercentChanged();
    }
    const int fps = qBound(1, object.value("fpsCap").toInt(60), 60);
    if (fpsCap_ != fps) {
      fpsCap_ = fps;
      emit fpsCapChanged();
    }
    const QString mode = object.value("gamingMode").toString(QStringLiteral("auto"));
    if (!mode.isEmpty() && gamingMode_ != mode) {
      gamingMode_ = mode;
      emit gamingModeChanged();
    }
    QStringList blacklist;
    for (const auto &value : object.value("gamingBlacklist").toArray()) {
      const QString pattern = value.toString().trimmed().toLower();
      if (pattern.size() >= 2 && pattern.size() <= 128 && !blacklist.contains(pattern)) {
        blacklist << pattern;
      }
      if (blacklist.size() >= 64) break;
    }
    if (gamingBlacklist_ != blacklist) {
      gamingBlacklist_ = blacklist;
      emit gamingBlacklistChanged();
    }
  });
  send("status.get", {}, [this](const QJsonValue &result, const QString &) {
    if (!result.isObject()) return;
    const auto catalog = result.toObject().value("catalog").toObject();
    setStatusLine(QStringLiteral("%1 wallpapers · daemon listo")
                      .arg(catalog.value("items").toInt()));
  });
}

void RpcClient::setOnline(bool online, const QString &line) {
  if (online_ != online) {
    online_ = online;
    emit onlineChanged();
  }
  setStatusLine(line);
}

void RpcClient::setToast(const QString &message) {
  if (toast_ == message) return;
  toast_ = message;
  emit toastChanged();
}

void RpcClient::setStatusLine(const QString &line) {
  if (statusLine_ == line) return;
  statusLine_ = line;
  emit statusLineChanged();
}

void RpcClient::setApplying(bool applying) {
  if (applying_ == applying) return;
  applying_ = applying;
  emit applyingChanged();
}
