#include "../common/wayland_monitor.h"
#include "../renderers/renderer_child.h"
#include "../renderers/renderer_manager.h"

#include <QCoreApplication>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSaveFile>
#include <QSocketNotifier>
#include <QThreadPool>
#include <QTimer>
#include <QPointer>
#include <QThread>
#include <QSet>
#include <QHash>

#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <optional>
#include <cmath>

namespace {
constexpr qsizetype kMaxRequestInput = 1024 * 1024;
constexpr qsizetype kMaxResponse = 4 * 1024 * 1024;
constexpr qint64 kMaxPending = 4LL * 1024 * 1024;
constexpr int kMaxConnections = 32;

QString configHome() { auto v=qEnvironmentVariable("XDG_CONFIG_HOME"); return v.isEmpty()?QDir::homePath()+"/.config":v; }
QString settingsPath() { return configHome()+"/anispaper/settings.json"; }
QString runtimeDir() { return qEnvironmentVariable("XDG_RUNTIME_DIR"); }
QString socketPath() { return runtimeDir()+"/anispaper.sock"; }
bool isWithin(const QString &root, const QString &path) { return path==root || path.startsWith(root+QDir::separator()); }
bool integerJson(const QJsonValue &v, int low, int high) { if(!v.isDouble()) return false; const double n=v.toDouble(); return std::isfinite(n) && std::floor(n)==n && n>=low && n<=high; }
bool validScaleMode(const QString &value) {
  return value == QStringLiteral("cover") || value == QStringLiteral("fit") ||
         value == QStringLiteral("stretch");
}
QString canonicalDir(const QString &p) { QFileInfo i(p); return i.isDir() ? i.canonicalFilePath() : QString(); }
QString filesystemIdentity(const QString &path) { struct stat st{}; const auto bytes=path.toUtf8(); return stat(bytes.constData(),&st)==0 ? QString::number(static_cast<qulonglong>(st.st_dev))+":"+QString::number(static_cast<qulonglong>(st.st_ino)) : QString(); }
QString hashId(const QString &prefix, const QString &s) { return prefix+QString::fromLatin1(QCryptographicHash::hash(s.toUtf8(),QCryptographicHash::Sha256).toHex()); }

struct Settings {
  QStringList customFolders;
  QStringList favorites;
  int fpsCap=30;
  double defaultVolume=1.;
  int retryQuota=3;
  QString wallpaperScaleMode=QStringLiteral("cover");
  bool corrupt=false;
};
QJsonObject asJson(const Settings&s) {
  QJsonArray roots;
  for(const auto&x:s.customFolders) roots.append(x);
  QJsonArray favorites;
  for(const auto&x:s.favorites) favorites.append(x);
  return {{"customFolders",roots},
          {"favorites",favorites},
          {"fpsCap",s.fpsCap},
          {"defaultVolume",s.defaultVolume},
          {"retryQuota",s.retryQuota},
          {"wallpaper", QJsonObject{{"scaleMode", s.wallpaperScaleMode}}}};
}

class SettingsStore {
 public:
  Settings load() {
    Settings s; QFile f(settingsPath()); if(!f.exists()) return s;
    if(!f.open(QIODevice::ReadOnly) || f.size()>5*1024*1024) return bad("settings unreadable or oversized");
    QJsonParseError e; auto d=QJsonDocument::fromJson(f.readAll(),&e); if(e.error!=QJsonParseError::NoError || !d.isObject()) return bad("settings corrupt: "+e.errorString());
    auto o=d.object(); const QSet<QString> known{"customFolders","favorites","fpsCap","defaultVolume","retryQuota","wallpaper"}; for(auto it=o.begin();it!=o.end();++it) if(!known.contains(it.key())) return bad("settings has unknown fields");
    if(!o.value("customFolders").isArray() || !integerJson(o.value("fpsCap"),1,240) || !o.value("defaultVolume").isDouble() || o.value("defaultVolume").toDouble()<0 || o.value("defaultVolume").toDouble()>1 || !integerJson(o.value("retryQuota"),0,10)) return bad("settings values invalid");
    if (o.contains("favorites") && !o.value("favorites").isArray()) return bad("settings favorites invalid");
    if (o.contains("wallpaper")) {
      const auto wallpaper=o.value("wallpaper");
      if(!wallpaper.isObject()) return bad("settings wallpaper invalid");
      const auto object=wallpaper.toObject();
      const auto mode=object.value("scaleMode");
      if(object.size()!=1 || !mode.isString() || !validScaleMode(mode.toString())) return bad("settings wallpaper scaleMode invalid");
      s.wallpaperScaleMode=mode.toString();
    }
    for(auto v:o.value("customFolders").toArray()) { if(!v.isString()) return bad("settings customFolders invalid"); auto c=canonicalDir(v.toString()); if(!c.isEmpty() && !s.customFolders.contains(c)) s.customFolders<<c; }
    for(auto v:o.value("favorites").toArray()) {
      if(!v.isString()) return bad("settings favorites invalid");
      const auto id=v.toString().trimmed();
      if(id.isEmpty() || id.size()>512) return bad("settings favorites invalid");
      if(!s.favorites.contains(id)) s.favorites<<id;
      if(s.favorites.size()>10000) return bad("settings favorites invalid");
    }
    s.fpsCap=o.value("fpsCap").toInt(); s.defaultVolume=o.value("defaultVolume").toDouble(); s.retryQuota=o.value("retryQuota").toInt();
    return s;
  }
  bool save(const Settings&s, QString *error) {
    static bool testFailureConsumed=false;
    if(qEnvironmentVariable("ANISPAPER_TEST_FAIL_SAVE_ONCE")=="1"&&!testFailureConsumed) { testFailureConsumed=true; *error="test forced save failure"; return false; }
    QDir().mkpath(QFileInfo(settingsPath()).absolutePath()); QSaveFile f(settingsPath());
    if(!f.open(QIODevice::WriteOnly)) { *error=f.errorString(); return false; }
    if(f.write(QJsonDocument(asJson(s)).toJson(QJsonDocument::Compact))<0 || !f.commit()) { *error=f.errorString(); return false; } return true;
  }
 private: Settings bad(const QString&m) { qWarning().noquote()<<m<<"; using defaults without overwriting file"; Settings s; s.corrupt=true; return s; }
};

struct ScanResult { QJsonArray items; QStringList watchDirs; QStringList vdfParsePaths; QStringList vdfWatchPaths; QHash<QString,QSet<QString>> watchFilters; };
QString safeChild(const QString &root, const QString &relative) {
  if(relative.isEmpty() || QDir::isAbsolutePath(relative)) return {};
  const QString lexical=QDir::cleanPath(root+"/"+relative);
  if(!isWithin(root,lexical)) return {};
  QFileInfo target(lexical);
  if(target.exists()) { const QString canonical=target.canonicalFilePath(); return !canonical.isEmpty()&&isWithin(root,canonical) ? canonical : QString(); }
  // A missing declared asset (notably Wallpaper Engine scene.json) remains
  // meaningful metadata.  It is safe only if every existing ancestor resolves
  // inside the project root; a broken symlink is never accepted as an ancestor.
  QString ancestor=lexical;
  while(!QFileInfo(ancestor).exists()) {
    QFileInfo probe(ancestor);
    if(probe.isSymLink()) return {};
    const QString parent=probe.absoluteDir().absolutePath();
    if(parent==ancestor) return {};
    ancestor=parent;
  }
  const QString canonicalAncestor=QFileInfo(ancestor).canonicalFilePath();
  return !canonicalAncestor.isEmpty()&&isWithin(root,canonicalAncestor) ? lexical : QString();
}
QString inferType(QString type, const QString &file) { type=type.trimmed().toLower(); if(type=="video"||type=="web"||type=="scene"||type=="application") return type; auto e=QFileInfo(file).suffix().toLower(); if(e=="mp4"||e=="webm"||e=="mkv") return "video"; if(e=="html"||e=="htm") return "web"; return "unknown"; }
QJsonObject projectItem(const QString &root, const QString &projectPath, const QString &id, const QString &source) {
  if(QFileInfo(projectPath).isSymLink()) return {};
  QFile f(projectPath); if(!f.open(QIODevice::ReadOnly)||f.size()>5*1024*1024) return {}; QJsonParseError pe; auto doc=QJsonDocument::fromJson(f.readAll(),&pe); if(pe.error!=QJsonParseError::NoError||!doc.isObject()) return {}; auto p=doc.object();
  auto file=p.value("file").toString(); auto preview=p.value("preview").toString(); auto general=p.value("general").toObject(); auto props=general.value("properties");
  auto sf=safeChild(root,file); if(!file.isEmpty()&&sf.isEmpty()) return {}; auto sp=preview.isEmpty()?QString():safeChild(root,preview); if(!preview.isEmpty()&&sp.isEmpty()) preview={};
  QJsonArray tags; for(auto t:p.value("tags").toArray()) if(t.isString()) tags.append(t);
  return {{"id",id},{"title",p.value("title").toString(QFileInfo(root).fileName())},{"type",inferType(p.value("type").toString(),file)},{"file",sf},{"preview",sp},{"tags",tags},{"properties",props.isObject()?props:QJsonValue(QJsonObject{})},{"source",source},{"root",root}};
}
QStringList configuredVdfPaths() {
  QStringList out; auto e=qEnvironmentVariable("ANISPAPER_STEAM_VDF"); if(!e.isEmpty()) out<<e;
  else { const auto home=QDir::homePath(); out<<home+"/.steam/steam/steamapps/libraryfolders.vdf"<<home+"/.local/share/Steam/steamapps/libraryfolders.vdf"; }
  const auto extra=qEnvironmentVariable("ANISPAPER_TEST_EXTRA_VDF"); if(!extra.isEmpty()) out<<extra;
  QStringList lexical;
  for(const auto &path:out) lexical<<QDir::cleanPath(QFileInfo(path).absoluteFilePath());
  lexical.removeDuplicates(); return lexical;
}
QStringList vdfPaths() {
  const auto out=configuredVdfPaths();
  QStringList unique; QSet<QString> identities;
  for(const auto &path:out) { QFileInfo fi(path); if(!fi.exists()||!fi.isFile()) continue; const auto canonical=fi.canonicalFilePath(); const auto identity=filesystemIdentity(canonical); if(canonical.isEmpty()||identity.isEmpty()||identities.contains(identity)) continue; identities.insert(identity); unique<<canonical; }
  return unique;
}
QStringList librariesFromVdf(const QString &vdf) {
  QFile f(vdf); if(!f.open(QIODevice::ReadOnly)||f.size()>16*1024*1024) return {}; QString t=QString::fromUtf8(f.readAll()); QStringList out;
  QRegularExpression rx("\\\"path\\\"\\s*\\\"((?:\\\\.|[^\\\"])*)\\\"",QRegularExpression::CaseInsensitiveOption); auto it=rx.globalMatch(t); while(it.hasNext()) { auto s=it.next().captured(1); s.replace("\\\\","\\"); auto c=canonicalDir(s); if(!c.isEmpty()&&!out.contains(c)) out<<c; }
  auto steamapps=QFileInfo(vdf).absoluteDir().absolutePath(); auto lib=QFileInfo(steamapps).absoluteDir().absolutePath(); auto c=canonicalDir(lib); if(!c.isEmpty()&&!out.contains(c)) out.prepend(c); return out;
}
void addMissingContentWatch(const QString &library, ScanResult &r) {
  QString current=library;
  const QStringList components{"steamapps","workshop","content","431960"};
  for(const auto &component:components) {
    const QString next=current+"/"+component;
    if(QFileInfo(next).isDir()) { const auto canonical=canonicalDir(next); if(canonical.isEmpty()) return; current=canonical; continue; }
    const auto ancestor=canonicalDir(current); if(ancestor.isEmpty()) return;
    r.watchDirs<<ancestor; r.watchFilters[ancestor].insert(component); return;
  }
}
void addMissingPathWatch(const QString &target, QStringList &watchDirs, QHash<QString,QSet<QString>> &filters) {
  QString probe=QDir::cleanPath(QFileInfo(target).absoluteFilePath());
  QString child=QFileInfo(probe).fileName();
  while(!QFileInfo(probe).exists()) { const QString parent=QFileInfo(probe).absoluteDir().absolutePath(); if(parent==probe) return; child=QFileInfo(probe).fileName(); probe=parent; }
  const auto ancestor=canonicalDir(probe); if(ancestor.isEmpty()||child.isEmpty()) return;
  watchDirs<<ancestor; filters[ancestor].insert(child);
}
ScanResult scan(const Settings &settings) {
  ScanResult r; QSet<QString> ids, projectFiles, contentRoots; auto append=[&](QJsonObject x){ if(!x.isEmpty()&&!ids.contains(x.value("id").toString())) { ids.insert(x.value("id").toString()); if(!x.value("file").toString().isEmpty()) projectFiles.insert(x.value("file").toString()); r.items.append(x); } };
  r.vdfWatchPaths=configuredVdfPaths();
  for(const auto&vdf:vdfPaths()) { if(!QFileInfo::exists(vdf)) continue; r.vdfParsePaths<<vdf; for(const auto&lib:librariesFromVdf(vdf)) { QString content=lib+"/steamapps/workshop/content/431960"; auto cc=canonicalDir(content); if(cc.isEmpty()) { addMissingContentWatch(lib,r); continue; } if(contentRoots.contains(cc)) continue; contentRoots.insert(cc); r.watchDirs<<cc; QDir d(cc); for(auto n:d.entryList(QDir::Dirs|QDir::NoDotAndDotDot,QDir::Name)) { if(!n.contains(QRegularExpression("^\\d+$"))) continue; const QFileInfo entry(cc+"/"+n); if(entry.isSymLink()) continue; auto root=canonicalDir(entry.absoluteFilePath()); if(root.isEmpty()||!isWithin(cc,root)||canonicalDir(QFileInfo(root).absolutePath())!=cc) continue; r.watchDirs<<root; auto project=root+"/project.json"; if(QFileInfo(project).isFile()) append(projectItem(root,project,"steam:"+n,"steam")); } } }
  for(const auto&custom:settings.customFolders) { auto root=canonicalDir(custom); if(root.isEmpty()) continue; r.watchDirs<<root; QDirIterator dirs(root,QDir::Dirs|QDir::NoDotAndDotDot,QDirIterator::Subdirectories); while(dirs.hasNext()) { auto d=canonicalDir(dirs.next()); if(!d.isEmpty()&&isWithin(root,d))r.watchDirs<<d; } QDirIterator it(root,{"project.json"},QDir::Files,QDirIterator::Subdirectories); while(it.hasNext()) { auto pj=it.next(); auto pRoot=canonicalDir(QFileInfo(pj).absolutePath()); if(!pRoot.isEmpty()&&isWithin(root,pRoot)) { r.watchDirs<<pRoot; append(projectItem(pRoot,pj,hashId("custom:",pRoot),"custom")); } } QDirIterator videos(root,{"*.mp4","*.webm","*.mkv"},QDir::Files,QDirIterator::Subdirectories); while(videos.hasNext()) { auto f=videos.next(); auto cf=QFileInfo(f).canonicalFilePath(); if(cf.isEmpty()||!isWithin(root,cf)||projectFiles.contains(cf)) continue; append(QJsonObject{{"id",hashId("custom:",cf)},{"title",QFileInfo(cf).completeBaseName()},{"type","video"},{"file",cf},{"preview",QString()},{"tags",QJsonArray{}},{"properties",QJsonObject{}},{"source","custom"},{"root",root}}); } }
  r.watchDirs.removeDuplicates(); r.vdfParsePaths.removeDuplicates(); r.vdfWatchPaths.removeDuplicates(); return r;
}

class Watcher : public QObject {
 public:
  struct Update { bool armedChanged=false; int failures=0; };
  struct Record { QString path; QSet<QString> childFilters; };
  explicit Watcher(std::function<void()> changed, QObject *parent=nullptr):QObject(parent), changed_(std::move(changed)) { fd_=inotify_init1(IN_NONBLOCK|IN_CLOEXEC); if(fd_>=0) notifier_=new QSocketNotifier(fd_,QSocketNotifier::Read,this), connect(notifier_,&QSocketNotifier::activated,this,&Watcher::read); timer_.setSingleShot(true); timer_.setInterval(500); connect(&timer_,&QTimer::timeout,this,[this]{changed_();}); }
  ~Watcher(){ if(fd_>=0) ::close(fd_); }
  Update setPaths(const QStringList &paths, const QHash<QString,QSet<QString>> &requestedFilters={}) { Update result; if(fd_<0)return result; QHash<QString,QSet<QString>> wanted; QHash<QString,QString> canonicalForIdentity; for(const auto &raw:paths) { QFileInfo fi(raw); const auto canonical=fi.canonicalFilePath(); const auto identity=canonical.isEmpty()?QString():filesystemIdentity(canonical); if(canonical.isEmpty()||identity.isEmpty()) continue; const auto filters=requestedFilters.value(canonical); if(canonicalForIdentity.contains(identity)) { const auto chosen=canonicalForIdentity.value(identity); if(filters.isEmpty()) wanted[chosen].clear(); else if(!wanted.value(chosen).isEmpty()) wanted[chosen].unite(filters); continue; } canonicalForIdentity.insert(identity,canonical); wanted.insert(canonical,filters); } for(auto it=paths_.begin();it!=paths_.end();) { if(!wanted.contains(it.value().path)) { if(inotify_rm_watch(fd_,it.key())==0) { suppress_.insert(it.key()); result.armedChanged=true; } it=paths_.erase(it); } else { it.value().childFilters=wanted.value(it.value().path); ++it; } } QStringList toAdd; for(auto it=wanted.cbegin();it!=wanted.cend();++it) { bool exists=false; for(const auto &record:paths_) if(record.path==it.key()){exists=true;break;} if(!exists) toAdd<<it.key(); } const int delay=qEnvironmentVariableIntValue("ANISPAPER_TEST_DELAY_BEFORE_WATCH_MS"); if(!toAdd.isEmpty()&&delay>0) { const QString marker=qEnvironmentVariable("ANISPAPER_TEST_WATCH_ARM_MARKER"); if(!marker.isEmpty()) { QFile f(marker); if(f.open(QIODevice::WriteOnly)) f.write("arming\n"); } QThread::msleep(static_cast<unsigned long>(delay)); } constexpr uint32_t m=IN_CREATE|IN_DELETE|IN_MOVED_TO|IN_MOVED_FROM|IN_CLOSE_WRITE|IN_IGNORED|IN_DELETE_SELF|IN_MOVE_SELF; for(const auto&p:toAdd) { auto b=p.toUtf8(); int wd=inotify_add_watch(fd_,b.constData(),m); if(wd>=0) { if(paths_.contains(wd)) { /* Same kernel object/WD: retain the established canonical mapping. */ } else { paths_.insert(wd,{p,wanted.value(p)}); result.armedChanged=true; } } else ++result.failures; } lastFailures_=result.failures; return result; }
  bool active() const { return fd_>=0; }
  int watchCount() const { return paths_.size(); }
  int failures() const { return lastFailures_; }
 private:
  void read(){ char buf[8192]; bool changed=false; ssize_t n; while((n=::read(fd_,buf,sizeof(buf)))>0) { for(char *p=buf;p<buf+n;) { auto *e=reinterpret_cast<inotify_event*>(p); const bool ignored=e->mask&IN_IGNORED; const bool own=ignored&&suppress_.contains(e->wd); const auto record=paths_.value(e->wd); bool relevant=true; if(e->len>0&&!record.childFilters.isEmpty()) { const QByteArray rawName(e->name,static_cast<int>(e->len)); const QString name=QString::fromUtf8(rawName.constData(),rawName.indexOf('\0')); relevant=record.childFilters.contains(name); } if(ignored) { if(own) suppress_.remove(e->wd); else paths_.remove(e->wd); } if(relevant&&!own) changed=true; p+=sizeof(inotify_event)+e->len; } } if(changed) timer_.start(); }
  int fd_=-1; QSocketNotifier *notifier_{}; QHash<int,Record> paths_; QSet<int> suppress_; QTimer timer_; std::function<void()> changed_; int lastFailures_=0;
};

class Daemon;
struct Client { QLocalSocket *socket{}; QByteArray in; bool subscribed=false; qint64 queued=0; };
class Daemon : public QObject {
 public:
  Daemon() : watcher_([this] { refreshAsync(); }, this) {
    renderers_ = &RendererManager::instance(this);
    connect(renderers_, &RendererManager::wallpaperActive, this,
            [this](const QJsonObject &event) { broadcast("wallpaper.active", event); });
    connect(renderers_, &RendererManager::wallpaperStopped, this,
            [this](const QJsonObject &event) { broadcast("wallpaper.stopped", event); });
    connect(renderers_, &RendererManager::wallpaperCrash, this,
            [this](const QJsonObject &event) { broadcast("wallpaper.crashed", event); });
    connect(renderers_, &RendererManager::wallpaperSafeMode, this,
            [this](const QJsonObject &event) { broadcast("wallpaper.safeMode", event); });
    settings_ = store_.load();
    refreshAsync();
  }

