#!/usr/bin/env python3
"""F1 end-to-end fixture. Exit 77 only when this host forbids AF_UNIX bind."""
import json, os, shutil, socket, subprocess, sys, tempfile, time

daemon=sys.argv[1]; root=tempfile.mkdtemp(prefix="anispaper-f1-",dir="/tmp")
runtime,config=root+"/runtime",root+"/config"; os.makedirs(runtime); os.makedirs(config)
lib1,lib2,lib3,lib4,alias=root+"/one",root+"/two",root+"/three",root+"/four",root+"/one-alias"; vdf=root+"/libraryfolders.vdf"; vdf_alias=root+"/libraryfolders-alias.vdf"; include_third=False; include_fourth=False
def touch(path): os.makedirs(os.path.dirname(path),exist_ok=True); open(path,"ab").close()
def project(lib,wid,body,files=()):
 d=f"{lib}/steamapps/workshop/content/431960/{wid}";os.makedirs(d,exist_ok=True)
 for n in files:touch(d+"/"+n)
 with open(d+"/project.json","w") as f:json.dump(body,f)
 return d
def writevdf(target=vdf):
 global include_third,include_fourth
 paths=[lib1,alias,lib2]+([lib3] if include_third else [])+([lib4] if include_fourth else [])
 with open(target+".new","w") as f:f.write('"libraryfolders" { '+" ".join('"%d" { "path" "%s" }'%(i,p) for i,p in enumerate(paths))+' }')
 os.replace(target+".new",target)
project(lib1,"10",{"title":"Upper","type":"VIDEO","file":"movie.webm","preview":"preview.jpg","general":{"properties":[]}},("movie.webm","preview.jpg"))
project(lib1,"11",{"title":"Unknown","file":"plain.bin"},("plain.bin",))
project(lib1,"12",{"title":"Escape","file":"../escape.webm"})
project(lib1,"14",{"title":"Declared missing scene","type":"SCENE","file":"future/scene.json"})
project(lib1,"18",{"title":"Large response","file":"large.bin","general":{"properties":{"blob":"x"*(1200*1024)}}},("large.bin",))
project(lib2,"10",{"title":"Second duplicate","file":"movie.webm"},("movie.webm",));os.symlink(lib1,alias);writevdf();os.link(vdf,vdf_alias)
project(lib3,"30",{"title":"VDF replacement","file":"vdf.webm"},("vdf.webm",))
external_project=root+"/external-project";os.makedirs(external_project);touch(external_project+"/outside.webm")
with open(external_project+"/project.json","w") as f:json.dump({"title":"Must not escape","file":"outside.webm"},f)
os.symlink(external_project,lib1+"/steamapps/workshop/content/431960/99")
linked_project=project(lib1,"19",{"title":"Will be symlinked","file":"movie.webm"},("movie.webm",))
os.unlink(linked_project+"/project.json");os.symlink(external_project+"/project.json",linked_project+"/project.json")
# This directory exists before daemon startup, so the content-root IN_CREATE
# watch cannot rescue the race below.
prearmed=f"{lib1}/steamapps/workshop/content/431960/22";os.makedirs(prearmed)
# Existing or broken links outside the project root must reject the item.
outside=root+"/outside.webm";touch(outside)
bad=project(lib1,"15",{"title":"External link","file":"link.webm"});os.symlink(outside,bad+"/link.webm")
bad=project(lib1,"16",{"title":"Broken link","file":"link.webm"});os.symlink(root+"/does-not-exist",bad+"/link.webm")
bad=project(lib1,"17",{"title":"External parent","file":"parent/missing.webm"});os.symlink(root,bad+"/parent")
marker=runtime+"/watch-arm.marker"
env=os.environ|{"XDG_RUNTIME_DIR":runtime,"XDG_CONFIG_HOME":config,"ANISPAPER_STEAM_VDF":vdf,"ANISPAPER_TEST_EXTRA_VDF":vdf_alias,"ANISPAPER_TEST_DELAY_BEFORE_WATCH_MS":"500","ANISPAPER_TEST_WATCH_ARM_MARKER":marker,"ANISPAPER_TEST_FAIL_SAVE_ONCE":"1"}
run=root+"/anis-paperd";shutil.copy2(daemon,run);os.chmod(run,0o700)
p=None
class Rpc:
 def __init__(self): self.s=socket.socket(socket.AF_UNIX);self.s.connect(runtime+"/anispaper.sock");self.s.settimeout(2);self.buf=b""
 def line(self):
  while b"\n" not in self.buf:self.buf+=self.s.recv(65536)
  line,self.buf=self.buf.split(b"\n",1);return json.loads(line)
 def send(self,o,fragment=False):
  b=json.dumps(o,separators=(",",":"),ensure_ascii=False).encode()+b"\n"
  if fragment:self.s.sendall(b[:3]);self.s.sendall(b[3:])
  else:self.s.sendall(b)
 def call(self,o,fragment=False): self.send(o,fragment);return None if "id" not in o else self.line()
 def quiet(self,seconds=.2):
  self.s.settimeout(seconds)
  try:self.s.recv(1);return False
  except TimeoutError:return True
  finally:self.s.settimeout(2)
