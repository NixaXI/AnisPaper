#include "../src/renderers/wallpaper_properties.h"

#include <QJsonObject>
#include <cstdio>

int main() {
  QJsonObject schema{
      {QStringLiteral("bloom"),
       QJsonObject{{QStringLiteral("type"), QStringLiteral("bool")},
                   {QStringLiteral("value"), false}}},
      {QStringLiteral("tint"),
       QJsonObject{{QStringLiteral("type"), QStringLiteral("color")},
                   {QStringLiteral("value"), QStringLiteral("1 0 0")}}},
      {QStringLiteral("label"),
       QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                   {QStringLiteral("value"), QStringLiteral("skip")}}},
  };
  const QJsonObject merged = WallpaperProperties::mergeOverrides(
      schema, QJsonObject{{QStringLiteral("bloom"), true}});
  if (merged.value(QStringLiteral("bloom")).toObject().value(QStringLiteral("value")).toBool() !=
      true) {
    std::fprintf(stderr, "wallpaper_properties_test: bloom override missing\\n");
    return 1;
  }
  const QStringList assignments = WallpaperProperties::setPropertyAssignments(merged);
  if (!assignments.contains(QStringLiteral("bloom=1")) ||
      !assignments.contains(QStringLiteral("tint=1 0 0")) ||
      assignments.contains(QStringLiteral("label=skip"))) {
    std::fprintf(stderr, "wallpaper_properties_test: assignment list is wrong\\n");
    return 1;
  }
  const QString script = WallpaperProperties::applyUserPropertiesScript(merged);
  if (!script.contains(QStringLiteral("applyUserProperties")) ||
      !script.contains(QStringLiteral("\"bloom\""))) {
    std::fprintf(stderr, "wallpaper_properties_test: inject script missing payload\\n");
    return 1;
  }
  return 0;
}
