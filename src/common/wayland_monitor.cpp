#include "wayland_monitor.h"
#include <wayland-client.h>
#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonObject>
#include <QStringList>
#include <algorithm>
#include <chrono>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <vector>
#include <memory>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#undef Unsorted
#undef None
#undef Success
namespace {
struct Output {
  uint32_t global{};
  wl_output *object{};
  QString name;
  // wl_output.geometry uses compositor logical coordinates.  The current
  // wl_output.mode below is deliberately retained separately because its
  // dimensions are physical pixels, including under KDE fractional scaling.
  int x{}, y{};
  int physicalWidth{}, physicalHeight{};
  int bufferScale = 1;
  bool hasCurrentMode = false;
};
struct State { wl_display *display{}; wl_registry *registry{}; std::vector<std::unique_ptr<Output>> outputs; };
void geometry(void *d, wl_output*, int32_t x, int32_t y, int32_t, int32_t, int32_t,const char*,const char*,int32_t) { auto *o=static_cast<Output*>(d); o->x=x; o->y=y; }
void mode(void *d, wl_output*, uint32_t flags, int32_t w, int32_t h, int32_t) {
  // CURRENT is the drm mode actually driving the connector (kscreen '*').
  // PREFERRED on DP-2 is 1280x720 while CURRENT is 1920x1080 -- never use it,
  // and never fall back to geometry which is logical (1423x800 at 135%).
  if (flags & WL_OUTPUT_MODE_CURRENT) {
    auto *o=static_cast<Output*>(d);
    o->physicalWidth=w;
    o->physicalHeight=h;
    o->hasCurrentMode=true;
  }
}
void done(void*, wl_output*) {}
void scale(void *d, wl_output*, int32_t value) {
  auto *o=static_cast<Output*>(d);
  o->bufferScale = std::max(1, static_cast<int>(value));
}
void name(void *d, wl_output*, const char *n) { static_cast<Output*>(d)->name=QString::fromUtf8(n?n:""); }
void description(void*, wl_output*, const char*) {}
const wl_output_listener outputListener{geometry,mode,done,scale,name,description};
void global(void *d, wl_registry *r, uint32_t id, const char *interface, uint32_t version) { auto *s=static_cast<State*>(d); if(QString::fromUtf8(interface)=="wl_output") { auto o=std::make_unique<Output>(); o->global=id; o->object=static_cast<wl_output*>(wl_registry_bind(r,id,&wl_output_interface,std::min(version,4u))); wl_output_add_listener(o->object,&outputListener,o.get()); s->outputs.push_back(std::move(o)); } }
void globalRemove(void*, wl_registry*, uint32_t) {} const wl_registry_listener registryListener{global,globalRemove};
struct Sync { bool done=false; };
void syncDone(void *data, wl_callback *cb, uint32_t) { static_cast<Sync*>(data)->done=true; wl_callback_destroy(cb); }
const wl_callback_listener syncListener{syncDone};
bool boundedRoundtrip(wl_display *display, int timeoutMs) {
  Sync sync; auto *callback=wl_display_sync(display); if(!callback) return false;
  wl_callback_add_listener(callback,&syncListener,&sync);
  auto fail=[&]{ if(!sync.done) wl_callback_destroy(callback); return false; };
  if(wl_display_flush(display)<0 && errno!=EAGAIN) return fail();
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeoutMs);
  while(!sync.done) {
    if(wl_display_prepare_read(display)==0) {
      const auto left=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now()).count();
      if(left<=0) { wl_display_cancel_read(display); return fail(); }
      pollfd p{wl_display_get_fd(display),POLLIN,0}; const int rc=poll(&p,1,static_cast<int>(left));
      if(rc<=0 || !(p.revents&POLLIN)) { wl_display_cancel_read(display); return fail(); }
      if(wl_display_read_events(display)<0) return fail();
    }
    if(wl_display_dispatch_pending(display)<0) return fail();
    if(wl_display_flush(display)<0 && errno!=EAGAIN) return fail();
  }
  return true;
}
}
QJsonArray listWaylandOutputs() {
  const QString runtime=qEnvironmentVariable("XDG_RUNTIME_DIR"); if(runtime.isEmpty()) return {};
  QString display=qEnvironmentVariable("WAYLAND_DISPLAY"); if(display.isEmpty()) { auto c=QDir(runtime).entryList({"wayland-*"},QDir::System|QDir::Files,QDir::Name); c.erase(std::remove_if(c.begin(),c.end(),[&](const QString &n){ struct stat st{}; const auto p=QDir(runtime).filePath(n).toUtf8(); return lstat(p.constData(),&st)!=0 || !S_ISSOCK(st.st_mode) || st.st_uid!=geteuid(); }),c.end()); if(c.isEmpty()) return {}; display=c.first(); }
  State s; s.display=wl_display_connect(display.toUtf8().constData()); if(!s.display) return {}; s.registry=wl_display_get_registry(s.display); wl_registry_add_listener(s.registry,&registryListener,&s);
  if(!boundedRoundtrip(s.display,1000) || !boundedRoundtrip(s.display,1000)) { for(auto&o:s.outputs) if(o->object) wl_output_destroy(o->object); if(s.registry) wl_registry_destroy(s.registry); wl_display_disconnect(s.display); return {}; }
  // Name + CURRENT mode can land after the first two syncs, especially on the
  // second connector.  Missing CURRENT used to leave RendererSpec at 640x480
  // and attach scale_vaapi=w=640:h=480 on that child.
  auto outputsReady = [&] {
    if (s.outputs.empty()) return true;
    for (const auto &o : s.outputs) {
      if (!o->hasCurrentMode || o->physicalWidth <= 0 || o->physicalHeight <= 0) return false;
    }
    return true;
  };
  for (int extra = 0; extra < 8 && !outputsReady(); ++extra) {
    if (!boundedRoundtrip(s.display, 250)) break;
  }
  QJsonArray out;
  for(auto&o:s.outputs) {
    const QJsonObject physical{{"width", o->physicalWidth},
                               {"height", o->physicalHeight}};
    // `geometry` remains source-compatible with F1 clients.  Its x/y are
    // logical desktop positions; width/height are the (always authoritative)
    // physical current mode.  New consumers should use physicalSize/renderSize
    // instead of combining those coordinate systems.
    out.append(QJsonObject{{"name",o->name.isEmpty()?QString("output-%1").arg(o->global):o->name},
                           {"geometry",QJsonObject{{"x",o->x},{"y",o->y},{"width",o->physicalWidth},{"height",o->physicalHeight}}},
                           {"physicalSize", physical},
                           {"renderSize", physical},
                           {"bufferScale", o->bufferScale},
                           {"currentWallpaperId",QJsonValue::Null}});
    if(o->object) wl_output_destroy(o->object);
  }
  if(s.registry) wl_registry_destroy(s.registry);
  wl_display_disconnect(s.display);
  return out;
}

