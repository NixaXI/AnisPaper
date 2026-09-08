import QtQuick
import QtQuick.Window
import org.kde.plasma.plasmoid
import "org/anispaper/frame" as AnisPaperFrame

WallpaperItem {
    id: root

    AnisPaperFrame.FrameBridgeSupport { id: bridgeSupport }

    readonly property string frameOutput: {
        const configured = String(root.configuration.Output || "").trim()
        return configured.length > 0 ? configured : Screen.name
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
