import QtQuick
import QtQuick.Window
import org.kde.plasma.plasmoid
import "org/anispaper/frame" as AnisPaperFrame

WallpaperItem {
    id: root

    // Instantiating the marker guarantees the local QML extension has added
    // image://anispaper to this Plasma engine before Image resolves its source.
    AnisPaperFrame.FrameBridgeSupport { id: bridgeSupport }

    // Plasma can set Output explicitly per containment. When it is unset, the
    // physical Qt screen name gives each monitor its own shm bridge by default.
    readonly property string frameOutput: {
        const configured = String(root.configuration.Output || "").trim()
        return configured.length > 0 ? configured : Screen.name
    }
    // Push-driven: watcher.frameNo only changes when the daemon really
    // published a new bridge frame, so the Image source (and thus the
    // expensive copy + texture rebuild) updates at the true publication rate
    // instead of on a fixed GUI timer that re-uploads every tick.
    // `real` keeps the 64-bit sequence exact past 2^31 frames.
    readonly property real frameNo: watcher.frameNo
    // Native FrameHeader geometry, read from the bridge header by the watcher.
    // Deriving it from bridgeImage.implicitWidth (as this did before) is both a
    // binding cycle -- sourceClipRect defines the implicit size -- and wrong at
    // startup, where the implicit size belongs to the provider's 1280x720
    // fallback and produced a visibly stretched first paint.
    readonly property bool frameReady: watcher.frameWidth > 0 && watcher.frameHeight > 0
    readonly property real frameWidth: frameReady ? watcher.frameWidth : Math.max(1, width)
    readonly property real frameHeight: frameReady ? watcher.frameHeight : Math.max(1, height)
    readonly property real frameAspect: frameWidth / frameHeight
    readonly property real itemAspect: width > 0 && height > 0 ? width / height : frameAspect
    // The image provider exposes the native FrameHeader dimensions.  Never
    // implicitly stretch that frame to a logical-size WallpaperItem: calculate
    // source and destination rectangles from both aspect ratios instead.
    readonly property string scaleMode: {
        const requested = String(root.configuration.ScaleMode || "cover").trim().toLowerCase()
        return requested === "fit" || requested === "stretch" ? requested : "cover"
    }

    // cover crops source pixels around the centre; fit letterboxes on the
    // #0A0D14 backdrop; stretch is the only deliberate non-aspect mode.
    // Before the first real frame the clip rect stays null so the provider's
    // fallback is shown through PreserveAspectCrop at its own aspect ratio.
    readonly property rect sourceRect: {
        if (!frameReady || scaleMode !== "cover" || frameWidth <= 0 || frameHeight <= 0
                || itemAspect <= 0)
            return Qt.rect(0, 0, 0, 0)
        if (frameAspect > itemAspect) {
            const cropWidth = frameHeight * itemAspect
            return Qt.rect((frameWidth - cropWidth) / 2, 0, cropWidth, frameHeight)
        }
        const cropHeight = frameWidth / itemAspect
        return Qt.rect(0, (frameHeight - cropHeight) / 2, frameWidth, cropHeight)
    }
    readonly property rect destRect: {
        if (!frameReady || scaleMode === "cover" || scaleMode === "stretch" ||
                frameWidth <= 0 || frameHeight <= 0 || width <= 0 || height <= 0)
            return Qt.rect(0, 0, width, height)
        const factor = Math.min(width / frameWidth, height / frameHeight)
        const destinationWidth = frameWidth * factor
        const destinationHeight = frameHeight * factor
        return Qt.rect((width - destinationWidth) / 2, (height - destinationHeight) / 2,
                       destinationWidth, destinationHeight)
    }

    Rectangle {
        anchors.fill: parent
        color: "#0A0D14"
    }

    Item {
        anchors.fill: parent
        clip: true

        Image {
            id: bridgeImage
            x: root.destRect.x
            y: root.destRect.y
            width: root.destRect.width
            height: root.destRect.height
            sourceClipRect: root.sourceRect
            // sourceRect/destRect preserve the ratio for cover and fit, so
            // Stretch is only a true stretch when the user selected it.  Until
            // the first bridge frame arrives there is no clip rect and the
            // source is the provider's own-aspect fallback: stretching that to
            // the item is exactly the startup distortion, so crop instead.
            fillMode: root.frameReady ? Image.Stretch : Image.PreserveAspectCrop
            // Bridge frames arrive at native output resolution and are drawn at
            // (or below) 1:1, so mipmapping is pure cost and linear filtering is
            // all the scene graph needs.
            smooth: true
            mipmap: false
            cache: false
            asynchronous: false
            // retainWhileLoading is Qt 6.5+.  Assigning it in the object
            // literal makes Ubuntu 24.04 (Qt 6.4) refuse to load the wallpaper.
            Component.onCompleted: {
                const parts = String(Qt.version).split(".")
                if (Number(parts[0]) > 6 || (Number(parts[0]) === 6 && Number(parts[1]) >= 5))
                    retainWhileLoading = true
            }
            horizontalAlignment: Image.AlignHCenter
            verticalAlignment: Image.AlignVCenter
            source: "image://anispaper/" +
                    (root.frameOutput.length > 0 ? root.frameOutput : "unknown") +
                    "?f=" + root.frameNo
        }
    }

    // Push-driven source updates: FrameWatcher polls the ANIS header (~60 Hz,
    // header-only, ~3 syscalls/tick) and emits frameNoChanged only when the
    // daemon published a new frame.
    AnisPaperFrame.FrameWatcher {
        id: watcher
        output: root.frameOutput
    }
}
