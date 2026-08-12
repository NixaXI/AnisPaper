#include "../src/renderers/static_image_renderer.h"

#include <QCryptographicHash>
#include <QDir>
#include <QImage>
#include <QTemporaryDir>

#include <cstdio>

namespace {
QImage makeGradient(int width, int height) {
  QImage image(width, height, QImage::Format_ARGB32);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      image.setPixelColor(x, y,
                          QColor((x * 255) / width, (y * 255) / height,
                                 ((x + y) * 255) / (width + height)));
    }
  }
  return image;
}

QString hashPixels(const QImage &image) {
  const QImage normalized =
      image.convertToFormat(QImage::Format_RGBA8888);
  return QString::fromLatin1(
      QCryptographicHash::hash(QByteArrayView(
                                   reinterpret_cast<const char *>(
                                       normalized.constBits()),
                                   normalized.sizeInBytes()),
                               QCryptographicHash::Sha256)
          .toHex());
}

int check(bool ok, const char *name) {
  std::fprintf(ok ? stdout : stderr, "%s: %s\n", ok ? "PASS" : "FAIL", name);
  return ok ? 0 : 1;
}
}  // namespace

int main() {
  QTemporaryDir dir;
  if (!dir.isValid()) {
    std::fprintf(stderr, "FAIL: cannot create temp dir\n");
    return 1;
  }
  const QString sourcePath = dir.filePath(QStringLiteral("image.png"));
  if (!makeGradient(400, 200).save(sourcePath)) {
    std::fprintf(stderr, "FAIL: cannot write source image\n");
    return 1;
  }

  int failures = 0;

  RendererSpec spec;
  spec.type = QStringLiteral("static");
  spec.file = sourcePath;
  spec.preview = sourcePath;
  spec.width = 320;
  spec.height = 240;

  QString error;
  StaticImageRenderer renderer(spec);
  failures += check(renderer.start(&error), "start");
  failures += check(!renderer.lastFrame().isNull(), "non-null frame");
  failures += check(renderer.lastFrame().size() == QSize(320, 240),
                    "frame matches target size");
  failures += check(renderer.isRunning(), "running after start");

  // cover crop must differ from a fit letterbox and from a plain stretch, and
  // the image content hash must change with the mode.
  QImage cover = renderer.lastFrame().convertToFormat(QImage::Format_RGBA8888);

  spec.scaleMode = QStringLiteral("fit");
  StaticImageRenderer fitRenderer(spec);
  failures += check(fitRenderer.start(&error), "fit start");
  const QImage fit = fitRenderer.lastFrame().convertToFormat(QImage::Format_RGBA8888);
  failures += check(fit.size() == QSize(320, 240), "fit size");
  failures += check(hashPixels(fit) != hashPixels(cover), "fit != cover pixels");

  spec.scaleMode = QStringLiteral("stretch");
  StaticImageRenderer stretchRenderer(spec);
  failures += check(stretchRenderer.start(&error), "stretch start");
  const QImage stretch =
      stretchRenderer.lastFrame().convertToFormat(QImage::Format_RGBA8888);
  failures += check(stretch.size() == QSize(320, 240), "stretch size");
  failures += check(hashPixels(stretch) != hashPixels(cover),
                    "stretch != cover pixels");

  failures += check(!StaticImageRenderer::defaultFrame(64, 64).isNull(),
                    "default frame available");

  if (failures != 0) {
    return 1;
  }
  std::fprintf(stdout, "ALL TESTS PASSED\n");
  return 0;
}