QSize physicalWaylandOutputSize(const QString &outputName) {
  const QString wanted = outputName.trimmed();
  if (wanted.isEmpty()) return {};
  // Two independent connects: the first can race a sibling renderer child's
  // own wl_display_connect and miss CURRENT on one connector.
  for (int attempt = 0; attempt < 2; ++attempt) {
    for (const auto value : listWaylandOutputs()) {
      const QJsonObject output = value.toObject();
      if (output.value(QStringLiteral("name")).toString().compare(
              wanted, Qt::CaseInsensitive) != 0) {
        continue;
      }
      const QJsonObject physical = output.value(QStringLiteral("physicalSize")).toObject();
      const int width = physical.value(QStringLiteral("width")).toInt();
      const int height = physical.value(QStringLiteral("height")).toInt();
      if (width > 0 && height > 0) return {width, height};
    }
  }
  return {};
}

namespace {
QByteArray edidForX11Output(Display *display, RROutput output) {
  const Atom property = XInternAtom(display, "EDID", True);
  if (property == 0) return {};
  Atom actualType = 0;
  int actualFormat = 0;
  unsigned long count = 0;
  unsigned long remaining = 0;
  unsigned char *value = nullptr;
  const int status = XRRGetOutputProperty(display, output, property, 0, 256,
                                           False, False, AnyPropertyType,
                                           &actualType, &actualFormat, &count,
                                           &remaining, &value);
  QByteArray bytes;
  if (status == 0 && actualFormat == 8 && value && count >= 128) {
    bytes = QByteArray(reinterpret_cast<const char *>(value), static_cast<qsizetype>(count));
  }
  if (value) XFree(value);
  return bytes;
}

QJsonArray listX11Outputs() {
  QJsonArray result;
  const QByteArray displayName = qgetenv("DISPLAY");
  if (displayName.isEmpty()) return result;
  Display *display = XOpenDisplay(displayName.constData());
  if (!display) return result;
  XRRScreenResources *resources = XRRGetScreenResourcesCurrent(display, DefaultRootWindow(display));
  if (!resources) {
    XCloseDisplay(display);
    return result;
  }
  for (int index = 0; index < resources->noutput; ++index) {
    const RROutput output = resources->outputs[index];
    XRROutputInfo *info = XRRGetOutputInfo(display, resources, output);
    if (!info) continue;
    if (info->connection == RR_Connected && info->crtc != 0 && info->name && info->nameLen > 0) {
      XRRCrtcInfo *crtc = XRRGetCrtcInfo(display, resources, info->crtc);
      if (crtc && crtc->width > 0 && crtc->height > 0) {
        const QString name = QString::fromUtf8(info->name, info->nameLen);
        const QByteArray edid = edidForX11Output(display, output);
        const QString identity = edid.isEmpty() ? QString() :
            QStringLiteral("edid:") + QString::fromLatin1(
                QCryptographicHash::hash(edid, QCryptographicHash::Sha256).toHex());
        QJsonObject row{{QStringLiteral("name"), name},
                        {QStringLiteral("geometry"), QJsonObject{
                            {QStringLiteral("x"), crtc->x}, {QStringLiteral("y"), crtc->y},
                            {QStringLiteral("width"), static_cast<int>(crtc->width)},
                            {QStringLiteral("height"), static_cast<int>(crtc->height)}}},
                        {QStringLiteral("physicalSize"), QJsonObject{
                            {QStringLiteral("width"), static_cast<int>(crtc->width)},
                            {QStringLiteral("height"), static_cast<int>(crtc->height)}}}};
        if (!identity.isEmpty()) row.insert(QStringLiteral("identity"), identity);
        result.append(row);
      }
      if (crtc) XRRFreeCrtcInfo(crtc);
    }
    XRRFreeOutputInfo(info);
  }
  XRRFreeScreenResources(resources);
  XCloseDisplay(display);
  return result;
}

QString drmEdidIdentity(const QString &requestedName) {
  // Wayland connector names are generally DRM names (DP-1, HDMI-A-1). Match
  // the complete connector suffix, never a numeric suffix or substring.
  const QString suffix = requestedName.section(QLatin1Char('-'), -2);
  QDir drm(QStringLiteral("/sys/class/drm"));
  const QStringList paths = drm.entryList({QStringLiteral("card*-*")},
                                           QDir::Dirs | QDir::System, QDir::Name);
  QString match;
  for (const QString &entry : paths) {
    const QString connector = entry.mid(entry.indexOf(QLatin1Char('-')) + 1);
    if (connector != requestedName && connector != suffix) continue;
    QFile file(drm.filePath(entry + QStringLiteral("/edid")));
    if (!file.open(QIODevice::ReadOnly)) continue;
    const QByteArray edid = file.read(4096);
    if (edid.size() < 128) continue;
    const QString identity = QStringLiteral("edid:") + QString::fromLatin1(
        QCryptographicHash::hash(edid, QCryptographicHash::Sha256).toHex());
    if (!match.isEmpty() && match != identity) return {};
    match = identity;
  }
  return match;
}
} // namespace

