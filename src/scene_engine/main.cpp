// anis-paper-scene-engine — offscreen Wallpaper Engine scene renderer child.
//
// Renders a "scene" wallpaper offscreen using the vendored
// almamu/linux-wallpaperengine engine (hidden GLFW X11 window, never mapped).
// Frames are streamed to the AnisPaper daemon through a binary shared-memory
// transport (triple-buffered RGBA8888, latest-frame-wins); the JSON-lines
// channel carries only small control/notification messages.  A JPEG/base64
// fallback remains available for debugging (ANISPAPER_SCENE_JPEG=1) or when
// the transport cannot be created.  Plasma only displays the resulting
// frames; all engine complexity (shaders, particles, GL) stays inside this
// isolated process.

#include "WallpaperEngine/Application/ApplicationContext.h"
#include "WallpaperEngine/Application/WallpaperApplication.h"
#include "WallpaperEngine/Logging/Log.h"
#include "WallpaperEngine/Render/CWallpaper.h"
#include "WallpaperEngine/Render/RenderContext.h"
#include "WallpaperEngine/Render/WallpaperState.h"

#include <GL/glew.h>

#include <jpeglib.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace WallpaperEngine::Application;
using namespace WallpaperEngine::Render;

// ANISPAPER_SCENE_PROFILE=1 enables per-stage frame profiling.  Samples are
// collected in small ring vectors and reported to stderr every kProfileWindow
// frames with average + P95 so hot-path costs can be measured without
// polluting the JSON control channel.
constexpr int kProfileWindow = 120;

struct StageProfiler {
    using Clock = ::std::chrono::steady_clock;

    bool enabled = ::getenv ("ANISPAPER_SCENE_PROFILE") != nullptr;
    Clock::time_point callbackStart;
    Clock::time_point prevCallback;
    bool havePrev = false;
    ::std::vector<double> periodMs, submitMs, waitMs, copyMs, jpegMs, b64Ms, ipcWriteMs, callbackMs;

    static double ms (Clock::time_point a, Clock::time_point b) {
        return ::std::chrono::duration<double, ::std::milli> (b - a).count ();
    }
    void begin (Clock::time_point now) {
        if (!enabled) return;
        callbackStart = now;
        if (havePrev) {
            periodMs.push_back (ms (prevCallback, now));
        }
        prevCallback = now;
        havePrev = true;
    }
    void sample (::std::vector<double>& bucket, Clock::time_point a, Clock::time_point b) {
        if (enabled) bucket.push_back (ms (a, b));
    }
    void end (Clock::time_point now) {
        if (!enabled) return;
        callbackMs.push_back (ms (callbackStart, now));
        if (callbackMs.size () >= kProfileWindow) {
            report ();
        }
    }
    static void stats (const ::std::vector<double>& v, double& avg, double& p95, double& maxv) {
        if (v.empty ()) { avg = p95 = maxv = 0.0; return; }
        ::std::vector<double> sorted = v;
        ::std::sort (sorted.begin (), sorted.end ());
        double sum = 0.0;
        for (double d : sorted) sum += d;
        avg = sum / static_cast<double> (sorted.size ());
        p95 = sorted [static_cast<size_t> ((sorted.size () - 1) * 95 / 100)];
        maxv = sorted.back ();
    }
    void report () {
        auto print = [] (const char* name, const ::std::vector<double>& v) {
            double avg, p95, maxv;
            stats (v, avg, p95, maxv);
            ::fprintf (stderr, "  %-14s avg=%6.2f ms  p95=%6.2f ms  max=%6.2f ms  n=%zu\n",
                       name, avg, p95, maxv, v.size ());
        };
        ::fprintf (stderr, "[scene-profile] window of %zu callbacks:\n", callbackMs.size ());
        print ("period", periodMs);
        print ("submit", submitMs);
        print ("wait", waitMs);
        print ("map+copy", copyMs);
        if (!jpegMs.empty ()) print ("jpeg", jpegMs);
        if (!b64Ms.empty ()) print ("base64", b64Ms);
        print ("ipc-write", ipcWriteMs);
        print ("callback", callbackMs);
        ::fflush (stderr);
        periodMs.clear (); submitMs.clear (); waitMs.clear (); copyMs.clear ();
        jpegMs.clear (); b64Ms.clear (); ipcWriteMs.clear (); callbackMs.clear ();
    }
};

StageProfiler g_profiler;