  ~Daemon() {
    // Disconnect callbacks first: a shutdown abort must not run the normal
    // disconnected lambda after the Client record has been reclaimed.
    for (auto *client : clients_) {
      QObject::disconnect(client->socket, nullptr, this, nullptr);
      client->socket->abort();
      delete client->socket;
      delete client;
    }
    clients_.clear();
    pool_.waitForDone();
    cleanSocket();
  }

  bool start(QString *error) {
    if (runtimeDir().isEmpty()) {
      *error = "XDG_RUNTIME_DIR is required";
      return false;
    }
    QDir().mkpath(runtimeDir());
    const auto path = socketPath();
    struct stat existing {};
    if (lstat(path.toUtf8().constData(), &existing) == 0) {
      if (!S_ISSOCK(existing.st_mode) || existing.st_uid != geteuid()) {
        *error = "refusing unsafe existing socket path";
        return false;
      }
      inodeDev_ = existing.st_dev;
      inode_ = existing.st_ino;
      QLocalSocket probe;
      probe.connectToServer(path);
      if (probe.waitForConnected(150)) {
        *error = "anispaper daemon already active";
        return false;
      }
      if (::unlink(path.toUtf8().constData()) != 0) {
        *error = QString::fromLocal8Bit(strerror(errno));
        return false;
      }
    }
    server_.setSocketOptions(QLocalServer::UserAccessOption);
    if (!server_.listen(path)) {
      *error = server_.errorString();
      return false;
    }
    struct stat ours {};
    if (lstat(path.toUtf8().constData(), &ours) == 0) {
      inodeDev_ = ours.st_dev;
      inode_ = ours.st_ino;
      chmod(path.toUtf8().constData(), S_IRUSR | S_IWUSR);
    }
    connect(&server_, &QLocalServer::newConnection, this, &Daemon::accept);
    return true;
  }

