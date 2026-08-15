#pragma once

#include <glm/vec4.hpp>
#include <chrono>
#include <map>
#include <string>
#include <vector>

#include "FullScreenDetector.h"
#include "WallpaperEngine/Render/Drivers/VideoDriver.h"
#include <X11/Xlib.h>

namespace WallpaperEngine::Render::Drivers {
class GLFWOpenGLDriver;

namespace Detectors {
    class X11FullScreenDetector final : public FullScreenDetector {
    public:
	X11FullScreenDetector (Application::ApplicationContext& appContext, VideoDriver& driver);
	~X11FullScreenDetector () override;

	[[nodiscard]] bool anythingFullscreen () const override;
	void reset () override;

    private:
	void initialize ();
	void stop ();

	Display* m_display = nullptr;
	Window m_root;
	std::map<std::string, glm::ivec4> m_screens = {};
	VideoDriver& m_driver;
	// XQueryTree/XGetWindowAttributes are synchronous X11 round trips.  The
	// wallpaper only needs to react within the existing 250 ms fullscreen
	// pause cadence, so avoid issuing them on every render frame.
	mutable std::chrono::steady_clock::time_point m_cacheUntil {};
	mutable bool m_cachedFullscreen = false;
	};
} // namespace Detectors
} // namespace WallpaperEngine::Render::Drivers
