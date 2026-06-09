import QtQuick 2.12
import Ubuntu.Components 1.3
import xonotic 1.0

MainView {
    id: root
    applicationName: "xonotic.ratrad"
    width: units.gu(45)
    height: units.gu(75)

    Launcher {
        id: gameLauncher
    }

    Page {
        anchors.fill: parent

        header: PageHeader {
            title: i18n.tr("Xonotic")
        }

        Column {
            anchors.centerIn: parent
            width: parent.width - units.gu(4)
            spacing: units.gu(2)

            Label {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: i18n.tr("Launch the bundled Xonotic client.")
            }

            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                text: i18n.tr("Play")
                color: UbuntuColors.warmGrey
                onClicked: {
                    if (!gameLauncher.launch()) {
                        notification.show(i18n.tr("Could not start Xonotic."))
                    }
                }
            }
        }
    }

    Notification {
        id: notification
    }
}
