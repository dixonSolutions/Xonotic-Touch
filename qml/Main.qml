import QtQuick 2.12
import QtQuick.Window 2.2
import Ubuntu.Components 1.3
import xonotic 1.0
import "./components"

MainView {
    id: root
    applicationName: "xonotic.ratrad"
    width: Screen.width
    height: Screen.height
    anchorToKeyboard: false

    OrientationHelper {
        automaticOrientationAngle: false
        orientationAngle: Orientation.Landscape

        Item {
            id: gameRoot
            anchors.fill: parent

            property int hudHp: 100
            property int hudAmmo: 50
            property int hudScore: 0

            Connections {
                target: Launcher
                onHpChanged: gameRoot.hudHp = hp
                onAmmoChanged: gameRoot.hudAmmo = ammo
            }

            // ─── LOOK ZONE ────────────────────────────────────────────────
            MouseArea {
                id: lookZone
                anchors.fill: parent
                z: 0

                property real lastX: 0
                property real lastY: 0

                onPressed: {
                    lastX = mouse.x
                    lastY = mouse.y
                }
                onPositionChanged: {
                    var dx = mouse.x - lastX
                    var dy = mouse.y - lastY
                    lastX = mouse.x
                    lastY = mouse.y
                    InputDevice.sendLook(dx, dy)
                }
                onDoubleClicked: {
                    InputDevice.pressButton(0, true)
                    flashAnim.restart()
                    btnReleaseTimer.start()
                }
            }

            // Releases BTN_SOUTH after a brief press window
            Timer {
                id: btnReleaseTimer
                interval: 80
                repeat: false
                onTriggered: InputDevice.pressButton(0, false)
            }

            // ─── CROSSHAIR ────────────────────────────────────────────────
            Item {
                anchors.centerIn: parent
                z: 5
                width: 32
                height: 32
                enabled: false

                Rectangle {
                    width: 24; height: 2
                    color: Qt.rgba(1, 1, 1, 0.8)
                    anchors.centerIn: parent
                }
                Rectangle {
                    width: 2; height: 24
                    color: Qt.rgba(1, 1, 1, 0.8)
                    anchors.centerIn: parent
                }
                Rectangle {
                    width: 12; height: 12; radius: 6
                    color: "transparent"
                    border.color: Qt.rgba(1, 1, 1, 0.6)
                    border.width: 1
                    anchors.centerIn: parent
                }
            }

            // ─── GLASS JOYSTICK ──────────────────────────────────────────
            GlassJoystick {
                id: joystick
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                anchors.margins: 22
                z: 20

                onMoved: {
                    var dead = 0.15
                    if (magnitude < dead) {
                        InputDevice.setAxis(0, 0.0)
                        InputDevice.setAxis(1, 0.0)
                        return
                    }
                    // Decompose polar (angle°, magnitude) → Cartesian axes.
                    // angle follows atan2 convention: 0° = right, 90° = down.
                    // axis 0 = ABS_X (strafe left/right), axis 1 = ABS_Y (forward/back).
                    var rad = angle * Math.PI / 180
                    InputDevice.setAxis(0, magnitude * Math.cos(rad))
                    InputDevice.setAxis(1, magnitude * Math.sin(rad))
                }

                onReleased: {
                    InputDevice.setAxis(0, 0.0)
                    InputDevice.setAxis(1, 0.0)
                }
            }

            // ─── HUD PILLS ────────────────────────────────────────────────
            Row {
                id: hudRow
                anchors.bottom: parent.bottom
                anchors.right: parent.right
                anchors.margins: 18
                spacing: 8
                z: 20

                Rectangle {
                    width: 72; height: 30; radius: 15
                    color: Qt.rgba(0, 0, 0, 0.45)
                    border.color: Qt.rgba(1, 1, 1, 0.18); border.width: 0.5
                    Row {
                        anchors.centerIn: parent; spacing: 4
                        Text { text: "❤"; color: Qt.rgba(1, 0.3, 0.3, 0.9); font.pixelSize: 11 }
                        Text { text: gameRoot.hudHp; color: "white"; font.pixelSize: 12; font.bold: true }
                    }
                }

                Rectangle {
                    width: 72; height: 30; radius: 15
                    color: Qt.rgba(0, 0, 0, 0.45)
                    border.color: Qt.rgba(1, 1, 1, 0.18); border.width: 0.5
                    Row {
                        anchors.centerIn: parent; spacing: 4
                        Text { text: "◎"; color: Qt.rgba(1, 0.85, 0.2, 0.9); font.pixelSize: 11 }
                        Text { text: gameRoot.hudAmmo; color: "white"; font.pixelSize: 12; font.bold: true }
                    }
                }

                Rectangle {
                    width: 72; height: 30; radius: 15
                    color: Qt.rgba(0, 0, 0, 0.45)
                    border.color: Qt.rgba(1, 1, 1, 0.18); border.width: 0.5
                    Row {
                        anchors.centerIn: parent; spacing: 4
                        Text { text: "★"; color: Qt.rgba(0.3, 0.7, 1, 0.9); font.pixelSize: 11 }
                        Text { text: gameRoot.hudScore; color: "white"; font.pixelSize: 12; font.bold: true }
                    }
                }
            }

            // ─── SHOOT FLASH ──────────────────────────────────────────────
            Rectangle {
                id: shootFlash
                anchors.fill: parent
                z: 25
                color: "white"
                opacity: 0
                visible: opacity > 0

                SequentialAnimation {
                    id: flashAnim
                    NumberAnimation {
                        target: shootFlash; property: "opacity"
                        from: 0; to: 0.08; duration: 75
                        easing.type: Easing.OutQuad
                    }
                    NumberAnimation {
                        target: shootFlash; property: "opacity"
                        from: 0.08; to: 0; duration: 75
                        easing.type: Easing.InQuad
                    }
                }
            }
        }
    }
}