  void refreshAsync() {
    if (scanning_) {
      queuedRefresh_ = true;
      return;
    }
    scanning_ = true;
    ++generation_;
    const Settings copy = settings_;
    QPointer<Daemon> self(this);
    pool_.start([self, copy] {
      auto result = scan(copy);
      if (self) {
        QMetaObject::invokeMethod(
            self,
            [self, result = std::move(result)]() mutable {
              if (!self) return;
              self->applyScan(std::move(result));
              self->scanning_ = false;
              if (self->queuedRefresh_) {
                self->queuedRefresh_ = false;
                self->refreshAsync();
              }
            },
            Qt::QueuedConnection);
      }
    });
  }
 private:
  void applyScan(ScanResult result) {
    QList<QJsonValue> ordered;
    for (const auto &value : result.items) ordered << value;
    std::sort(ordered.begin(), ordered.end(), [](const QJsonValue &left,
                                                  const QJsonValue &right) {
      return left.toObject().value("id").toString() <
             right.toObject().value("id").toString();
    });
    result.items = QJsonArray{};
    for (const auto &value : ordered) result.items.append(value);

    QHash<QString, QByteArray> oldItems;
    QHash<QString, QByteArray> newItems;
    for (const auto value : catalog_) {
      oldItems.insert(value.toObject().value("id").toString(),
                      QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
    }
    for (const auto value : result.items) {
      newItems.insert(value.toObject().value("id").toString(),
                      QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
    }
    QJsonArray added;
    QJsonArray removed;
    for (auto it = newItems.cbegin(); it != newItems.cend(); ++it) {
      if (!oldItems.contains(it.key()) || oldItems.value(it.key()) != it.value()) {
        added.append(it.key());
      }
    }
    for (auto it = oldItems.cbegin(); it != oldItems.cend(); ++it) {
      if (!newItems.contains(it.key()) || newItems.value(it.key()) != it.value()) {
        removed.append(it.key());
      }
    }
    catalog_ = result.items;
    QStringList watchPaths = result.watchDirs;
    auto filters = result.watchFilters;
    for (const auto &vdf : result.vdfWatchPaths) {
      const auto parent = canonicalDir(QFileInfo(vdf).absolutePath());
      if (!parent.isEmpty()) {
        watchPaths << parent;
        filters[parent].insert(QFileInfo(vdf).fileName());
      } else {
        addMissingPathWatch(vdf, watchPaths, filters);
      }
      if (QFileInfo(vdf).isFile()) watchPaths << vdf;
    }
    const auto update = watcher_.setPaths(watchPaths, filters);
    if (update.armedChanged) {
      QTimer::singleShot(75, this, [this] { refreshAsync(); });
    }
    if (!added.isEmpty() || !removed.isEmpty()) {
      broadcast("catalog.changed", {{"added", added}, {"removed", removed}});
    }
  }

  void accept() {
    while (server_.hasPendingConnections()) {
      auto *socket = server_.nextPendingConnection();
      if (clients_.size() >= kMaxConnections) {
        socket->disconnectFromServer();
        socket->deleteLater();
        continue;
      }
      auto *client = new Client{socket, {}, false, 0};
      clients_ << client;
      connect(socket, &QLocalSocket::readyRead, this,
              [this, client] { readClient(client); });
      connect(socket, &QLocalSocket::bytesWritten, this, [client](qint64 bytes) {
        client->queued = std::max<qint64>(0, client->queued - bytes);
      });
      connect(socket, &QLocalSocket::disconnected, this, [this, client] {
        clients_.removeAll(client);
        client->socket->deleteLater();
        delete client;
      });
    }
  }

  void readClient(Client *client) {
    client->in += client->socket->readAll();
    if (client->in.size() > kMaxRequestInput && !client->in.contains('\n')) {
      client->socket->disconnectFromServer();
      return;
    }
    while (true) {
      const auto newline = client->in.indexOf('\n');
      if (newline < 0) break;
      const QByteArray line = client->in.left(newline);
      client->in.remove(0, newline + 1);
      if (line.size() > kMaxRequestInput) {
        client->socket->disconnectFromServer();
        return;
      }
      handle(client, line);
    }
  }

  void write(Client *client, const QJsonObject &object) {
    const QByteArray bytes =
        QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    const qint64 pending = std::max(client->queued, client->socket->bytesToWrite());
    if (bytes.size() > kMaxResponse || pending + bytes.size() > kMaxPending) {
      client->socket->disconnectFromServer();
      return;
    }
    client->queued += bytes.size();
    client->socket->write(bytes);
  }

  void response(Client *client, const QJsonValue &id, const QJsonValue &result) {
    write(client, {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
  }

  void error(Client *client, const QJsonValue &id, int code, const QString &message) {
    write(client, {{"jsonrpc", "2.0"},
                   {"id", id},
                   {"error", QJsonObject{{"code", code}, {"message", message}}}});
  }

  void broadcast(const QString &method, const QJsonObject &params) {
    for (auto *client : clients_) {
      if (client->subscribed) {
        write(client, {{"jsonrpc", "2.0"}, {"method", method}, {"params", params}});
      }
    }
  }

  QJsonObject catalogItem(const QString &id) const {
    for (const auto value : catalog_) {
      const auto item = value.toObject();
      if (item.value("id").toString() == id) return item;
    }
    return {};
  }

  QJsonArray monitors() const {
    QJsonArray result;
    for (const auto value : listWaylandOutputs()) {
      QJsonObject output = value.toObject();
      output.insert("currentWallpaperId",
                    renderers_->wallpaperId(output.value("name").toString()));
      result.append(output);
    }
    return result;
  }

  QJsonObject daemonStatus() const {
    const QJsonObject renderStatus = renderers_->status();
    return {{"phase", "F4 scene fallback + F3 SHM bridge"},
            {"catalog", QJsonObject{{"items", catalog_.size()},
                                     {"scanning", scanning_},
                                     {"generation", static_cast<qint64>(generation_)}}},
            {"watch", QJsonObject{{"backend", "inotify"},
                                   {"active", watcher_.active()},
                                   {"count", watcher_.watchCount()},
                                   {"failures", watcher_.failures()}}},
            {"renderers", renderStatus.value("outputs")},
            {"watchdog", renderStatus.value("watchdog")},
            {"socket", QJsonObject{{"path", socketPath()},
                                    {"connections", clients_.size()}}}};
  }

  void handle(Client *client, const QByteArray &line) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || document.isArray() ||
        !document.isObject()) {
      error(client, QJsonValue::Null, parseError.error == QJsonParseError::NoError
                                           ? -32600
                                           : -32700,
            parseError.error == QJsonParseError::NoError ? "invalid request"
                                                         : "parse error");
      return;
    }
    const QJsonObject request = document.object();
    const bool hasId = request.contains("id");
    const QJsonValue id = request.value("id");
    const bool validId = !hasId || id.isNull() || id.isString() || id.isDouble();
    if (request.value("jsonrpc") != "2.0" || !request.value("method").isString() ||
        !validId) {
      error(client, QJsonValue::Null, -32600, "invalid request");
      return;
    }
    const QString method = request.value("method").toString();
    const QJsonValue params = request.value("params");
    const auto reply = [&](const QJsonValue &result) {
      if (hasId) response(client, id, result);
    };
    const auto fail = [&](int code, const QString &message) {
      if (hasId) error(client, id, code, message);
    };
    const auto requireObject = [&] {
      if (!params.isUndefined() && !params.isObject()) {
        fail(-32602, "invalid params");
        return false;
      }
      return true;
    };

    if (method == "catalog.list") {
      if (!requireObject()) return;
      reply(catalog_);
      return;
    }
    if (method == "catalog.refresh") {
      if (!requireObject()) return;
      refreshAsync();
      reply(QJsonObject{{"scheduled", true}, {"count", catalog_.size()},
                        {"generation", static_cast<qint64>(generation_)}});
      return;
    }
    if (method == "monitor.list") {
      if (!requireObject()) return;
      reply(monitors());
      return;
    }
    if (method == "events.subscribe") {
      if (!requireObject()) return;
      client->subscribed = true;
      reply(QJsonObject{{"subscribed", true}});
      return;
    }
    if (method == "settings.get") {
      if (!requireObject()) return;
      reply(asJson(settings_));
      return;
    }
    if (method == "status.get") {
      if (!requireObject()) return;
      reply(daemonStatus());
      return;
    }
    if (method == "catalog.addFolder") {
      if (!params.isObject() || !params.toObject().value("path").isString()) {
        fail(-32602, "invalid params");
        return;
      }
      const auto path = canonicalDir(params.toObject().value("path").toString());
      if (path.isEmpty()) {
        fail(-32602, "path must be an existing directory");
        return;
      }
      Settings candidate = settings_;
      const bool added = !candidate.customFolders.contains(path);
      if (added) candidate.customFolders << path;
      QString saveError;
      if (!store_.save(candidate, &saveError)) {
        fail(-32603, "settings write failed: " + saveError);
        return;
      }
      settings_ = candidate;
      settings_.corrupt = false;
      refreshAsync();
      reply(QJsonObject{{"path", path}, {"added", added}});
      return;
    }
    if (method == "settings.set") {
      if (!params.isObject()) {
        fail(-32602, "invalid params");
        return;
      }
      const auto patch = params.toObject();
      if (patch.isEmpty()) {
        fail(-32602, "empty patch");
        return;
      }
      for (auto it = patch.begin(); it != patch.end(); ++it) {
        if (it.key() != "favorites" && it.key() != "fpsCap" && it.key() != "defaultVolume" &&
            it.key() != "retryQuota" && it.key() != "wallpaper.scaleMode" &&
            it.key() != "wallpaper") {
          fail(-32602, "unknown or immutable setting");
          return;
        }
      }
      if (patch.contains("wallpaper.scaleMode") && patch.contains("wallpaper")) {
        fail(-32602, "wallpaper scaleMode specified twice");
        return;
      }
      Settings next = settings_;
      if (patch.contains("favorites")) {
        const auto values = patch.value("favorites");
        if (!values.isArray()) {
          fail(-32602, "favorites must be an array of wallpaper ids");
          return;
        }
        QStringList favorites;
        for (const auto &value : values.toArray()) {
          if (!value.isString()) {
            fail(-32602, "favorites must be an array of wallpaper ids");
            return;
          }
          const auto favorite = value.toString().trimmed();
          if (favorite.isEmpty() || favorite.size() > 512) {
            fail(-32602, "favorites contains an invalid wallpaper id");
            return;
          }
          if (!favorites.contains(favorite)) favorites << favorite;
          if (favorites.size() > 10000) {
            fail(-32602, "favorites exceeds 10000 wallpaper ids");
            return;
          }
        }
        next.favorites = favorites;
      }
      if (patch.contains("fpsCap")) {
        if (!integerJson(patch.value("fpsCap"), 1, 240)) {
          fail(-32602, "fpsCap must be 1..240");
          return;
        }
        next.fpsCap = patch.value("fpsCap").toInt();
      }
      if (patch.contains("defaultVolume")) {
        const auto value = patch.value("defaultVolume");
        if (!value.isDouble() || value.toDouble() < 0 || value.toDouble() > 1) {
          fail(-32602, "defaultVolume must be 0..1");
          return;
        }
        next.defaultVolume = value.toDouble();
      }
      if (patch.contains("retryQuota")) {
        if (!integerJson(patch.value("retryQuota"), 0, 10)) {
          fail(-32602, "retryQuota must be 0..10");
          return;
        }
        next.retryQuota = patch.value("retryQuota").toInt();
      }
      if (patch.contains("wallpaper.scaleMode") || patch.contains("wallpaper")) {
        QJsonValue mode;
        if (patch.contains("wallpaper.scaleMode")) {
          mode = patch.value("wallpaper.scaleMode");
        } else {
          const auto wallpaper=patch.value("wallpaper");
          if (!wallpaper.isObject() || wallpaper.toObject().size()!=1 ||
              !wallpaper.toObject().contains("scaleMode")) {
            fail(-32602, "wallpaper must contain only scaleMode");
            return;
          }
          mode=wallpaper.toObject().value("scaleMode");
        }
        if (!mode.isString() || !validScaleMode(mode.toString())) {
          fail(-32602, "wallpaper.scaleMode must be cover, fit or stretch");
          return;
        }
        next.wallpaperScaleMode=mode.toString();
      }
      QString saveError;
      if (!store_.save(next, &saveError)) {
        fail(-32603, "settings write failed: " + saveError);
        return;
      }
      settings_ = next;
      settings_.corrupt = false;
      reply(asJson(settings_));
      return;
    }
    if (method == "wallpaper.apply") {
      if (!params.isObject()) {
        fail(-32602, "invalid params");
        return;
      }
      const auto requestParams = params.toObject();
      const QString idValue = requestParams.value("id").toString();
      const QString output = requestParams.value("output").toString();
      if (idValue.isEmpty() || output.isEmpty()) {
        fail(-32602, "id and output are required");
        return;
      }
      const QJsonObject item = catalogItem(idValue);
      if (item.isEmpty()) {
        fail(-32602, "unknown wallpaper id");
        return;
      }
      RendererOptions options{qBound(1, settings_.fpsCap, 60), settings_.defaultVolume,
                              settings_.wallpaperScaleMode};
      QString applyError;
      int applyCode = -32001;
      if (!renderers_->apply(item, output, options, &applyError, &applyCode)) {
        fail(applyCode, applyError);
        return;
      }
      reply(QJsonObject{{"id", idValue}, {"output", output},
                        {"safeMode", renderers_->safeMode(output)}});
      return;
    }
    if (method == "wallpaper.stop") {
      if (!params.isObject() || !params.toObject().value("output").isString() ||
          params.toObject().value("output").toString().trimmed().isEmpty()) {
        fail(-32602, "output is required");
        return;
      }
      reply(renderers_->stop(params.toObject().value("output").toString()));
      return;
    }
    if (method == "preview.frame") {
      if (!params.isObject() || !params.toObject().value("output").isString() ||
          params.toObject().value("output").toString().trimmed().isEmpty()) {
        fail(-32602, "output is required");
        return;
      }
      const QString output = params.toObject().value("output").toString();
      const QImage frame = renderers_->lastFrame(output);
      if (frame.isNull()) {
        fail(-32001, "renderer unavailable");
        return;
      }
      QByteArray jpeg;
      QBuffer buffer(&jpeg);
      if (!buffer.open(QIODevice::WriteOnly) || !frame.save(&buffer, "JPEG", 85)) {
        fail(-32001, "renderer unavailable");
        return;
      }
      reply(QJsonObject{{"mimeType", "image/jpeg"},
                        {"data", QString::fromLatin1(jpeg.toBase64())},
                        {"width", frame.width()},
                        {"height", frame.height()},
                        {"safeMode", renderers_->safeMode(output)}});
      return;
    }
    fail(-32601, "method not found");
  }

  void cleanSocket() {
    if (inode_ == 0) return;
    struct stat current {};
    const auto path = socketPath().toUtf8();
    if (lstat(path.constData(), &current) == 0 && current.st_dev == inodeDev_ &&
        current.st_ino == inode_) {
      ::unlink(path.constData());
    }
  }

  SettingsStore store_;
  Settings settings_;
  QJsonArray catalog_;
  QLocalServer server_;
  QList<Client *> clients_;
  Watcher watcher_;
  QThreadPool pool_;
  RendererManager *renderers_ = nullptr;
  bool scanning_ = false;
  bool queuedRefresh_ = false;
  quint64 generation_ = 0;
  dev_t inodeDev_{};
  ino_t inode_{};
};
}
int main(int argc, char **argv) {
  if (isRendererChildInvocation(argc, argv)) {
    return runRendererChild(argc, argv);
  }
  QCoreApplication app(argc, argv);
  Daemon daemon;
  QString error;
  if (!daemon.start(&error)) {
    fprintf(stderr, "anis-paperd: %s\n", error.toUtf8().constData());
    return 1;
  }
  return app.exec();
}
