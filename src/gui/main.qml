import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.mauikit.controls as Maui

Maui.ApplicationWindow {
    id: root

    title: qsTr("Nitrux Update Tool System")

    // Debug flags for testing UI states
    readonly property bool debugMode: true  // Set to true to test update UI
    readonly property bool debugUpdateAvailable: debugMode ? true : nutsClient.updateAvailable
    readonly property string debugUpdateVersion: debugMode ? "6.0.0" : nutsClient.updateVersion
    readonly property string debugUpdateSize: debugMode ? "1.5 GB" : nutsClient.updateSize
    readonly property string debugUpdateNotes: debugMode ? "Nitrux 6.0.0 combines the latest software updates, bug fixes, and performance improvements with ready-to-use hardware support. This release continues the evolution of our immutable foundation.\n\n• Improved system stability\n\n• Updated kernel to 6.18.6\n\n• Performance improvements\n\n• Bug fixes and security updates\n\n [Learn more...](https://nxos.org/changelog/release-announcement-nitrux-6-0-0/)" : nutsClient.updateNotes

    // These properties enable compositor-level transparency
    color: "transparent"
    background: null

    Maui.WindowBlur {
        view: root
        geometry: Qt.rect(0, 0, root.width, root.height)
        windowRadius: Maui.Style.radiusV
        enabled: true
    }

    Rectangle {
        anchors.fill: parent
        color: Maui.Theme.backgroundColor
        opacity: 0.76
        radius: Maui.Style.radiusV
        border.color: Qt.rgba(1, 1, 1, 0)
        border.width: 1
    }

    Maui.Page {
        id: mainPage
        anchors.fill: parent
        headerMargins: Maui.Style.contentMargins
        background: null

        headBar.visible: true

        headBar.leftContent: [
            Button {
                display: AbstractButton.IconOnly
                icon.name: "folder-download"
                enabled: nutsClient.connected && !nutsClient.busy
                onClicked: updateDialog.open()

                ToolTip.delay: 500
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Update System")
            },

            Button {
                display: AbstractButton.IconOnly
                icon.name: "system-help"
                enabled: nutsClient.connected && !nutsClient.busy
                onClicked: rescueDialog.open()

                ToolTip.delay: 500
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Rescue")
            }
        ]

        headBar.rightContent: [
            Button {
                display: AbstractButton.IconOnly
                icon.name: "view-refresh"
                enabled: nutsClient.connected && !nutsClient.busy
                onClicked: nutsClient.checkForUpdates()

                ToolTip.delay: 500
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Check for Updates")
            },

            ToolSeparator {
                bottomPadding: 10
                topPadding: 10
            },

            Maui.ToolButtonMenu {
                icon.name: "overflow-menu"

                MenuItem {
                    text: qsTr("About")
                    icon.name: "help-about"
                    onTriggered: Maui.App.aboutDialog()
                }

                MenuItem {
                    text: qsTr("Quit")
                    icon.name: "application-exit"
                    onTriggered: Qt.quit()
                }
            }
        ]

        Maui.ScrollColumn {
            anchors.fill: parent
            spacing: Maui.Style.space.big
            visible: nutsClient.busy

            Maui.SectionHeader {
                Layout.fillWidth: true
                text1: qsTr("System Status")
                text2: qsTr("Current system information and update status.")
            }

            Maui.FlexSectionItem {
                label1.text: qsTr("Distribution")
                label2.text: nutsClient.distributionInfo || qsTr("Unknown")
                visible: nutsClient.distributionInfo !== ""
            }

            Maui.FlexSectionItem {
                label1.text: qsTr("Status")
                label2.text: nutsClient.statusMessage || qsTr("Ready to update")
                label2.color: nutsClient.connected ? Maui.Theme.positiveTextColor : Maui.Theme.neutralTextColor
            }

            ProgressBar {
                Layout.fillWidth: true
                value: nutsClient.progressPercentage / 100.0
            }

            Label {
                Layout.fillWidth: true
                text: nutsClient.progressMessage
                wrapMode: Text.WordWrap
                visible: text !== ""
            }

            Button {
                Layout.alignment: Qt.AlignHCenter
                text: qsTr("Cancel Operation")
                icon.name: "process-stop"
                onClicked: nutsClient.cancel()
            }
        }

        Maui.Holder {
            anchors.fill: parent
            visible: !nutsClient.busy && !debugUpdateAvailable
            emoji: nutsClient.connected ? "dialog-ok-apply" : "network-disconnect"
            title: nutsClient.connected
                   ? qsTr("No Updates Available")
                   : qsTr("Helper Disconnected")
            body: nutsClient.connected
                  ? qsTr("Your system is up to date.\nClick 'Check for Updates' to manually check again.")
                  : qsTr("The update helper service is not running.")
        }

        // iOS-style update details view
        Maui.ScrollColumn {
            anchors.fill: parent
            spacing: Maui.Style.space.big
            visible: !nutsClient.busy && debugUpdateAvailable

            // Update info with section header and button
            RowLayout {
                Layout.fillWidth: true
                spacing: Maui.Style.space.medium

                Maui.SectionHeader {
                    Layout.fillWidth: true
                    text1: qsTr("Software Update")
                    text2: qsTr("A new system update is available.")
                }

                Button {
                    text: qsTr("Update Now")
                    onClicked: updateDialog.open()
                }
            }

            // Version and size info
            RowLayout {
                Layout.fillWidth: true
                spacing: Maui.Style.space.medium

                Maui.Icon {
                    source: "live-installer"
                    implicitHeight: Maui.Style.iconSizes.big
                    implicitWidth: Maui.Style.iconSizes.big
                }

                Maui.SectionHeader {
                    Layout.fillWidth: true
                    text1: qsTr("Nitrux ") + debugUpdateVersion
                    text2: debugUpdateSize
                }
            }

            // Release notes
            Label {
                Layout.fillWidth: true
                Layout.leftMargin: Maui.Style.space.big
                Layout.rightMargin: Maui.Style.space.big
                text: debugUpdateNotes
                wrapMode: Text.WordWrap
                textFormat: Text.MarkdownText
            }
        }
    }

    Maui.InfoDialog {
        id: updateDialog
        title: qsTr("System Update")
        message: qsTr("A backup will be created, and the system will reboot automatically after updating.\n\nContinue?")
        standardButtons: Dialog.Yes | Dialog.No
        onAccepted: nutsClient.performUpdate()
    }

    Maui.InfoDialog {
        id: rescueDialog
        title: qsTr("System Restore")
        message: qsTr("Restoring will overwrite current system data. Ensure you are running from a Live session.\n\nContinue?")
        standardButtons: Dialog.Yes | Dialog.No
        onAccepted: nutsClient.performRescue()
    }

    Maui.InfoDialog {
        id: completionDialog
        property bool success: true
        property string dialogMessage: ""
        title: success ? qsTr("Success") : qsTr("Completed")
        message: completionDialog.dialogMessage
        standardButtons: Dialog.Ok
    }

    Maui.InfoDialog {
        id: errorDialog
        property string errorMessage: ""
        title: qsTr("Error")
        message: errorMessage
        standardButtons: Dialog.Ok
    }

    Connections {
        target: nutsClient
        function onOperationCompleted(success, message) {
            completionDialog.success = success
            completionDialog.dialogMessage = message
            completionDialog.open()
        }
        function onOperationFailed(error) {
            errorDialog.errorMessage = error
            errorDialog.open()
        }
    }

    Component.onCompleted: {
        nutsClient.refreshSystemInfo()
    }
}
