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
    // The image provider exposes the native FrameHeader dimensions.  Never
    // implicitly stretch that frame to a logical-size WallpaperItem: calculate
    // source and destination rectangles from both aspect ratios instead.
    readonly property string scaleMode: {
        const requested = String(root.configuration.ScaleMode || "cover").trim().toLowerCase()
        return requested === "fit" || requested === "stretch" ? requested : "cover"
    }
    readonly property real frameWidth: bridgeImage.implicitWidth > 0
                                       ? bridgeImage.implicitWidth : Math.max(1, width)
    readonly property real frameHeight: bridgeImage.implicitHeight > 0
                                        ? bridgeImage.implicitHeight : Math.max(1, height)
    readonly property real frameAspect: frameWidth / frameHeight
    readonly property real itemAspect: width > 0 && height > 0 ? width / height : frameAspect

    // cover crops source pixels around the centre; fit letterboxes on the
    // #0A0D14 backdrop; stretch is the only deliberate non-aspect mode.
    readonly property rect sourceRect: {
        if (scaleMode !== "cover" || frameWidth <= 0 || frameHeight <= 0 || itemAspect <= 0)
            return Qt.rect(0, 0, frameWidth, frameHeight)
        if (frameAspect > itemAspect) {
            const cropWidth = frameHeight * itemAspect
            return Qt.rect((frameWidth - cropWidth) / 2, 0, cropWidth, frameHeight)
        }
        const cropHeight = frameWidth / itemAspect
        return Qt.rect(0, (frameHeight - cropHeight) / 2, frameWidth, cropHeight)
    }
    readonly property rect destRect: {
        if (scaleMode === "cover" || scaleMode === "stretch" ||
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
            // sourceRect/destRect preserve the ratio for cover and fit.  This
            // fill mode is therefore only a true stretch when explicitly
            // selected above.
            fillMode: Image.Stretch
            smooth: true
            mipmap: false
            cache: false
            asynchronous: false
            // Keep the last uploaded texture while the next provider request
            // is in flight.  The frame source changes at publication rate;
            // clearing Image on every URL change exposes the #0A0D14 backing
            // rectangle for a tick when Plasma's scene graph is busy.
            retainWhileLoading: true
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
