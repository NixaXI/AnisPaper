#include "static_image_renderer.h"

#include <QFileInfo>
#include <QImageReader>
#include <QPainter>
#include <QStandardPaths>

StaticImageRenderer::StaticImageRenderer(RendererSpec spec, QObject *parent)
    : Renderer(std::move(spec), parent) {}

bool StaticImageRenderer::start(QString *error) {
  Q_UNUSED(error);
  QImage image;
  if (!spec_.preview.isEmpty() && QFileInfo(spec_.preview).isFile()) {
    QImageReader reader(spec_.preview);
    image = reader.read();
  }
  if (image.isNull()) image = defaultFrame(spec_.width, spec_.height);
  frame_ = image.convertToFormat(QImage::Format_RGBA8888);
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
  const int safeWidth = qBound(64, width, 3840);
  const int safeHeight = qBound(64, height, 2160);
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
  const QSize requested(qBound(64, width, 3840), qBound(64, height, 2160));
  return image.scaled(requested, Qt::KeepAspectRatio, Qt::SmoothTransformation)
      .convertToFormat(QImage::Format_RGBA8888);
}