QString graphicalSessionType() {
  QString type = qEnvironmentVariable("ANISPAPER_SESSION_TYPE").trimmed().toLower();
  if (type.isEmpty()) type = qEnvironmentVariable("XDG_SESSION_TYPE").trimmed().toLower();
  if (type == QStringLiteral("x11") || type == QStringLiteral("wayland")) return type;
  // Do not infer Wayland from a stale socket when X11 is the active session.
  if (!qEnvironmentVariableIsEmpty("DISPLAY") && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))
    return QStringLiteral("x11");
  if (!qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) return QStringLiteral("wayland");
  return {};
}

QJsonArray listSessionOutputs() {
  const QString session = graphicalSessionType();
  if (session == QStringLiteral("x11")) return listX11Outputs();
  if (session == QStringLiteral("wayland")) {
    QJsonArray outputs = listWaylandOutputs();
    for (qsizetype i = 0; i < outputs.size(); ++i) {
      QJsonObject row = outputs.at(i).toObject();
      const QString identity = drmEdidIdentity(row.value(QStringLiteral("name")).toString());
      if (!identity.isEmpty()) row.insert(QStringLiteral("identity"), identity);
      outputs.replace(i, row);
    }
    return outputs;
  }
  return {};
}

QSize physicalSessionOutputSize(const QString &outputName) {
  for (const QJsonValue &value : listSessionOutputs()) {
    const QJsonObject row = value.toObject();
    if (row.value(QStringLiteral("name")).toString() != outputName) continue;
    const QJsonObject size = row.value(QStringLiteral("physicalSize")).toObject();
    const int width = size.value(QStringLiteral("width")).toInt();
    const int height = size.value(QStringLiteral("height")).toInt();
    if (width > 0 && height > 0) return {width, height};
  }
  return {};
}

QString stableOutputIdentity(const QString &outputName) {
  for (const QJsonValue &value : listSessionOutputs()) {
    const QJsonObject row = value.toObject();
    if (row.value(QStringLiteral("name")).toString() == outputName)
      return row.value(QStringLiteral("identity")).toString();
  }
  return {};
}