std::atomic<bool> g_paused { false };
WallpaperApplication* g_app = nullptr;

void emitJson (const ::std::string& line) {
    ::fwrite (line.c_str (), 1, line.size (), stdout);
    ::fputc ('\n', stdout);
    ::fflush (stdout);
}

::std::string jsonEscape (const ::std::string& in) {
    ::std::string out;
    out.reserve (in.size () + 16);
    for (const char c : in) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c; break;
        }
    }
    return out;
}

void emitFatal (const ::std::string& message) {
    emitJson ("{\"event\":\"fatal\",\"message\":\"" + jsonEscape (message) + "\"}");
}

void emitReady (const ::std::string& renderer) {
    emitJson ("{\"event\":\"ready\",\"renderer\":\"" + jsonEscape (renderer) + "\"}");
}

::std::string base64Encode (const unsigned char* data, size_t length) {
    static const char* table
        = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    ::std::string out;
    out.reserve (((length + 2) / 3) * 4);
    for (size_t i = 0; i < length; i += 3) {
        const unsigned int triple
            = (data[i] << 16) | (i + 1 < length ? data[i + 1] << 8 : 0) | (i + 2 < length ? data[i + 2] : 0);
        out += table[(triple >> 18) & 0x3F];
        out += table[(triple >> 12) & 0x3F];
        out += (i + 1 < length) ? table[(triple >> 6) & 0x3F] : '=';
        out += (i + 2 < length) ? table[triple & 0x3F] : '=';
    }
    return out;
}

::std::string jpegEncode (const unsigned char* rgb, int width, int height, int quality) {
    jpeg_compress_struct cinfo {};
    jpeg_error_mgr jerr {};
    cinfo.err = jpeg_std_error (&jerr);
    jpeg_create_compress (&cinfo);

    unsigned char* outBuffer = nullptr;
    unsigned long outSize = 0;
    jpeg_mem_dest (&cinfo, &outBuffer, &outSize);

    cinfo.image_width = static_cast<JDIMENSION> (width);
    cinfo.image_height = static_cast<JDIMENSION> (height);
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults (&cinfo);
    jpeg_set_quality (&cinfo, quality, TRUE);
    jpeg_start_compress (&cinfo, TRUE);

    const int rowStride = width * 3;
    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW row = const_cast<JSAMPROW> (rgb + cinfo.next_scanline * static_cast<unsigned long> (rowStride));
        jpeg_write_scanlines (&cinfo, &row, 1);
    }
    jpeg_finish_compress (&cinfo);

    ::std::string result (reinterpret_cast<char*> (outBuffer), outSize);
    jpeg_destroy_compress (&cinfo);
    ::free (outBuffer);
    return result;
}

