#pragma once

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <cmath>

// Wallpaper Engine project.json `general.properties` helpers.  The schema is
// an object of named entries, each typically `{type, text, value, ...}`.
namespace WallpaperProperties {

inline QString encodeValue(const QJsonValue &value) {
  if (value.isBool()) {
    return value.toBool() ? QStringLiteral("1") : QStringLiteral("0");
  }
  if (value.isDouble()) {
    const double number = value.toDouble();
    if (!std::isfinite(number)) return {};
    if (number == std::floor(number) && number >= -1e15 && number <= 1e15) {
      return QString::number(static_cast<qint64>(number));
    }
    return QString::number(number, 'g', 15);
  }
  if (value.isString()) return value.toString();
  return {};
}

inline bool isUserFacing(const QJsonObject &property) {
  if (property.value(QStringLiteral("hidden")).toBool()) return false;
  const QString type = property.value(QStringLiteral("type")).toString();
  return type == QStringLiteral("bool") || type == QStringLiteral("slider") ||
         type == QStringLiteral("color") || type == QStringLiteral("combo") ||
         type == QStringLiteral("textinput") || type == QStringLiteral("file") ||
         type == QStringLiteral("directory");
}

inline QJsonObject mergeOverrides(const QJsonObject &schema,
                                  const QJsonObject &overrides) {
  if (overrides.isEmpty()) return schema;
  QJsonObject merged = schema;
  for (auto it = overrides.begin(); it != overrides.end(); ++it) {
    const QJsonValue current = merged.value(it.key());
    if (!current.isObject()) continue;
    QJsonObject property = current.toObject();
    property.insert(QStringLiteral("value"), it.value());
    merged.insert(it.key(), property);
  }
  return merged;
}

inline QJsonObject applyUserPropertiesPayload(const QJsonObject &schema) {
  QJsonObject payload;
  for (auto it = schema.begin(); it != schema.end(); ++it) {
    if (!it.value().isObject()) continue;
    const QJsonObject property = it.value().toObject();
    if (!isUserFacing(property) && !property.contains(QStringLiteral("value"))) {
      continue;
    }
    payload.insert(it.key(), property);
  }
  return payload;
}

inline QString applyUserPropertiesScript(const QJsonObject &schema) {
  const QJsonObject payload = applyUserPropertiesPayload(schema);
  if (payload.isEmpty()) return {};
  const QByteArray json =
      QJsonDocument(payload).toJson(QJsonDocument::Compact);
  return QStringLiteral(
             "(function(){"
             "const props=%1;"
             "window.__anispaperUserProperties=props;"
             "function deliver(){"
             "try{"
             "const l=window.wallpaperPropertyListener;"
             "if(l&&typeof l.applyUserProperties==='function'){"
             "l.applyUserProperties(props);"
             "}"
             "}catch(e){}"
             "}"
             "if(document.readyState==='loading'){"
             "document.addEventListener('DOMContentLoaded',deliver);"
             "}else{deliver();}"
             "setTimeout(deliver,0);"
             "setTimeout(deliver,250);"
             "setTimeout(deliver,1000);"
             "})();")
      .arg(QString::fromUtf8(json));
}

inline QStringList setPropertyAssignments(const QJsonObject &schema) {
  QStringList assignments;
  for (auto it = schema.begin(); it != schema.end(); ++it) {
    if (!it.value().isObject()) continue;
    const QJsonObject property = it.value().toObject();
    if (!isUserFacing(property) || !property.contains(QStringLiteral("value"))) {
      continue;
    }
    const QJsonValue value = property.value(QStringLiteral("value"));
    if (value.isNull() || value.isUndefined() || value.isArray() ||
        value.isObject()) {
      continue;
    }
    const QString encoded = encodeValue(value);
    if (encoded.contains(QLatin1Char('\n')) ||
        encoded.contains(QLatin1Char('\r'))) {
      continue;
    }
    assignments << (it.key() + QLatin1Char('=') + encoded);
    if (assignments.size() >= 512) break;
  }
  return assignments;
}

inline bool validOverrideValue(const QJsonValue &value) {
  if (value.isBool() || value.isString()) return true;
  if (!value.isDouble()) return false;
  return std::isfinite(value.toDouble());
}

}  // namespace WallpaperProperties
