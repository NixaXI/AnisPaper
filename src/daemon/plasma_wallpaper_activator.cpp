#include "plasma_wallpaper_activator.h"

#include <QDBusInterface>
#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusReply>
#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSet>

#include <cmath>
#include <limits>

namespace {
constexpr int kPlasmaCallTimeoutMs = 5000;
constexpr auto kPlasmaPlugin = "org.anispaper.frame";

QString dbusError(const QString &operation, const QDBusMessage &reply) {
  return QStringLiteral("Plasma %1 failed: %2").arg(operation, reply.errorMessage());
}

bool equalVariant(const QVariant &left, const QVariant &right) {
  if (left.metaType().id() == QMetaType::QVariantMap ||
      right.metaType().id() == QMetaType::QVariantMap) {
    if (left.metaType().id() != QMetaType::QVariantMap ||
        right.metaType().id() != QMetaType::QVariantMap) {
      return false;
    }
    const QVariantMap leftMap = left.toMap();
    const QVariantMap rightMap = right.toMap();
    if (leftMap.size() != rightMap.size()) return false;
    for (auto it = leftMap.cbegin(); it != leftMap.cend(); ++it) {
      const auto rightIt = rightMap.constFind(it.key());
      if (rightIt == rightMap.cend() || !equalVariant(it.value(), rightIt.value())) return false;
    }
    return true;
  }
  if (left.metaType().id() == QMetaType::QVariantList ||
      right.metaType().id() == QMetaType::QVariantList) {
    if (left.metaType().id() != QMetaType::QVariantList ||
        right.metaType().id() != QMetaType::QVariantList) {
      return false;
    }
    const QVariantList leftList = left.toList();
    const QVariantList rightList = right.toList();
    if (leftList.size() != rightList.size()) return false;
    for (qsizetype index = 0; index < leftList.size(); ++index) {
      if (!equalVariant(leftList.at(index), rightList.at(index))) return false;
    }
    return true;
  }
  if (left.metaType().id() == qMetaTypeId<QDBusArgument>() ||
      right.metaType().id() == qMetaTypeId<QDBusArgument>()) {
    if (left.metaType().id() != qMetaTypeId<QDBusArgument>() ||
        right.metaType().id() != qMetaTypeId<QDBusArgument>()) {
      return false;
    }
    // Keep these arguments const: QDBusArgument has separate marshalling and
    // demarshalling overloads for beginStructure().  The non-const overload
    // attempts to write and aborts when used on the read-only reply payload.
    const QDBusArgument leftArgument = left.value<QDBusArgument>();
    const QDBusArgument rightArgument = right.value<QDBusArgument>();
    if (leftArgument.currentSignature() != QStringLiteral("(u)") ||
        rightArgument.currentSignature() != QStringLiteral("(u)")) {
      return false;
    }
    quint32 leftValue = 0;
    quint32 rightValue = 0;
    leftArgument.beginStructure();
    leftArgument >> leftValue;
    leftArgument.endStructure();
    rightArgument.beginStructure();
    rightArgument >> rightValue;
    rightArgument.endStructure();
    return leftValue == rightValue;
  }
  return left.metaType() == right.metaType() && left == right;
}

class HelperRunner final : public PlasmaOutputMapRunner {
 public:
  bool run(QString *stdoutText, QString *error) override {
    const QString helper = QCoreApplication::applicationDirPath() +
                           QStringLiteral("/anispaper-plasma-output-map");
    QProcess process;
    process.setProgram(helper);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start();
    if (!process.waitForStarted(kPlasmaCallTimeoutMs)) {
      if (error) *error = QStringLiteral("Plasma output-map helper could not start: %1")
                              .arg(process.errorString());
      return false;
    }
    if (!process.waitForFinished(kPlasmaCallTimeoutMs)) {
      process.kill();
      process.waitForFinished(1000);
      if (error) *error = QStringLiteral("Plasma output-map helper timed out");
      return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
      if (error) {
        *error = QStringLiteral("Plasma output-map helper failed: %1")
                     .arg(QString::fromUtf8(process.readAllStandardError()).trimmed());
      }
      return false;
    }
    if (stdoutText) *stdoutText = QString::fromUtf8(process.readAllStandardOutput());
    return true;
  }
};
}  // namespace

PlasmaDbusTransport::PlasmaDbusTransport(PlasmaOutputMapRunner *mappingRunner)
    : fallbackConnectionName_(QStringLiteral("anispaper-plasma-wallpaper-activator")),
      mappingRunner_(mappingRunner) {
  if (!mappingRunner_) {
    ownedMappingRunner_ = std::make_unique<HelperRunner>();
    mappingRunner_ = ownedMappingRunner_.get();
  }
}

