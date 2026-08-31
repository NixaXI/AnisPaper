#include "../src/common/gpu_vendor.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cstdio>

namespace {

bool writeFile(const QString &path, const QString &content) {
  QDir().mkpath(QFileInfo(path).absolutePath());
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
  return file.write(content.toLatin1()) >= 0;
}

// Builds `<root>/<card>/device/vendor` (+ modalias marker) trees and asserts
// the documented decision order: hybrid AMD+Intel picks AMD, Intel-only picks
// Intel, NVIDIA-only picks NVDEC, virtual GPUs are software.
bool expectVendor(const QTemporaryDir &root, const QString &card,
                  const QString &vendor, GpuVendor expected,
                  bool withDevice = true) {
  if (withDevice) {
    if (!writeFile(root.path() + "/" + card + "/device/modalias",
                   QStringLiteral("pci:v000010DE"))) {
      std::fprintf(stderr, "gpu_vendor_test: cannot write modalias for %s\n",
                   card.toLatin1().constData());
      return false;
    }
    if (!writeFile(root.path() + "/" + card + "/device/vendor", vendor)) {
      std::fprintf(stderr, "gpu_vendor_test: cannot write vendor for %s\n",
                   card.toLatin1().constData());
      return false;
    }
  } else {
    // Connector-style entry (card0-DP-1) with no device directory: it must be
    // skipped without fabricating a vendor.
    QDir().mkpath(root.path() + "/" + card);
  }
  const GpuVendor detected = detectGpuVendor(root.path());
  if (detected != expected) {
    std::fprintf(stderr, "gpu_vendor_test: %s vendor=%s expected=%s got=%s\n",
                 card.toLatin1().constData(), vendor.toLatin1().constData(),
                 gpuVendorId(expected).toLatin1().constData(),
                 gpuVendorId(detected).toLatin1().constData());
    return false;
  }
  return true;
}

}  // namespace

int main() {
  bool ok = true;

  {
    QTemporaryDir root;
    if (!root.isValid()) {
      std::fprintf(stderr, "gpu_vendor_test: temporary root unavailable\n");
      return 1;
    }
    ok = expectVendor(root, "card0", QStringLiteral("0x10de\n"), GpuVendor::Nvidia) && ok;
    ok = expectVendor(root, "renderD128", QStringLiteral("0x10de\n"),
                      GpuVendor::Nvidia) && ok;
  }
  {
    QTemporaryDir root;
    ok = expectVendor(root, "card0", QStringLiteral("0x1002\n"), GpuVendor::Amd) && ok;
  }
  {
    QTemporaryDir root;
    ok = expectVendor(root, "card0", QStringLiteral("0x8086\n"), GpuVendor::Intel) && ok;
  }
  {
    // Hybrid laptop: iGPU (Intel) + dGPU (NVIDIA).  The renderers run on the
    // display GPU, so the recommendation must stay VAAPI.
    QTemporaryDir root;
    ok = expectVendor(root, "card0", QStringLiteral("0x8086\n"), GpuVendor::Intel) && ok;
    ok = expectVendor(root, "card1", QStringLiteral("0x10de\n"), GpuVendor::Intel) && ok;
  }
  {
    // virtio-gpu in a VM must not be misread as a hardware decoder vendor.
    QTemporaryDir root;
    ok = expectVendor(root, "card0", QStringLiteral("0x1af4\n"),
                      GpuVendor::Software) && ok;
  }
  {
    // Connectors (card0-DP-1) and nodes without a device directory carry no
    // vendor evidence and must not crash or fabricate a vendor.
    QTemporaryDir root;
    ok = expectVendor(root, "card0-DP-1", QString(), GpuVendor::Unknown,
                      /*withDevice=*/false) && ok;
  }

  // The mpv interop recommendation follows the vendor decision.  Zero-copy
  // vaapi is preferred (core-profile 3.3 render context satisfies the Mesa
  // GL import); mpv self-falls-back to -copy/software when unavailable.
  ok = recommendedVideoHwdec(GpuVendor::Nvidia) == QStringLiteral("nvdec") && ok;
  ok = recommendedVideoHwdec(GpuVendor::Amd) == QStringLiteral("vaapi-copy") && ok;
  ok = recommendedVideoHwdec(GpuVendor::Intel) == QStringLiteral("vaapi-copy") && ok;
  ok = recommendedVideoHwdec(GpuVendor::Unknown) == QStringLiteral("auto-safe") && ok;

  if (ok) std::puts("gpu_vendor_test: PASS");
  return ok ? 0 : 1;
}
