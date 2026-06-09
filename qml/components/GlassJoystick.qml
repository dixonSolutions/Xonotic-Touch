import QtQuick 2.12
import Ubuntu.Components 1.3

Item {
    id: root
    width: 140
    height: 140

    // angle  — degrees, atan2 convention: 0° = right, 90° = down
    // magnitude — normalised 0..1
    signal moved(real angle, real magnitude)
    signal released()

    property real thumbX: width  / 2
    property real thumbY: height / 2
    property real maxRadius: 38
    property bool active: false

    // ── Outer glass disc ─────────────────────────────────────────────────
    Rectangle {
        id: outerRing
        anchors.fill: parent
        radius: width / 2
        color: Qt.rgba(1, 1, 1, 0.07)
        border.color: Qt.rgba(1, 1, 1, 0.20)
        border.width: 1

        // Top specular highlight
        Rectangle {
            width: parent.width * 0.55
            height: parent.height * 0.18
            anchors.top: parent.top
            anchors.topMargin: 6
            anchors.horizontalCenter: parent.horizontalCenter
            radius: width / 2
            color: Qt.rgba(1, 1, 1, 0.18)
        }

        // Bottom shadow
        Rectangle {
            width: parent.width * 0.7
            height: parent.height * 0.12
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 8
            anchors.horizontalCenter: parent.horizontalCenter
            radius: width / 2
            color: Qt.rgba(0, 0, 0, 0.15)
        }
    }

    // Travel-limit ring
    Rectangle {
        width: root.maxRadius * 2 + 16
        height: root.maxRadius * 2 + 16
        anchors.centerIn: parent
        radius: width / 2
        color: "transparent"
        border.color: Qt.rgba(1, 1, 1, 0.08)
        border.width: 1
    }

    // ── Thumb cap ────────────────────────────────────────────────────────
    Rectangle {
        id: thumb
        width: 44
        height: 44
        radius: 22
        x: root.thumbX - width  / 2
        y: root.thumbY - height / 2
        color: root.active ? Qt.rgba(1, 1, 1, 0.30) : Qt.rgba(1, 1, 1, 0.14)
        border.color: Qt.rgba(1, 1, 1, root.active ? 0.50 : 0.28)
        border.width: 1

        Behavior on color { ColorAnimation { duration: 80 } }

        // Specular highlight on thumb cap
        Rectangle {
            width: parent.width * 0.5
            height: parent.height * 0.25
            anchors.top: parent.top
            anchors.topMargin: 5
            anchors.horizontalCenter: parent.horizontalCenter
            radius: width / 2
            color: Qt.rgba(1, 1, 1, 0.35)
        }

        // Centre dot
        Rectangle {
            width: 6; height: 6; radius: 3
            anchors.centerIn: parent
            color: Qt.rgba(1, 1, 1, 0.5)
        }

        // Spring-return animation when thumb is released
        Behavior on x {
            enabled: !root.active
            SpringAnimation { spring: 4; damping: 0.6 }
        }
        Behavior on y {
            enabled: !root.active
            SpringAnimation { spring: 4; damping: 0.6 }
        }
    }

    // ── Touch handler ────────────────────────────────────────────────────
    MultiPointTouchArea {
        anchors.fill: parent
        minimumTouchPoints: 1
        maximumTouchPoints: 1

        onPressed: {
            root.active = true
        }

        onUpdated: {
            var t   = touchPoints[0]
            var cx  = root.width  / 2
            var cy  = root.height / 2
            var dx  = t.x - cx
            var dy  = t.y - cy
            var dist    = Math.sqrt(dx * dx + dy * dy)
            var clamped = Math.min(dist, root.maxRadius)
            var angle   = Math.atan2(dy, dx) * 180 / Math.PI
            var mag     = clamped / root.maxRadius
            var scale   = clamped / (dist + 0.001)

            root.thumbX = cx + dx * scale
            root.thumbY = cy + dy * scale
            root.moved(angle, mag)
        }

        onReleased: {
            root.active = false
            root.thumbX = root.width  / 2
            root.thumbY = root.height / 2
            root.released()
        }
    }
}