PlasmaDbusTransport::~PlasmaDbusTransport() {
  if (fallbackConnection_) QDBusConnection::disconnectFromBus(fallbackConnectionName_);
}

QDBusConnection PlasmaDbusTransport::connection(QString *error) {
  QDBusConnection result = QDBusConnection::sessionBus();
  if (!result.isConnected() && !fallbackConnection_) {
    const QString runtimeDir = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (!runtimeDir.isEmpty()) {
      fallbackConnection_.emplace(QDBusConnection::connectToBus(
          QStringLiteral("unix:path=") + QDir::cleanPath(runtimeDir + "/bus"),
          fallbackConnectionName_));
    }
  }
  if (!result.isConnected() && fallbackConnection_) result = *fallbackConnection_;
  if (!result.isConnected() && error) {
    *error = QStringLiteral("Plasma D-Bus endpoint is unavailable");
  }
  return result;
}

bool PlasmaDbusTransport::screenMappings(QVector<PlasmaScreenMapping> *mappings,
                                         QString *error) {
  if (!mappings) {
    if (error) *error = QStringLiteral("invalid Plasma screen mapping destination");
    return false;
  }
  QString json;
  if (!mappingRunner_ || !mappingRunner_->run(&json, error)) return false;
  return parseScreenMappings(json, mappings, error);
}

bool PlasmaDbusTransport::parseScreenMappings(const QString &json,
                                              QVector<PlasmaScreenMapping> *mappings,
                                              QString *error) {
  if (!mappings) {
    if (error) *error = QStringLiteral("invalid Plasma screen mapping destination");
    return false;
  }
  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8(), &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isArray()) {
    if (error) *error = QStringLiteral("Plasma output-map helper returned invalid JSON");
    return false;
  }
  QVector<PlasmaScreenMapping> parsed;
  parsed.reserve(document.array().size());
  for (const QJsonValue &value : document.array()) {
    const QJsonObject object = value.toObject();
    const QJsonValue connector = object.value(QStringLiteral("connector"));
    const QJsonValue screenNumber = object.value(QStringLiteral("screenNumber"));
    if (object.size() != 2 || !connector.isString() || !screenNumber.isDouble() ||
        screenNumber.toDouble() < 0 || screenNumber.toDouble() != std::floor(screenNumber.toDouble()) ||
        screenNumber.toDouble() > std::numeric_limits<quint32>::max()) {
      if (error) *error = QStringLiteral("Plasma output-map helper returned invalid mapping entries");
      return false;
    }
    parsed.push_back({connector.toString(), static_cast<quint32>(screenNumber.toDouble())});
  }
  *mappings = parsed;
  return true;
}

bool PlasmaDbusTransport::validateOutputTopology(
    const QStringList &outputOrder, const QVector<PlasmaOutputDescriptor> &screens,
    QVector<PlasmaScreenMapping> *mappings, QString *error) {
  if (!mappings || outputOrder.isEmpty() || outputOrder.size() != screens.size()) {
    if (error) *error = QStringLiteral("KWin output order and Qt screens are not a one-to-one mapping");
    return false;
  }
  QSet<QString> ordered;
  QSet<QString> screenNames;
  for (const QString &connector : outputOrder) {
    if (connector.isEmpty() || ordered.contains(connector)) {
      if (error) *error = QStringLiteral("KWin output order has empty or duplicate connectors");
      return false;
    }
    ordered.insert(connector);
  }
  for (int index = 0; index < screens.size(); ++index) {
    const PlasmaOutputDescriptor &screen = screens[index];
    if (screen.connector.isEmpty() || screenNames.contains(screen.connector) ||
        screen.geometry.isEmpty()) {
      if (error) *error = QStringLiteral("Qt screens have empty, duplicate, or invalid entries");
      return false;
    }
    for (int other = 0; other < index; ++other) {
      if (screen.geometry.intersects(screens[other].geometry)) {
        if (error) *error = QStringLiteral("Qt screen geometry is overlapping or mirrored");
        return false;
      }
    }
    screenNames.insert(screen.connector);
  }
  if (ordered != screenNames) {
    if (error) *error = QStringLiteral("KWin output order and Qt screen connectors differ");
    return false;
  }
  QVector<PlasmaScreenMapping> result;
  result.reserve(outputOrder.size());
  for (qsizetype index = 0; index < outputOrder.size(); ++index) {
    result.push_back({outputOrder.at(index), static_cast<quint32>(index)});
  }
  *mappings = result;
  return true;
}