uint64_t fnv1aSample (const unsigned char* data, size_t length, size_t step) {
    uint64_t hash = 1469598103934665603ULL;
    for (size_t i = 0; i < length; i += step) {
        hash ^= data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

// ---------------------------------------------------------------------------
// Binary scene transport (child -> daemon)
//
// POSIX shm object named /anispaper-scene-<pid>, owned by this child. Layout:
// SceneTransportHeader followed by `buffers` RGBA8888 slots. Publication is
// latest-frame-wins: the writer never blocks; the reader skips to the newest
// published slot and retries if the index moves mid-copy. No image bytes are
// ever sent through the JSON channel.

constexpr uint32_t kTransportVersion = 1;
constexpr uint32_t kTransportBuffers = 3;

struct SceneTransportHeader {
    char magic [4];            // "ANST"
    uint32_t version;
    uint64_t frameNo;          // acquire/release publication counter
    uint64_t timestampNs;
    uint32_t writeIndex;       // slot that holds frameNo
    uint32_t width;
    uint32_t height;
    uint32_t stride;           // bytes per row
    uint32_t format;           // 1 = RGBA8888
    uint32_t buffers;          // slot count
    uint32_t reserved [4];
};

static_assert (sizeof (SceneTransportHeader) == 64,
               "Scene transport ABI drifted from src/bridge/frame_protocol.h");

uint64_t monotonicNs () {
    timespec stamp {};
    ::clock_gettime (CLOCK_MONOTONIC, &stamp);
    return static_cast<uint64_t> (stamp.tv_sec) * 1000000000ULL
           + static_cast<uint64_t> (stamp.tv_nsec);
}

struct SceneTransport {
    ::std::string name;
    int fd = -1;
    void* mapping = nullptr;
    size_t mappingSize = 0;
    SceneTransportHeader* header = nullptr;
    unsigned char* slotBase = nullptr;
    size_t slotBytes = 0;

    bool create (int width, int height) {
        destroy ();
        slotBytes = static_cast<size_t> (width) * static_cast<size_t> (height) * 4;
        mappingSize = sizeof (SceneTransportHeader) + slotBytes * kTransportBuffers;
        name = "/anispaper-scene-" + ::std::to_string (::getpid ());
        fd = ::shm_open (name.c_str (), O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd < 0 && errno == EEXIST) {
            // Stale object from a crashed previous run; reclaim the name.
            ::shm_unlink (name.c_str ());
            fd = ::shm_open (name.c_str (), O_RDWR | O_CREAT | O_EXCL, 0600);
        }
        if (fd < 0 || ::ftruncate (fd, static_cast<off_t> (mappingSize)) != 0) {
            destroy ();
            return false;
        }
        mapping = ::mmap (nullptr, mappingSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (mapping == MAP_FAILED) {
            mapping = nullptr;
            destroy ();
            return false;
        }
        header = static_cast<SceneTransportHeader*> (mapping);
        ::memset (header, 0, sizeof (*header));
        ::memcpy (header->magic, "ANST", 4);
        header->version = kTransportVersion;
        header->width = static_cast<uint32_t> (width);
        header->height = static_cast<uint32_t> (height);
        header->stride = static_cast<uint32_t> (width) * 4;
        header->format = 1;
        header->buffers = kTransportBuffers;
        __atomic_store_n (&header->frameNo, 0ULL, __ATOMIC_RELEASE);
        __atomic_store_n (&header->writeIndex, 0u, __ATOMIC_RELEASE);
        header->timestampNs = monotonicNs ();
        slotBase = static_cast<unsigned char*> (mapping) + sizeof (SceneTransportHeader);
        return true;
    }

    // Writer side only (this child): the mapped PBO is memcpy'ed straight
    // into the slot, then finishWrite publishes atomically.
    unsigned char* slotFor (uint64_t seq) const {
        return slotBase + static_cast<size_t> (seq % kTransportBuffers) * slotBytes;
    }
    void finishWrite (uint64_t seq) {
        header->timestampNs = monotonicNs ();
        __atomic_store_n (&header->writeIndex, static_cast<uint32_t> (seq % kTransportBuffers),
                          __ATOMIC_RELEASE);
        __atomic_store_n (&header->frameNo, seq, __ATOMIC_RELEASE);
    }

    void destroy () {
        if (mapping) {
            ::munmap (mapping, mappingSize);
            mapping = nullptr;
        }
        header = nullptr;
        slotBase = nullptr;
        if (fd >= 0) {
            ::close (fd);
            fd = -1;
        }
        if (!name.empty ()) {
            ::shm_unlink (name.c_str ());
            name.clear ();
        }
        mappingSize = 0;
        slotBytes = 0;
    }
};

// ---------------------------------------------------------------------------
// GPU crop/flip/scale + asynchronous readback
//
// The engine renders each wallpaper into its own FBO, whose backing texture
// may include UV padding and whose orientation follows the GL bottom-up
// convention.  Instead of reading that FBO synchronously and remapping ~6M
// pixels on the CPU, the region of interest is blitted (glBlitFramebuffer,
// GPU-side crop + scale + vertical flip) into a scratch FBO of exactly the
// transport size, and the scratch FBO is read through a ring of pixel buffer
// objects with fences.  CPU work per frame is a single memcpy of a mapped
// PBO straight into the transport slot; there is no glFinish() in the hot
// path and the GPU pipeline is never stalled by the read.

struct AsyncReadback {
    static constexpr int kPboCount = static_cast<int> (kTransportBuffers);
    int width = 0;
    int height = 0;
    size_t bytes = 0;
    GLuint scratchFbo = 0;
    GLuint scratchTex = 0;
    GLuint pbos [kPboCount] = {};
    ::std::vector<GLuint> freePbos;

    struct Inflight {
        GLuint pbo;
        GLsync fence;
        uint64_t seq;
    };
    ::std::deque<Inflight> inflight;

    bool initialize (int w, int h) {
        width = w;
        height = h;
        bytes = static_cast<size_t> (w) * static_cast<size_t> (h) * 4;
        glGenTextures (1, &scratchTex);
        glBindTexture (GL_TEXTURE_2D, scratchTex);
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture (GL_TEXTURE_2D, 0);
        glGenFramebuffers (1, &scratchFbo);
        glBindFramebuffer (GL_FRAMEBUFFER, scratchFbo);
        glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, scratchTex, 0);
        const bool ok = glCheckFramebufferStatus (GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        glBindFramebuffer (GL_FRAMEBUFFER, 0);
        if (!ok) {
            return false;
        }
        glGenBuffers (kPboCount, pbos);
        for (int i = 0; i < kPboCount; ++i) {
            glBindBuffer (GL_PIXEL_PACK_BUFFER, pbos [i]);
            glBufferData (GL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr> (bytes), nullptr, GL_STREAM_READ);
            freePbos.push_back (pbos [i]);
        }
        glBindBuffer (GL_PIXEL_PACK_BUFFER, 0);
        return true;
    }

    // GPU-side crop/flip/scale of `sourceFbo`'s content into the scratch FBO
    // followed by an asynchronous readPixels into a PBO.  Returns false when
    // no free PBO is available (consumer side is falling behind).
    bool submit (GLuint sourceFbo, int sx0, int sy0, int sx1, int sy1, uint64_t seq) {
        if (scratchFbo == 0 || freePbos.empty ()) {
            return false;
        }
        glBindFramebuffer (GL_READ_FRAMEBUFFER, sourceFbo);
        glBindFramebuffer (GL_DRAW_FRAMEBUFFER, scratchFbo);
        glBlitFramebuffer (sx0, sy0, sx1, sy1, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer (GL_DRAW_FRAMEBUFFER, 0);
        const GLuint pbo = freePbos.back ();
        freePbos.pop_back ();
        // glReadPixels samples GL_READ_FRAMEBUFFER, which is still the source
        // wallpaper FBO at this point.  Point it at the scratch FBO or the read
        // silently returns the raw, uncropped, unflipped scene FBO plus
        // undefined pixels wherever the requested rectangle exceeds it.
        glBindFramebuffer (GL_READ_FRAMEBUFFER, scratchFbo);
        glBindBuffer (GL_PIXEL_PACK_BUFFER, pbo);
        glPixelStorei (GL_PACK_ALIGNMENT, 4);
        glReadPixels (0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindBuffer (GL_PIXEL_PACK_BUFFER, 0);
        glBindFramebuffer (GL_READ_FRAMEBUFFER, 0);

        GLsync fence = glFenceSync (GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (fence == nullptr) {
            freePbos.push_back (pbo);
            return false;
        }
        inflight.push_back ({pbo, fence, seq});
        return true;
    }

    // Non-blocking fence poll for the oldest read.  Returns false when the
    // GPU has not finished it yet.  The wait has a zero timeout: the read is
    // one frame old at steady state, so the GPU is normally already done.
    bool frontReady () {
        if (inflight.empty ()) {
            return false;
        }
        const GLenum status = glClientWaitSync (inflight.front ().fence, 0, 0);
        if (status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED) {
            return true;
        }
        if (status == GL_WAIT_FAILED) {
            // Broken fence: drop the read rather than stalling the ring.
            glDeleteSync (inflight.front ().fence);
            freePbos.push_back (inflight.front ().pbo);
            inflight.pop_front ();
        }
        return false;
    }

    // Copies the mapped PBO of the oldest read into `dst`.  Call only after
    // frontReady() returned true.
    bool consume (unsigned char* dst) {
        if (inflight.empty ()) {
            return false;
        }
        const Inflight front = inflight.front ();
        glDeleteSync (front.fence);
        inflight.pop_front ();
        bool copied = false;
        glBindBuffer (GL_PIXEL_PACK_BUFFER, front.pbo);
        void* mapped = glMapBufferRange (GL_PIXEL_PACK_BUFFER, 0,
                                         static_cast<GLsizeiptr> (bytes), GL_MAP_READ_BIT);
        if (mapped) {
            ::memcpy (dst, mapped, bytes);
            glUnmapBuffer (GL_PIXEL_PACK_BUFFER);
            copied = true;
        }
        glBindBuffer (GL_PIXEL_PACK_BUFFER, 0);
        freePbos.push_back (front.pbo);
        return copied;
    }

    uint64_t frontSeq () const { return inflight.empty () ? 0 : inflight.front ().seq; }

    void shutdown () {
        // GL object destruction is intentionally skipped: at shutdown the SDL
        // context may already be gone, and the driver reclaims everything at
        // process exit anyway.  Only the CPU-side ring state is reset.
        inflight.clear ();
        freePbos.clear ();
    }
};

AsyncReadback g_readback;
SceneTransport g_transport;

// Queues the GPU-side capture of the current wallpaper frame into the PBO
// ring (crop + flip + scale via blit, then asynchronous read).  Returns false
// until the wallpaper framebuffer is available.
bool captureFrame (WallpaperApplication& app, int width, int height, uint64_t seq) {
    auto& renderContext = app.getRenderContext ();
    const auto& wallpapers = renderContext.getWallpapers ();
    const auto wallpaperIt = wallpapers.find ("default");
    if (wallpaperIt == wallpapers.end () || !wallpaperIt->second) {
        return false;
    }
    const auto& wallpaper = wallpaperIt->second;
    const int srcWidth = wallpaper->getWidth ();
    const int srcHeight = wallpaper->getHeight ();
    if (srcWidth <= 0 || srcHeight <= 0) {
        return false;
    }
    const auto [ustart, uend, vstart, vend] = wallpaper->getState ().getTextureUVs ();
    const bool vflip = renderContext.getOutput ().renderVFlip ();
    const int sx0 = ::std::clamp (static_cast<int> (ustart * srcWidth), 0, srcWidth - 1);
    const int sx1 = ::std::clamp (static_cast<int> (uend * srcWidth + 0.5f), 1, srcWidth);
    const int sy0 = ::std::clamp (static_cast<int> (vstart * srcHeight), 0, srcHeight - 1);
    const int sy1 = ::std::clamp (static_cast<int> (vend * srcHeight + 0.5f), 1, srcHeight);
    // ANISPAPER_SCENE_GEOMETRY=1 dumps the full coordinate chain once so the
    // scene source resolution, the UV crop, the hidden GLFW viewport and the
    // transport size can be compared without guessing.
    static bool geometryLogged = false;
    if (!geometryLogged && ::getenv ("ANISPAPER_SCENE_GEOMETRY") != nullptr) {
        geometryLogged = true;
        const auto& output = renderContext.getOutput ();
        const auto& viewports = output.getViewports ();
        ::fprintf (stderr,
                   "[scene-geometry] transport=%dx%d wallpaper=%dx%d vflip=%d\n"
                   "[scene-geometry] UVs u=[%.6f,%.6f] v=[%.6f,%.6f]\n"
                   "[scene-geometry] blitSrc=(%d,%d)-(%d,%d) -> %dx%d\n"
                   "[scene-geometry] output full=%dx%d viewports=%zu\n",
                   width, height, srcWidth, srcHeight, vflip ? 1 : 0, ustart, uend,
                   vstart, vend, sx0, sy0, sx1, sy1, width, height,
                   output.getFullWidth (), output.getFullHeight (), viewports.size ());
        for (const auto& [name, viewport] : viewports) {
            ::fprintf (stderr, "[scene-geometry]   viewport %s = (%d,%d,%d,%d) single=%d\n",
                       name.c_str (), viewport->viewport.x, viewport->viewport.y,
                       viewport->viewport.z, viewport->viewport.w, viewport->single ? 1 : 0);
        }
        GLint drawFboWidth = 0, drawFboHeight = 0;
        glBindFramebuffer (GL_READ_FRAMEBUFFER, wallpaper->getWallpaperFramebuffer ());
        GLint attachedTexture = 0;
        glGetFramebufferAttachmentParameteriv (GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                               GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,
                                               &attachedTexture);
        if (attachedTexture > 0) {
            glBindTexture (GL_TEXTURE_2D, static_cast<GLuint> (attachedTexture));
            glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &drawFboWidth);
            glGetTexLevelParameteriv (GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &drawFboHeight);
            glBindTexture (GL_TEXTURE_2D, 0);
        }
        glBindFramebuffer (GL_READ_FRAMEBUFFER, 0);
        ::fprintf (stderr, "[scene-geometry] wallpaper FBO texture = %dx%d\n",
                   drawFboWidth, drawFboHeight);
        ::fflush (stderr);
    }
    if (sx0 == sx1 || sy0 == sy1) {
        return false;
    }

    if (vflip) {
        return g_readback.submit (wallpaper->getWallpaperFramebuffer (), sx0, sy0, sx1, sy1, seq);
    }
    // GL textures are bottom-up; the bridge/plasma pipeline is top-down.
    // Inverting the blit's source rectangle flips on the GPU for free.
    return g_readback.submit (wallpaper->getWallpaperFramebuffer (), sx0, sy1, sx1, sy0, seq);
}


// Locates the wallpaper_engine "assets" folder derived from the project path
// (.../steamapps/workshop/content/431960/<id> -> .../steamapps/common/wallpaper_engine/assets).
::std::optional<::std::filesystem::path> findAssetsDir (const ::std::filesystem::path& projectDir) {
    const char* env = ::getenv ("ANISPAPER_WE_ASSETS");
    if (env && *env && ::std::filesystem::is_directory (env)) {
        return ::std::filesystem::path (env);
    }
    ::std::filesystem::path current = projectDir.lexically_normal ();
    while (current.has_parent_path () && current.filename () != ::std::string ("steamapps")) {
        current = current.parent_path ();
    }
    if (current.filename () == ::std::string ("steamapps")) {
        const auto candidate = current / "common" / "wallpaper_engine" / "assets";
        if (::std::filesystem::is_directory (candidate)) {
            return candidate;
        }
    }
    const char* home = ::getenv ("HOME");
    if (home) {
        const auto candidate = ::std::filesystem::path (home) / ".local/share/Steam" / "steamapps"
            / "common" / "wallpaper_engine" / "assets";
        if (::std::filesystem::is_directory (candidate)) {
            return candidate;
        }
    }
    return ::std::nullopt;
}

void stdinCommandLoop (ApplicationContext* context) {
    char buffer [4096];
    ::std::string pending;
    while (context->state.general.keepRunning) {
        pending.clear ();
        const ssize_t count = ::read (STDIN_FILENO, buffer, sizeof (buffer));
        if (count <= 0) {
            ::std::this_thread::sleep_for (::std::chrono::milliseconds (50));
            continue;
        }
        pending.append (buffer, static_cast<size_t> (count));
        if (pending.find ("\"pause\"") != ::std::string::npos) {
            g_paused = true;
        } else if (pending.find ("\"resume\"") != ::std::string::npos) {
            g_paused = false;
        } else if (pending.find ("\"stop\"") != ::std::string::npos) {
            context->state.general.keepRunning = false;
        }
    }
}

void signalHandler (int) {
    if (g_app) {
        g_app->signal (SIGTERM);
    }
}
} // namespace

int main (int argc, char* argv []) {
    // The upstream engine may parse a malformed Workshop project from an
    // internal callback/thread.  Letting that reach std::terminate turns a
    // useful nlohmann diagnostic into SIGABRT and deprives the parent of its
    // structured fatal event.  The child is disposable, so emit the bounded
    // JSON protocol error and exit; the parent watchdog then retains a static
    // bridge fallback and applies its normal retry/safe-mode policy.
    ::std::set_terminate ([] {
        try {
            const ::std::exception_ptr pending = ::std::current_exception ();
            if (pending) ::std::rethrow_exception (pending);
        } catch (const ::std::exception& error) {
            emitFatal (::std::string ("uncaught scene exception: ") + error.what ());
        } catch (...) {
            emitFatal ("uncaught non-standard scene exception");
        }
        ::fflush (nullptr);
        ::_Exit (1);
    });
    sLog.addOutput (new ::std::ostream (::std::cerr.rdbuf ()));
    sLog.addError (new ::std::ostream (::std::cerr.rdbuf ()));

    // Parse our own child protocol arguments first.
    ::std::filesystem::path projectDir;
    int width = 1920, height = 1080, fps = 30;
    ::std::string scaling = "fill";
    for (int i = 1; i < argc; ++i) {
        const ::std::string arg = argv [i];
        if (arg == "--file" && i + 1 < argc) {
            projectDir = argv [++i];
        } else if (arg == "--width" && i + 1 < argc) {
            width = ::std::stoi (argv [++i]);
        } else if (arg == "--height" && i + 1 < argc) {
            height = ::std::stoi (argv [++i]);
        } else if (arg == "--fps" && i + 1 < argc) {
            fps = ::std::stoi (argv [++i]);
        } else if (arg == "--scaling" && i + 1 < argc) {
            scaling = argv [++i];
        }
    }

    if (projectDir.empty () || !::std::filesystem::is_directory (projectDir) || width < 64 || height < 64) {
        emitFatal ("invalid renderer child arguments");
        return 2;
    }

    if (::std::getenv ("ANISPAPER_TEST_CRASH_ON_START") != nullptr) {
        ::kill (::getpid (), SIGKILL);
    }

    const auto assetsDir = findAssetsDir (projectDir);
    if (!assetsDir) {
        emitFatal ("cannot locate wallpaper_engine assets directory (set ANISPAPER_WE_ASSETS)");
        return 2;
    }

    // Build the upstream engine context from a synthesized command line.
    fps = ::std::clamp (fps, 1, 60);
    ::std::vector<::std::string> engineArgs {
        "anis-paper-scene-engine",
        "--silent",
        "--fps",
        ::std::to_string (fps),
        "--window",
        "0x0x" + ::std::to_string (width) + "x" + ::std::to_string (height),
        "--scaling",
        scaling,
        "--assets-dir",
        assetsDir->string (),
        projectDir.string (),
    };
    // Fullscreen pause stays enabled by default: when a fullscreen app (game)
    // covers the screen the daemon already stops polling, and the engine will
    // additionally stop rendering altogether.  ANISPAPER_SCENE_NO_FULLSCREEN_PAUSE=1
    // restores the debug-only behavior of rendering regardless.
    if (::getenv ("ANISPAPER_SCENE_NO_FULLSCREEN_PAUSE")) {
        engineArgs.insert (engineArgs.begin () + 2, "--no-fullscreen-pause");
    }
    // Debug-only: ANISPAPER_SCENE_SCREENSHOT=<file.png> asks the upstream
    // engine to write the frame through its own CPU screenshot path.  That
    // path is independent of our GPU capture, so it is the ground truth used
    // to validate crop/scale/orientation of the SHM frames.
    if (const char* shot = ::getenv ("ANISPAPER_SCENE_SCREENSHOT"); shot && *shot) {
        engineArgs.insert (engineArgs.begin () + 2, "--screenshot");
        engineArgs.insert (engineArgs.begin () + 3, shot);
        engineArgs.insert (engineArgs.begin () + 4, "--screenshot-delay");
        engineArgs.insert (engineArgs.begin () + 5, "5");
    }

    ::std::vector<char*> engineArgv;
    for (auto& arg : engineArgs) {
        engineArgv.push_back (arg.data ());
    }

    try {
        ApplicationContext context (static_cast<int> (engineArgv.size ()), engineArgv.data ());
        context.loadSettingsFromArgv ();
        // AnisPaper patch: keep the GLFW window hidden; we read the wallpaper framebuffer.
        context.settings.render.offscreen = true;

        WallpaperApplication app (context);
        g_app = &app;
        ::std::signal (SIGTERM, signalHandler);
        ::std::signal (SIGINT, signalHandler);

        ::std::thread stdinThread (stdinCommandLoop, &context);
        stdinThread.detach ();

        const bool debug = ::getenv ("ANISPAPER_SCENE_DEBUG") != nullptr;
        // ANISPAPER_SCENE_JPEG=1 forces the legacy JPEG/base64 diagnostic path.
        const bool jpegFallback = ::getenv ("ANISPAPER_SCENE_JPEG") != nullptr;
        uint64_t frameNo = 0;
        uint64_t submitSeq = 0;
        bool readyEmitted = false;
        bool readbackReady = false;
        bool useTransport = false;
        ::std::vector<unsigned char> fallbackPixels;  // JPEG fallback only
        ::std::vector<unsigned char> fallbackRgb;     // JPEG fallback only

        app.setFrameCallback ([&] (WallpaperApplication& application) {
            g_profiler.begin (StageProfiler::Clock::now ());
            constexpr int kWarmupFrames = 10;
            ++frameNo;
            if (!readyEmitted) {
                // Give the engine a handful of warm-up frames so textures,
                // shaders and particle systems are fully initialized.
                if (frameNo <= kWarmupFrames) {
                    return;
                }
                readyEmitted = true;
                // The scratch FBO and PBO ring must be created on the render
                // thread, once the GL context is fully initialized.
                readbackReady = g_readback.initialize (width, height);
                useTransport = readbackReady && !jpegFallback
                    && g_transport.create (width, height);
                if (useTransport) {
                    emitJson ("{\"event\":\"transport\",\"path\":\"" + g_transport.name
                              + "\",\"width\":" + ::std::to_string (width)
                              + ",\"height\":" + ::std::to_string (height)
                              + ",\"stride\":" + ::std::to_string (width * 4)
                              + ",\"buffers\":" + ::std::to_string (kTransportBuffers) + "}");
                } else {
                    ::fprintf (stderr, "scene-engine: binary transport unavailable, "
                                       "falling back to JPEG over JSON\n");
                    ::fflush (stderr);
                }
                emitReady ("scene");
                return;
            }
            if (g_paused) {
                return;
            }
            // 1) GPU crop/flip/scale + asynchronous readPixels submit.
            const auto t0 = StageProfiler::Clock::now ();
            captureFrame (application, width, height, ++submitSeq);
            const auto t1 = StageProfiler::Clock::now ();
            // 2) The oldest readback (one frame old) should already be done.
            if (!g_readback.frontReady ()) {
                g_profiler.sample (g_profiler.submitMs, t0, t1);
                g_profiler.sample (g_profiler.waitMs, t1, t1);
                g_profiler.end (t1);
                return;
            }
            const uint64_t readSeq = g_readback.frontSeq ();
            unsigned char* dst = useTransport
                ? g_transport.slotFor (readSeq)
                : (fallbackPixels.resize (static_cast<size_t> (width) * height * 4),
                   fallbackPixels.data ());
            const auto t2 = StageProfiler::Clock::now ();
            const bool copied = g_readback.consume (dst);
            const auto t3 = StageProfiler::Clock::now ();
            if (copied) {
                if (useTransport) {
                    g_transport.finishWrite (readSeq);
                    emitJson ("{\"event\":\"frame\",\"shm\":true,\"seq\":"
                              + ::std::to_string (readSeq) + "}");
                } else {
                    const auto tj0 = StageProfiler::Clock::now ();
                    const size_t count = static_cast<size_t> (width) * height;
                    fallbackRgb.resize (count * 3);
                    for (size_t i = 0; i < count; ++i) {
                        fallbackRgb [i * 3 + 0] = fallbackPixels [i * 4 + 0];
                        fallbackRgb [i * 3 + 1] = fallbackPixels [i * 4 + 1];
                        fallbackRgb [i * 3 + 2] = fallbackPixels [i * 4 + 2];
                    }
                    const ::std::string jpeg = jpegEncode (fallbackRgb.data (), width, height, 82);
                    const ::std::string b64 = base64Encode (
                        reinterpret_cast<const unsigned char*> (jpeg.data ()), jpeg.size ());
                    const auto tj1 = StageProfiler::Clock::now ();
                    emitJson ("{\"event\":\"frame\",\"jpeg\":\"" + b64
                              + "\",\"width\":" + ::std::to_string (width)
                              + ",\"height\":" + ::std::to_string (height)
                              + ",\"fallback\":false}");
                    const auto tj2 = StageProfiler::Clock::now ();
                    g_profiler.sample (g_profiler.jpegMs, tj0, tj1);
                    g_profiler.sample (g_profiler.b64Ms, tj1, tj2);
                }
            }
            const auto t4 = StageProfiler::Clock::now ();
            g_profiler.sample (g_profiler.submitMs, t0, t1);
            g_profiler.sample (g_profiler.waitMs, t1, t2);
            g_profiler.sample (g_profiler.copyMs, t2, t3);
            g_profiler.sample (g_profiler.ipcWriteMs, t3, t4);
            g_profiler.end (t4);
            if (debug && frameNo % 30 == 0) {
                const size_t bytes = static_cast<size_t> (width) * height * 4;
                const size_t step = bytes / 4000 ? bytes / 4000 : 1;
                const uint64_t hash = fnv1aSample (dst, bytes, step);
                ::fprintf (stderr, "scene-engine frame=%llu pixelHash=%016llx\n",
                           static_cast<unsigned long long> (readSeq),
                           static_cast<unsigned long long> (hash));
                ::fflush (stderr);
            }
        });

        app.show ();
        g_readback.shutdown ();
        g_transport.destroy ();
        ::fflush (nullptr);
        return 0;
    } catch (const ::std::exception& e) {
        g_transport.destroy ();
        emitFatal (::std::string ("scene initialization failed: ") + e.what ());
        return 1;
    } catch (...) {
        g_transport.destroy ();
        emitFatal ("scene initialization failed with a non-standard exception");
        return 1;
    }
}
