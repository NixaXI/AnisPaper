#include "wayland_monitor.h"
#include <wayland-client.h>
#include <QDir>
#include <QJsonObject>
#include <QStringList>
#include <algorithm>
#include <chrono>
#include <poll.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <vector>
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
};
struct State { wl_display *display{}; wl_registry *registry{}; std::vector<std::unique_ptr<Output>> outputs; };
void geometry(void *d, wl_output*, int32_t x, int32_t y, int32_t, int32_t, int32_t,const char*,const char*,int32_t) { auto *o=static_cast<Output*>(d); o->x=x; o->y=y; }
void mode(void *d, wl_output*, uint32_t flags, int32_t w, int32_t h, int32_t) { if (flags & WL_OUTPUT_MODE_CURRENT) { auto *o=static_cast<Output*>(d); o->physicalWidth=w; o->physicalHeight=h; } }
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
  for (const auto value : listWaylandOutputs()) {
    const QJsonObject output = value.toObject();
    if (output.value(QStringLiteral("name")).toString() != wanted) continue;
    const QJsonObject physical = output.value(QStringLiteral("physicalSize")).toObject();
    const int width = physical.value(QStringLiteral("width")).toInt();
    const int height = physical.value(QStringLiteral("height")).toInt();
    if (width > 0 && height > 0) return {width, height};
  }
  return {};
}