def wait_catalog(r, predicate, seconds=2):
 end=time.monotonic()+seconds
 while time.monotonic()<end:
  x=r.call({"jsonrpc":"2.0","id":"list","method":"catalog.list"})["result"]
  if predicate(x):return x
  time.sleep(.05)
 raise AssertionError("catalog condition timeout")
def wait_stable(r, seconds=5, window=1):
 """Require a continuous quiet window, not an arbitrary point after an event."""
 end=time.monotonic()+seconds; quiet_since=None; stable_generation=None; saw_failure=False; last=[]
 while time.monotonic()<end:
  status=r.call({"jsonrpc":"2.0","id":"status","method":"status.get"})["result"]
  catalog,status_watch=status["catalog"],status["watch"]
  generation=catalog["generation"]; scanning=catalog["scanning"]
  saw_failure = saw_failure or status_watch.get("failures",0)>0
  last.append((generation,scanning,status_watch.get("failures",0)))
  last=last[-8:]
  if scanning:
   quiet_since=None;stable_generation=None
  elif stable_generation != generation:
   quiet_since=time.monotonic();stable_generation=generation
  elif time.monotonic()-quiet_since >= window:
   return stable_generation,saw_failure
  time.sleep(.1)
 raise AssertionError("catalog did not stabilize; recent generation/scanning/failures="+repr(last))
