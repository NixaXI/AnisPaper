#include "../src/renderers/renderer_manager.h"
#include "../src/renderers/isolated_renderer.h"

#include <QDBusConnection>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QThread>

#include <csignal>
#include <cstdio>
#include <functional>
#include <unistd.h>

// Run on a private bus: this owns a fake systemd manager, never the user's.
class ManagerEnvironment final : public QObject {
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.freedesktop.systemd1.Manager")
  Q_PROPERTY(QStringList Environment READ environment)
 public:
  QStringList values;
  QStringList environment() const { return values; }
};

bool waitUntil(const std::function<bool()> &predicate, int timeoutMs) {
  QElapsedTimer timer;
  timer.start();
  do {
    QCoreApplication::processEvents();
    if (predicate()) return true;
    QThread::msleep(10);
  } while (timer.elapsed() < timeoutMs);
  return false;
}

void require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "scene_session_recovery: %s\n", message);
    std::exit(1);
  }
}

void writeFile(const QString &path, const QByteArray &bytes) {
  QFile file(path);
  require(file.open(QIODevice::WriteOnly), "could not write fixture");
  require(file.write(bytes) == bytes.size(), "fixture write incomplete");
}

int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QGuiApplication app(argc, argv);
  ManagerEnvironment environment;
  auto bus = QDBusConnection::sessionBus();
  require(bus.registerService(QStringLiteral("org.freedesktop.systemd1")),
          "needs a private D-Bus session");
  require(bus.registerObject(QStringLiteral("/org/freedesktop/systemd1"), &environment,
                             QDBusConnection::ExportAllProperties), "could not export Environment");
  QTemporaryDir fixture;
  require(fixture.isValid(), "temporary directory unavailable");
  const QString engine = fixture.path() + QStringLiteral("/engine");
  const QString record = fixture.path() + QStringLiteral("/child-environment");
  const QString commands = fixture.path() + QStringLiteral("/child-commands");
  const QString auth = fixture.path() + QStringLiteral("/rotated cookie");
  writeFile(auth, "cookie");
  writeFile(engine, "#!/usr/bin/env bash\n"
                    "printf '%s\\n' \"$DISPLAY\" \"$XAUTHORITY\" >> \"$ANISPAPER_SESSION_TEST_RECORD\"\n"
                    "printf '{\"event\":\"ready\"}\\n'\n"
                    "while IFS= read -r line; do printf '%s\\n' \"$line\" >> \"$ANISPAPER_SESSION_TEST_COMMANDS\"; [[ $line == *stop* ]] && exit 0; done\n");
  require(QFile::setPermissions(engine, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner),
          "could not make mock engine executable");
  writeFile(fixture.path() + QStringLiteral("/scene.json"), "{}");
  qputenv("ANISPAPER_SCENE_ENGINE_BIN", engine.toUtf8());
  qputenv("ANISPAPER_SESSION_TEST_RECORD", record.toUtf8());
  qputenv("ANISPAPER_SESSION_TEST_COMMANDS", commands.toUtf8());
  qputenv("ANISPAPER_SESSION_TYPE", "wayland");
  qputenv("XDG_SESSION_TYPE", "wayland");
  qputenv("XDG_SESSION_ID", "fixture-session");
  qputenv("XDG_RUNTIME_DIR", fixture.path().toUtf8());
  qunsetenv("ANISPAPER_RENDERER_RUNTIME_DIR");
  qunsetenv("DISPLAY");
  qunsetenv("WAYLAND_DISPLAY");
  qunsetenv("XAUTHORITY");
  environment.values = {QStringLiteral("XDG_RUNTIME_DIR=") + fixture.path(),
                         QStringLiteral("XDG_SESSION_TYPE=wayland"),
                         QStringLiteral("XDG_SESSION_ID=fixture-session")};
  auto &manager = RendererManager::instance(&app);
  manager.setGamingMode(QStringLiteral("off"));
  const QString output = QStringLiteral("SESSION-TEST-%1").arg(getpid());
  const QJsonObject item{{"id", "fixture-scene"}, {"type", "scene"},
                         {"root", fixture.path()},
                         {"file", fixture.path() + QStringLiteral("/scene.json")}};
  QString error;
  int errorCode = 0;
  require(manager.apply(item, output, RendererOptions{}, &error, &errorCode), "apply failed");
  const auto status = [&] { return manager.status().value("outputs").toArray().at(0).toObject(); };
  require(status().value("state").toString() == "waiting-session", "missing DISPLAY did not defer");
  // Four retries used to exhaust the crash budget and permanently freeze the scene.
  waitUntil([] { return false; }, 21000);
  require(status().value("crashes").toInt() == 0 && !status().value("safeMode").toBool(),
          "login delay consumed renderer crash quota");
  require(!QFile::exists(record), "child started without DISPLAY");
  manager.setGamingMode(QStringLiteral("on"));
  environment.values << QStringLiteral("DISPLAY=:42") << QStringLiteral("XAUTHORITY=") + auth;
  environment.values.replace(2, QStringLiteral("XDG_SESSION_ID=other-session"));
  waitUntil([] { return false; }, 5500);
  require(!QFile::exists(record), "accepted DISPLAY from a different login");
  environment.values.replace(2, QStringLiteral("XDG_SESSION_ID=fixture-session"));
  require(waitUntil([&] { return status().value("state").toString() == "running"; }, 7000),
          "late manager import did not recover the scene");
  require(waitUntil([&] { return QFile::exists(record); }, 1000), "child environment not recorded");
  QFile recorded(record);
  require(recorded.open(QIODevice::ReadOnly), "record unavailable");
  require(recorded.readAll() == (QStringLiteral(":42\n") + auth + QLatin1Char('\n')).toUtf8(),
          "DISPLAY/cookie pair was lost or shell-parsed incorrectly");
  recorded.close();
  require(status().value("crashes").toInt() == 0 && !status().contains("error"), "recovery retained error");
  const auto received = [&](const QByteArray &command) {
    QFile data(commands);
    return data.open(QIODevice::ReadOnly) && data.readAll().contains(command);
  };
  require(status().value("gamingPaused").toBool() &&
          waitUntil([&] { return received("pause"); }, 1000), "recovered scene escaped gaming pause");
  const qint64 resumedPid = static_cast<qint64>(status().value("pid").toDouble());
  manager.setGamingMode(QStringLiteral("off"));
  require(!status().value("gamingPaused").toBool() &&
          waitUntil([&] { return received("resume"); }, 1000), "game exit did not resume recovered scene");
  require(status().value("pid").toDouble() == resumedPid, "resume unexpectedly replaced child");
  // A direct launch's explicit display must survive stale manager data.
  qputenv("DISPLAY", ":43");
  RendererSpec directSpec;
  directSpec.type = QStringLiteral("scene");
  directSpec.root = fixture.path();
  directSpec.file = fixture.path() + QStringLiteral("/scene.json");
  IsolatedRenderer direct(directSpec);
  require(direct.start(&error), "direct launch failed");
  require(waitUntil([&] {
    QFile data(record);
    return data.open(QIODevice::ReadOnly) && data.readAll().contains(":43\n");
  }, 1000), "manager replaced explicit direct DISPLAY");
  direct.stop();
  // Real child exits still count and activate safe mode on the third failure.
  qputenv("DISPLAY", ":42");
  for (int crash = 1; crash <= 3; ++crash) {
    require(waitUntil([&] { return status().value("pid").toDouble() > 0; }, 7000), "restart missing");
    ::kill(static_cast<pid_t>(status().value("pid").toDouble()), SIGKILL);
    require(waitUntil([&] { return status().value("crashes").toInt() == crash; }, 3000),
            "real child crash did not consume quota");
  }
  require(status().value("safeMode").toBool(), "actual crashes bypassed safe mode");
  manager.stop(output);
  std::puts("scene_session_recovery: PASS");
  return 0;
}

#include "scene_session_recovery_test.moc"
