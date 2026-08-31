#include "gpu_vendor.h"

#include <QDir>
#include <QFile>
#include <QSet>

namespace {
// PCI vendor ids as written by sysfs ("0x10de\n").
constexpr const char *kPciNvidia = "0x10de";
constexpr const char *kPciAmd = "0x1002";
constexpr const char *kPciIntel = "0x8086";

GpuVendor vendorFromFile(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return GpuVendor::Unknown;
  const QString id = QString::fromLatin1(file.readAll().trimmed());
  if (id == kPciNvidia) return GpuVendor::Nvidia;
  if (id == kPciAmd) return GpuVendor::Amd;
  if (id == kPciIntel) return GpuVendor::Intel;
  // virtio-gpu and friends expose the virtual vendor id; treat anything
  // recognized-but-not-real as software rather than guessing an interop.
  if (id == QStringLiteral("0x1af4") || id == QStringLiteral("0x1b36") ||
      id == QStringLiteral("0x1234")) {
    return GpuVendor::Software;
  }
  return GpuVendor::Unknown;
}
}  // namespace

GpuVendor detectGpuVendor(const QString &sysClassRoot) {
  const QDir root(sysClassRoot);
  if (!root.exists()) return GpuVendor::Unknown;
  const QStringList filters{QStringLiteral("card*"), QStringLiteral("renderD*")};
  QSet<int> seen;
  bool sawSoftware = false;
  // Sorted so the scan is deterministic across boots.
  const auto entries = root.entryInfoList(filters, QDir::Dirs | QDir::NoDotAndDotDot,
                                          QDir::Name);
  for (const auto &entry : entries) {
    QFile modalias(entry.absoluteFilePath() + QStringLiteral("/device/modalias"));
    // Skip encoders/connectors-only DRM nodes: they carry no device/vendor.
    if (!modalias.exists()) continue;
    const GpuVendor vendor = vendorFromFile(entry.absoluteFilePath() +
                                            QStringLiteral("/device/vendor"));
    const int tag = static_cast<int>(vendor);
    if (tag > 0) seen.insert(tag);
    if (vendor == GpuVendor::Software) sawSoftware = true;
  }
  // Hybrid decision: the GL context the renderers get is normally the primary
  // display GPU (Intel/AMD iGPU on Optimus machines), so prefer the interop
  // that works there.  Discrete-only systems get their native decoder.
  if (seen.contains(static_cast<int>(GpuVendor::Amd)) ||
      seen.contains(static_cast<int>(GpuVendor::Intel))) {
    return seen.contains(static_cast<int>(GpuVendor::Intel)) &&
                    !seen.contains(static_cast<int>(GpuVendor::Amd))
                ? GpuVendor::Intel
                : GpuVendor::Amd;
  }
  if (seen.contains(static_cast<int>(GpuVendor::Nvidia))) return GpuVendor::Nvidia;
  return sawSoftware ? GpuVendor::Software : GpuVendor::Unknown;
}

QString gpuVendorId(GpuVendor vendor) {
  switch (vendor) {
    case GpuVendor::Intel:
      return QStringLiteral("intel");
    case GpuVendor::Amd:
      return QStringLiteral("amd");
    case GpuVendor::Nvidia:
      return QStringLiteral("nvidia");
    case GpuVendor::Software:
      return QStringLiteral("software");
    case GpuVendor::Unknown:
      break;
  }
  return QStringLiteral("unknown");
}

QString gpuVendorLabel(GpuVendor vendor) {
  switch (vendor) {
    case GpuVendor::Intel:
      return QStringLiteral("Intel");
    case GpuVendor::Amd:
      return QStringLiteral("AMD");
    case GpuVendor::Nvidia:
      return QStringLiteral("NVIDIA");
    case GpuVendor::Software:
      return QStringLiteral("Software (llvmpipe/virtio)");
    case GpuVendor::Unknown:
      break;
  }
  return QStringLiteral("Unknown");
}

QString recommendedVideoHwdec(GpuVendor vendor) {
  switch (vendor) {
    case GpuVendor::Intel:
    case GpuVendor::Amd:
    case GpuVendor::Software:
      // VAAPI is the native decode path on Mesa.  The zero-copy interop
      // needs an EGL-owned GL context that Qt's offscreen surface cannot
      // provide (verified: hwdec-current=no with a core-profile context),
      // so use -copy and keep the download cheap by scaling decoded frames
      // to the output size before the copy (see the vf option in the video
      // renderer).  mpv falls back to software on its own when even the
      // copy path is unavailable.
      return QStringLiteral("vaapi-copy");
    case GpuVendor::Nvidia:
      // NVDEC maps directly into the GL texture; VDPAU stays as mpv's own
      // fallback when the proprietary driver lacks the NVDEC interop.
      return QStringLiteral("nvdec");
    case GpuVendor::Unknown:
      // No vendor evidence: let mpv probe its safe methods and fall back to
      // software decoding on its own.
      return QStringLiteral("auto-safe");
  }
  return QStringLiteral("auto-safe");
}