bool PlasmaDbusTransport::wallpaper(quint32 screenNumber, PlasmaWallpaperState *state,
                                    QString *error) {
  if (!state) {
    if (error) *error = QStringLiteral("invalid wallpaper state destination");
    return false;
  }
  QDBusInterface plasma(QStringLiteral("org.kde.plasmashell"),
                         QStringLiteral("/PlasmaShell"),
                         QStringLiteral("org.kde.PlasmaShell"), connection(error));
  plasma.setTimeout(kPlasmaCallTimeoutMs);
  if (!plasma.isValid()) {
    if (error && error->isEmpty()) *error = QStringLiteral("Plasma D-Bus endpoint is unavailable");
    return false;
  }
  const QDBusReply<QVariantMap> reply = plasma.call(QStringLiteral("wallpaper"), screenNumber);
  if (!reply.isValid()) {
    if (error) {
      const QString detail = reply.error().message().trimmed();
      *error = detail.isEmpty()
                   ? QStringLiteral("Plasma wallpaper read failed without a D-Bus error message")
                   : QStringLiteral("Plasma wallpaper read failed: %1").arg(detail);
    }
    return false;
  }
  const QVariantMap parameters = reply.value();
  const QVariant plugin = parameters.value(QStringLiteral("wallpaperPlugin"));
  if (!plugin.isValid() || plugin.typeId() != QMetaType::QString || plugin.toString().isEmpty()) {
    if (error) *error = QStringLiteral("Plasma wallpaper read lacks wallpaperPlugin");
    return false;
  }
  *state = {plugin.toString(), parameters};
  return true;
}

bool PlasmaDbusTransport::setWallpaper(const QString &plugin, const QVariantMap &parameters,
                                       quint32 screenNumber, QString *error) {
  QDBusInterface plasma(QStringLiteral("org.kde.plasmashell"),
                         QStringLiteral("/PlasmaShell"),
                         QStringLiteral("org.kde.PlasmaShell"), connection(error));
  plasma.setTimeout(kPlasmaCallTimeoutMs);
  if (!plasma.isValid()) {
    if (error && error->isEmpty()) *error = QStringLiteral("Plasma D-Bus endpoint is unavailable");
    return false;
  }
  const QDBusMessage reply = plasma.call(QStringLiteral("setWallpaper"), plugin, parameters,
                                          screenNumber);
  if (reply.type() == QDBusMessage::ErrorMessage) {
    if (error) *error = dbusError(QStringLiteral("setWallpaper"), reply);
    return false;
  }
  return true;
}

PlasmaWallpaperActivator::PlasmaWallpaperActivator(PlasmaWallpaperTransport *transport)
    : transport_(transport) {}

bool PlasmaWallpaperActivator::isCanonicalOutputRequest(const QString &requested) {
  return !requested.isEmpty() && requested == requested.trimmed();
}

QString PlasmaWallpaperActivator::connectedOutputIdentity(
    const QString &requested, const QJsonArray &waylandOutputs) {
  if (!isCanonicalOutputRequest(requested)) return {};
  for (const QJsonValue &value : waylandOutputs) {
    const QString identity = value.toObject().value(QStringLiteral("name")).toString();
    if (!identity.isEmpty() && identity == requested) return identity;
  }
  return {};
}

bool PlasmaWallpaperActivator::mappingsMatchWaylandOutputs(
    const QVector<PlasmaScreenMapping> &mappings, const QJsonArray &waylandOutputs,
    QString *error) {
  if (mappings.isEmpty() || waylandOutputs.isEmpty()) {
    if (error) *error = QStringLiteral("Plasma and Wayland connector mapping is unavailable");
    return false;
  }
  QSet<QString> plasmaConnectors;
  QSet<quint32> plasmaScreens;
  for (const PlasmaScreenMapping &mapping : mappings) {
    if (!isCanonicalOutputRequest(mapping.connector) ||
        plasmaConnectors.contains(mapping.connector) || plasmaScreens.contains(mapping.screenNumber)) {
      if (error) *error = QStringLiteral("Plasma connector mapping is ambiguous or invalid");
      return false;
    }
    plasmaConnectors.insert(mapping.connector);
    plasmaScreens.insert(mapping.screenNumber);
  }
  QSet<QString> waylandConnectors;
  for (const QJsonValue &value : waylandOutputs) {
    if (!value.isObject()) {
      if (error) *error = QStringLiteral("Wayland output inventory is invalid");
      return false;
    }
    const QString connector = value.toObject().value(QStringLiteral("name")).toString();
    if (!isCanonicalOutputRequest(connector) || waylandConnectors.contains(connector)) {
      if (error) *error = QStringLiteral("Wayland output inventory is ambiguous or invalid");
      return false;
    }
    waylandConnectors.insert(connector);
  }
  if (plasmaConnectors != waylandConnectors) {
    if (error) {
      *error = QStringLiteral("Plasma helper connectors do not exactly match daemon Wayland outputs");
    }
    return false;
  }
  return true;
}

