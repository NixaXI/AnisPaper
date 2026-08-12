#include "static_image_renderer.h"

#include <QFileInfo>
#include <QImageReader>
#include <QPainter>
#include <QStandardPaths>

namespace {

QSize targetSize(int width, int height) {
  return {qBound(64, width, 3840), qBound(64, height, 2160)};
}

QString normalizedScaleModeSpec(const QString &value) {
  const QString mode = value.trimmed().toLower();
  return mode == QStringLiteral("fit") || mode == QStringLiteral("stretch")
             ? mode
             : QStringLiteral("cover");
}

// Static scene fallbacks are already the final physical wl_output frame.  Do
// the aspect-aware scaling here, once, so the SHM bridge does not rescale a
// second time.  cover crops without deforming; fit letterboxes on #0A0D14;
// stretch is an explicit user choice.  One SmoothTransformation pass keeps the
// highest quality the source preview allows.
QImage scaledImage(const QImage &source, int width, int height,
                   const QString &scaleMode) {
  const QSize target = targetSize(width, height);
  if (source.isNull()) {
    return {};
  }

  const QString mode = normalizedScaleModeSpec(scaleMode);
  QImage result(target, QImage::Format_RGBA8888);
  result.fill(QColor(QStringLiteral("#0A0D14")));
  if (mode == QStringLiteral("stretch")) {
    result = source.scaled(target, Qt::IgnoreAspectRatio,
                           Qt::SmoothTransformation)
                 .convertToFormat(QImage::Format_RGBA8888);
    return result;
  }

  const Qt::AspectRatioMode aspect =
      mode == QStringLiteral("fit") ? Qt::KeepAspectRatio
                                    : Qt::KeepAspectRatioByExpanding;
  const QImage scaled = source.scaled(target, aspect, Qt::SmoothTransformation);
  QPainter painter(&result);
  painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
  painter.drawImage((target.width() - scaled.width()) / 2,
                    (target.height() - scaled.height()) / 2, scaled);
  return result;
}

QImage coverImage(const QImage &source, int width, int height) {
  return scaledImage(source, width, height, QStringLiteral("cover"));
}

}  // namespace

StaticImageRenderer::StaticImageRenderer(RendererSpec spec, QObject *parent)
    : Renderer(std::move(spec), parent) {}

bool StaticImageRenderer::start(QString *error) {
  Q_UNUSED(error);
  QImage image;
  if (!spec_.preview.isEmpty() && QFileInfo(spec_.preview).isFile()) {
    QImageReader reader(spec_.preview);
    // Orientation metadata must not rotate a wallpaper; high-quality decode.
    reader.setAutoTransform(true);
    image = reader.read();
  }
  if (image.isNull()) image = defaultFrame(spec_.width, spec_.height);
  // The static frame deliberately matches the exact physical renderer size and
  // honours the user's scaleMode.  There are no default letterbox bars for
  // scene-static fallbacks in cover mode.
  frame_ = scaledImage(image, spec_.width, spec_.height, spec_.scaleMode);
  running_ = true;
  paused_ = false;
  emit ready();
  emit frameReady(frame_);
  return true;
}

void StaticImageRenderer::stop() {
  running_ = false;
  paused_ = false;
}

void StaticImageRenderer::pause() { paused_ = true; }

void StaticImageRenderer::resume() { paused_ = false; }

QImage StaticImageRenderer::lastFrame() const { return frame_; }

QString StaticImageRenderer::rendererName() const {
  return QStringLiteral("static-image");
}

bool StaticImageRenderer::isRunning() const { return running_; }

bool StaticImageRenderer::isFallback() const { return true; }

QImage StaticImageRenderer::fallbackFrame(const QString &label, int width,
                                          int height) {
  const QSize target = targetSize(width, height);
  const int safeWidth = target.width();
  const int safeHeight = target.height();
  QImage image(safeWidth, safeHeight, QImage::Format_RGBA8888);
  image.fill(QColor(QStringLiteral("#101622")));
  QPainter painter(&image);
  painter.fillRect(0, 0, safeWidth, safeHeight / 7,
                   QColor(QStringLiteral("#0A0D14")));
  painter.setPen(QColor(QStringLiteral("#00C2FF")));
  painter.drawRect(10, 10, safeWidth - 21, safeHeight - 21);
  painter.setPen(QColor(QStringLiteral("#FFD000")));
  QFont font = painter.font();
  font.setBold(true);
  font.setPointSize(qMax(12, safeHeight / 16));
  painter.setFont(font);
  painter.drawText(image.rect(), Qt::AlignCenter | Qt::TextWordWrap, label);
  return image;
}

QImage StaticImageRenderer::defaultFrame(int width, int height) {
  const QString installedFallback = QStandardPaths::locate(
      QStandardPaths::GenericDataLocation, QStringLiteral("anispaper/fallback.jpg"),
      QStandardPaths::LocateFile);
  QImage image;
  if (!installedFallback.isEmpty()) {
    QImageReader reader(installedFallback);
    image = reader.read();
  }
  if (image.isNull()) {
    return fallbackFrame(QStringLiteral("ANISPAPER SAFE MODE"), width, height);
  }
  return coverImage(image, width, height);
}
