#pragma once

#include <QtGlobal>
#include <QString>

// Best-effort GPU vendor discovery for renderer offloading decisions.  It only
// reads sysfs (no subprocess, no GL context), so the daemon can use it before
// any renderer exists and the isolated children can use it at startup.
enum class GpuVendor {
  Unknown,
  Software,  // llvmpipe / virtio without a real vendor id
  Intel,
  Amd,
  Nvidia,
};

// Scans `<sysClassRoot>/card*/device/vendor` and `renderD*/device/vendor`.
// `sysClassRoot` is injectable so tests can point it at a fake tree.
GpuVendor detectGpuVendor(const QString &sysClassRoot = QStringLiteral("/sys/class/drm"));

// Stable lowercase id ("nvidia", "amd", "intel", "software", "unknown") used in
// logs and the status RPC.
QString gpuVendorId(GpuVendor vendor);

// Human label for the UI/journal.
QString gpuVendorLabel(GpuVendor vendor);

// mpv `hwdec` value for this machine.  Hybrid laptops (Intel iGPU + NVIDIA
// dGPU) render through the iGPU, so VAAPI stays the safe interop there.
QString recommendedVideoHwdec(GpuVendor vendor);
