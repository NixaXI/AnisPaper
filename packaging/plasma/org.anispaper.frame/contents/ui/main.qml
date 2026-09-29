import QtQuick
import QtQuick.Window
import org.kde.plasma.plasmoid
import "org/anispaper/frame" as AnisPaperFrame

WallpaperItem {
    id: root

    AnisPaperFrame.FrameBridgeSupport { id: bridgeSupport }

    readonly property string frameOutput: {
        const configured = String(root.configuration.Output || "").trim()
        const current = String(Screen.name || "").trim()
        // Configuration written by a different display backend can name a
        // connector that no longer exists (for example HDMI-A-1 on Wayland
        // versus HDMI-A-0 on X11). Each WallpaperItem belongs to one screen;
        // use its live connector when the stored value is stale.
        return current.length > 0 && configured !== current ? current
             : configured.length > 0 ? configured : current
    }
    readonly property real frameNo: watcher.frameNo
    readonly property string scaleMode: {
        const requested = String(root.configuration.ScaleMode || "cover").trim().toLowerCase()
        return requested === "fit" || requested === "stretch" ? requested : "cover"
    }

    Rectangle {
        anchors.fill: parent
        color: "#0A0D14"
    }

    Image {
        id: bridgeImage
        anchors.fill: parent
        fillMode: root.scaleMode === "stretch" ? Image.Stretch
                : root.scaleMode === "fit" ? Image.PreserveAspectFit
                : Image.PreserveAspectCrop
        smooth: true
        mipmap: false
        cache: false
        asynchronous: false
        horizontalAlignment: Image.AlignHCenter
        verticalAlignment: Image.AlignVCenter
        source: "image://anispaper/" +
                (root.frameOutput.length > 0 ? root.frameOutput : "unknown") +
                "?f=" + root.frameNo
        Component.onCompleted: {
            const parts = String(Qt.version).split(".")
            if (Number(parts[0]) > 6 || (Number(parts[0]) === 6 && Number(parts[1]) >= 5))
                retainWhileLoading = true
        }
    }

    AnisPaperFrame.FrameWatcher {
        id: watcher
        output: root.frameOutput
    }
}