bool PlasmaWallpaperActivator::preflight(const QString &output, const QString &scaleMode,
                                         PlasmaActivationPlan *plan, QString *error) {
  if (!plan || !isCanonicalOutputRequest(output) || !validScaleMode(scaleMode)) {
    if (error) *error = QStringLiteral("invalid Plasma activation settings");
    return false;
  }
  QVector<PlasmaScreenMapping> mappings;
  if (!mappingSnapshot(&mappings, error)) return false;
  quint32 screenNumber = 0;
  if (!findUniqueMapping(mappings, output, &screenNumber, error)) return false;

  PlasmaWallpaperState targetBefore;
  if (!transport_->wallpaper(screenNumber, &targetBefore, error)) return false;
  QVector<PlasmaWallpaperState> others;
  for (const PlasmaScreenMapping &mapping : mappings) {
    if (mapping.screenNumber == screenNumber) continue;
    PlasmaWallpaperState state;
    if (!transport_->wallpaper(mapping.screenNumber, &state, error)) return false;
    others.push_back(state);
  }
  *plan = {screenNumber, output, scaleMode, targetBefore, mappings, others};
  return true;
}

bool PlasmaWallpaperActivator::commit(const PlasmaActivationPlan &plan, QString *error) {
  if (!transport_ || !isCanonicalOutputRequest(plan.output) || !validScaleMode(plan.scaleMode)) {
    if (error) *error = QStringLiteral("invalid Plasma activation plan");
    return false;
  }
  QVector<PlasmaScreenMapping> mappings;
  if (!mappingSnapshot(&mappings, error)) return false;
  quint32 screenNumber = 0;
  if (!findUniqueMapping(mappings, plan.output, &screenNumber, error)) return false;
  if (screenNumber != plan.screenNumber || mappings != plan.mappingsBefore) {
    if (error) *error = QStringLiteral("Plasma screen mapping changed before activation");
    return false;
  }

  PlasmaWallpaperState current;
  if (!transport_->wallpaper(screenNumber, &current, error)) return false;
  if (matchesDesiredState(current, plan)) return otherScreensUnchanged(plan, error);

  const PlasmaWallpaperState desired = desiredState(plan);
  if (!transport_->setWallpaper(desired.plugin, desired.parameters, screenNumber, error)) {
    if (error && error->isEmpty()) *error = QStringLiteral("Plasma setWallpaper failed");
    return false;
  }
  PlasmaWallpaperState after;
  QString postError;
  QVector<PlasmaScreenMapping> mappingsAfter;
  const bool postcondition = transport_->wallpaper(screenNumber, &after, &postError) &&
                             matchesDesiredState(after, plan) &&
                             mappingSnapshot(&mappingsAfter, &postError) &&
                             mappingsAfter == plan.mappingsBefore &&
                             otherScreensUnchanged(plan, &postError);
  if (postcondition) return true;

  QString rollbackError;
  const bool rolledBack = restoreTarget(plan, &rollbackError);
  if (error) {
    *error = QStringLiteral("Plasma activation postcondition failed: %1%2")
                 .arg(postError,
                      rolledBack ? QString() : QStringLiteral("; rollback failed: %1").arg(rollbackError));
  }
  return false;
}

bool PlasmaWallpaperActivator::validScaleMode(const QString &scaleMode) {
  return scaleMode == QStringLiteral("cover") || scaleMode == QStringLiteral("fit") ||
         scaleMode == QStringLiteral("stretch");
}

