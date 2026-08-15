#pragma once

#include <functional>
#include <QObject>
#include <QString>

// Uses Plasma Login Manager's official KAuth save action.  This is separate
// from plasma-login-wallpaper, which is a greeter preview executable rather
// than a noninteractive installer.
using PlasmaLoginWallpaperCallback = std::function<void(bool, const QString &)>;

void installPlasmaLoginWallpaperAsync(const QString &pngPath, QObject *context,
                                       PlasmaLoginWallpaperCallback callback);