try:
 p=subprocess.Popen([run],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
 end=time.monotonic()+3
 while not os.path.exists(runtime+"/anispaper.sock") and time.monotonic()<end:time.sleep(.02)
 if not os.path.exists(runtime+"/anispaper.sock"):
  out,err=p.communicate(timeout=1);detail=(out+err).decode(errors="replace")
  if "Unknown error 1" in detail: print("f1_integration: skipped (sandbox forbids AF_UNIX bind)");sys.exit(77)
  raise AssertionError("socket startup: "+detail)
 # Wait until scan has produced a watch set and is deliberately paused before
 # installing it, then write only inside the directory that predated startup.
 end=time.monotonic()+3
 while not os.path.exists(marker) and time.monotonic()<end: time.sleep(.01)
 assert os.path.exists(marker), "watch-arm marker"
 touch(prearmed+"/prearm.webm")
 with open(prearmed+"/project.json","w") as f: json.dump({"title":"Pre-arm race","file":"prearm.webm"},f)
 r=Rpc();other=Rpc()
 # Startup scan, deterministic duplicate and parser/type/traversal contract.
 items=wait_catalog(r,lambda a:len(a)==5); by={x["id"]:x for x in items}
 assert set(by)=={"steam:10","steam:11","steam:14","steam:18","steam:22"} and "steam:99" not in by and "steam:19" not in by and by["steam:10"]["title"]=="Upper"
 assert by["steam:10"]["type"]=="video" and by["steam:10"]["properties"]=={} and by["steam:11"]["type"]=="unknown" and by["steam:14"]["type"]=="scene"
 assert len(json.dumps(items)) > 1024*1024, "catalog response was not >1MiB"
 # addFolder must not mutate memory/catalog when atomic persistence fails.
 failed_custom=root+"/failed-custom";os.makedirs(failed_custom)
 before_fail=r.call({"jsonrpc":"2.0","id":40,"method":"status.get"})["result"]["catalog"]["generation"]
 failed=r.call({"jsonrpc":"2.0","id":41,"method":"catalog.addFolder","params":{"path":failed_custom}})
 assert failed["error"]["code"]==-32603
 assert failed_custom not in r.call({"jsonrpc":"2.0","id":42,"method":"settings.get"})["result"]["customFolders"]
 time.sleep(.15);assert r.call({"jsonrpc":"2.0","id":43,"method":"status.get"})["result"]["catalog"]["generation"]==before_fail
 # The VDF parent is filtered: unrelated neighbours must not schedule a scan.
 before_neighbour,_=wait_stable(r)
 neighbour=root+"/not-libraryfolders.vdf";touch(neighbour);os.unlink(neighbour)
 after_neighbour,_=wait_stable(r)
 assert after_neighbour==before_neighbour, "neighbour VDF-parent activity caused a rescan"
 # Fragmented and coalesced frames; linear reader must retain both lines.
 r.send({"jsonrpc":"2.0","id":"str","method":"settings.get"},True);r.send({"jsonrpc":"2.0","id":7,"method":"settings.get"})
 assert {r.line()["id"],r.line()["id"]}=={"str",7}
 assert r.call({"jsonrpc":"2.0","id":None,"method":"settings.get"})["id"] is None
 assert r.call({"jsonrpc":"2.0","id":1,"method":"settings.get"})["result"]["fpsCap"]==30
 r.call({"jsonrpc":"2.0","method":"settings.get"});assert r.quiet()
 r.send({"jsonrpc":"1.0"}); invalid=r.line();assert invalid["id"] is None and invalid["error"]["code"]==-32600
 for bad_id in (True,{},[]):
  invalid=r.call({"jsonrpc":"2.0","id":bad_id,"method":"settings.get"})
  assert invalid["id"] is None and invalid["error"]["code"]==-32600
 # Standard errors and exact F2 boundary.
 r.send([{"jsonrpc":"2.0"}]); assert r.line()["error"]["code"]==-32600
 r.s.sendall(b"{bad}\n");assert r.line()["error"]["code"]==-32700
 assert r.call({"jsonrpc":"2.0","id":2,"method":"missing"})["error"]["code"]==-32601
 assert r.call({"jsonrpc":"2.0","id":3,"method":"monitor.list","params":[]})["error"]["code"]==-32602
 assert r.call({"jsonrpc":"2.0","id":4,"method":"wallpaper.stop","params":{}})["error"]["code"]==-32602
 assert r.call({"jsonrpc":"2.0","id":5,"method":"settings.set","params":{"fpsCap":61}})["result"]["fpsCap"]==61
 assert r.call({"jsonrpc":"2.0","id":6,"method":"settings.set","params":{"fpsCap":1.5}})["error"]["code"]==-32602
 assert r.call({"jsonrpc":"2.0","id":7,"method":"settings.set","params":{"retryQuota":1.2}})["error"]["code"]==-32602
 assert r.call({"jsonrpc":"2.0","id":8,"method":"settings.set","params":{"bad":1}})["error"]["code"]==-32602
 settings=r.call({"jsonrpc":"2.0","id":81,"method":"settings.get"})["result"]
 assert settings["wallpaper"]["scaleMode"]=="cover"
 settings=r.call({"jsonrpc":"2.0","id":82,"method":"settings.set","params":{"wallpaper.scaleMode":"fit"}})["result"]
 assert settings["wallpaper"]["scaleMode"]=="fit"
 settings=r.call({"jsonrpc":"2.0","id":83,"method":"settings.set","params":{"wallpaper":{"scaleMode":"stretch"}}})["result"]
 assert settings["wallpaper"]["scaleMode"]=="stretch"
 assert r.call({"jsonrpc":"2.0","id":84,"method":"settings.set","params":{"wallpaper.scaleMode":"invalid"}})["error"]["code"]==-32602
 # Only the subscribing connection receives watcher push.
 assert r.call({"jsonrpc":"2.0","id":9,"method":"events.subscribe"})["result"]["subscribed"]
 delayed=f"{lib1}/steamapps/workshop/content/431960/13";os.makedirs(delayed);time.sleep(.65);touch(delayed+"/late.mp4");project(lib1,"13",{"title":"Late","file":"late.mp4"},("late.mp4",))
 event=r.line();assert event["method"]=="catalog.changed" and "steam:13" in event["params"]["added"]
 assert other.quiet(),"non-subscriber received event"
 # Same ID update preserves event contract; delete is visible.
 with open(delayed+"/project.json","w") as f:json.dump({"title":"Updated","file":"late.mp4"},f)
 event=r.line();assert "steam:13" in event["params"]["added"] and "steam:13" in event["params"]["removed"]
 shutil.rmtree(delayed);event=r.line();assert "steam:13" in event["params"]["removed"]
 # Parent VDF watch and file-watch rearm: an atomic distinct replacement adds
 # then removes a third physical library item.
 include_third=True;writevdf();event=r.line();assert "steam:30" in event["params"]["added"]
 include_third=False;writevdf();event=r.line();assert "steam:30" in event["params"]["removed"]
 # The configured hardlink alias is watched lexically even though its initial
 # inode was deduped for parsing; replacing the alias must be visible too.
 include_third=True;writevdf(vdf_alias);event=r.line();assert "steam:30" in event["params"]["added"]
 include_third=False;writevdf(vdf_alias);event=r.line();assert "steam:30" in event["params"]["removed"]
 # Delete/recreate is not an atomic replacement: both lexical candidates stay
 # parent-watched while absent, catalog empties, then primary restores it.
 os.unlink(vdf);os.unlink(vdf_alias);event=r.line();assert "steam:10" in event["params"]["removed"]
 writevdf();event=r.line();assert "steam:10" in event["params"]["added"]
 # A library with no Steam hierarchy is watched one child component at a time.
 os.makedirs(lib4);include_fourth=True;writevdf();time.sleep(.65)
 hierarchy=lib4+"/steamapps/workshop/content/431960/40";os.makedirs(hierarchy);touch(hierarchy+"/new.webm")
 with open(hierarchy+"/project.json","w") as f:json.dump({"title":"New library","file":"new.webm"},f)
 event=r.line();assert "steam:40" in event["params"]["added"]
 shutil.rmtree(lib4+"/steamapps");event=r.line();assert "steam:40" in event["params"]["removed"]
 # Custom delayed project/video discovery.
 custom=root+"/custom";os.makedirs(custom)
 assert r.call({"jsonrpc":"2.0","id":10,"method":"catalog.addFolder","params":{"path":custom}})["result"]["added"]
 sub=custom+"/delayed";os.makedirs(sub);time.sleep(.65);touch(sub+"/custom.webm")
 with open(sub+"/project.json","w") as f: json.dump({"title":"Custom","file":"custom.webm"},f)
 event=r.line();assert event["method"]=="catalog.changed"
 touch(custom+"/loose.mkv");event=r.line();assert event["method"]=="catalog.changed"
 # A directory disappearing in the deliberate scan->add-watch gap must count
 # as a failed watch attempt, not create an automatic rescan loop.
 vanish=f"{lib1}/steamapps/workshop/content/431960/23";os.makedirs(vanish)
 if os.path.exists(marker): os.unlink(marker)
 r.call({"jsonrpc":"2.0","id":92,"method":"catalog.refresh"})
 end=time.monotonic()+3
 while not os.path.exists(marker) and time.monotonic()<end: time.sleep(.01)
 assert os.path.exists(marker), "vanishing watch-arm marker"
 os.rmdir(vanish)
 generation,saw_failure=wait_stable(r)
 assert saw_failure, "disappearing directory did not report a failed add-watch"
 time.sleep(.25)
 after=r.call({"jsonrpc":"2.0","id":94,"method":"status.get"})["result"]
 assert not after["catalog"]["scanning"] and after["catalog"]["generation"]==generation
 # Oversize is fail-closed.
 bad=Rpc();bad.s.sendall(b"x"*(1024*1024+1));time.sleep(.1);assert bad.s.recv(1)==b"";bad.s.close()
 # A second daemon cannot claim an active socket.
 q=subprocess.run([run],env=env,capture_output=True,timeout=2);assert q.returncode!=0
 print("f1_integration: catalog/rpc/settings/events/watcher/socket assertions passed")
finally:
 if p:
  p.terminate()
  try:p.wait(timeout=2)
  except subprocess.TimeoutExpired:p.kill()
 shutil.rmtree(root,ignore_errors=True)