bool PlasmaWallpaperActivator::findUniqueMapping(const QVector<PlasmaScreenMapping> &mappings,
                                                 const QString &output, quint32 *screenNumber,
                                                 QString *error) {
  QVector<quint32> matches;
  for (const PlasmaScreenMapping &mapping : mappings) {
    if (mapping.connector == output) matches.push_back(mapping.screenNumber);
  }
  if (matches.size() != 1) {
    if (error) {
      *error = matches.isEmpty()
                   ? QStringLiteral("no authoritative Plasma screen mapping for output %1").arg(output)
                   : QStringLiteral("multiple authoritative Plasma screen mappings for output %1").arg(output);
    }
    return false;
  }
  *screenNumber = matches.first();
  return true;
}

bool PlasmaWallpaperActivator::mappingSnapshot(QVector<PlasmaScreenMapping> *mappings,
                                               QString *error) {
  if (!transport_) {
    if (error) *error = QStringLiteral("Plasma activation transport is unavailable");
    return false;
  }
  if (!transport_->screenMappings(mappings, error)) {
    if (error && error->isEmpty()) *error = QStringLiteral("Plasma screen mapping is unavailable");
    return false;
  }
  if (!mappings || mappings->isEmpty()) {
    if (error) *error = QStringLiteral("Plasma screen mapping is unavailable");
    return false;
  }
  QSet<QString> connectors;
  QSet<quint32> screens;
  for (const PlasmaScreenMapping &mapping : *mappings) {
    if (!isCanonicalOutputRequest(mapping.connector) || connectors.contains(mapping.connector) ||
        screens.contains(mapping.screenNumber)) {
      if (error) *error = QStringLiteral("Plasma screen mapping is ambiguous or invalid");
      return false;
    }
    connectors.insert(mapping.connector);
    screens.insert(mapping.screenNumber);
  }
  return true;
}

bool PlasmaWallpaperActivator::restoreTarget(const PlasmaActivationPlan &plan, QString *error) {
  if (!transport_->setWallpaper(plan.targetBefore.plugin, plan.targetBefore.parameters,
                                plan.screenNumber, error)) {
    return false;
  }
  PlasmaWallpaperState restored;
  if (!transport_->wallpaper(plan.screenNumber, &restored, error)) return false;
  if (!equalWallpaperStates(restored, plan.targetBefore)) {
    if (error) *error = QStringLiteral("target wallpaper rollback postcondition failed");
    return false;
  }
  return otherScreensUnchanged(plan, error);
}

bool PlasmaWallpaperActivator::otherScreensUnchanged(const PlasmaActivationPlan &plan,
                                                      QString *error) {
  int otherIndex = 0;
  for (const PlasmaScreenMapping &mapping : plan.mappingsBefore) {
    if (mapping.screenNumber == plan.screenNumber) continue;
    PlasmaWallpaperState state;
    if (!transport_->wallpaper(mapping.screenNumber, &state, error)) return false;
    if (otherIndex >= plan.otherScreensBefore.size() ||
        !equalWallpaperStates(state, plan.otherScreensBefore[otherIndex])) {
      if (error) *error = QStringLiteral("non-target Plasma wallpaper changed during activation");
      return false;
    }
    ++otherIndex;
  }
  if (otherIndex != plan.otherScreensBefore.size()) {
    if (error) *error = QStringLiteral("non-target Plasma wallpaper snapshot is invalid");
    return false;
  }
  return true;
}

PlasmaWallpaperState PlasmaWallpaperActivator::desiredState(const PlasmaActivationPlan &plan) {
  const QString plugin = QString::fromLatin1(kPlasmaPlugin);
  QVariantMap parameters{{QStringLiteral("wallpaperPlugin"), plugin},
                          {QStringLiteral("Output"), plan.output},
                          {QStringLiteral("ScaleMode"), plan.scaleMode}};
  return {plugin, parameters};
}

bool PlasmaWallpaperActivator::matchesDesiredState(const PlasmaWallpaperState &state,
                                                    const PlasmaActivationPlan &plan) {
  return state.plugin == QString::fromLatin1(kPlasmaPlugin) &&
         state.parameters.value(QStringLiteral("Output")).metaType().id() == QMetaType::QString &&
         state.parameters.value(QStringLiteral("Output")).toString() == plan.output &&
         state.parameters.value(QStringLiteral("ScaleMode")).metaType().id() == QMetaType::QString &&
         state.parameters.value(QStringLiteral("ScaleMode")).toString() == plan.scaleMode;
}

bool PlasmaWallpaperActivator::equalWallpaperStates(const PlasmaWallpaperState &left,
                                                     const PlasmaWallpaperState &right) {
  return left.plugin == right.plugin &&
         equalVariant(QVariant(left.parameters), QVariant(right.parameters));
}
