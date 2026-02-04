// SPDX-License-Identifier: BSD-3-Clause
// Copyright 2026 Nitrux Latinoamericana S.C.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.mauikit.controls as Maui

Maui.ApplicationWindow {
    id: root

    title: qsTr("Nitrux Update Tool System")

    Maui.Page {
        id: mainPage
        anchors.fill: parent

        headBar.visible: true
        headBar.leftContent: Maui.ToolButtonMenu {
            icon.name: "application-menu"

            MenuItem {
                text: qsTr("About")
                icon.name: "help-about"
                onTriggered: aboutDialog.open()
            }

            MenuItem {
                text: qsTr("Quit")
                icon.name: "application-exit"
                onTriggered: Qt.quit()
            }
        }

        headBar.rightContent: Label {
            text: nutsClient.distributionInfo
            visible: nutsClient.distributionInfo !== ""
        }

        ColumnLayout {
            anchors.centerIn: parent
            width: Math.min(parent.width * 0.8, 600)
            spacing: Maui.Style.space.big

            // Icon
            Maui.Icon {
                Layout.alignment: Qt.AlignHCenter
                source: "system-software-update"
                implicitHeight: 128
                implicitWidth: 128
            }

            // Title
            Label {
                Layout.alignment: Qt.AlignHCenter
                text: qsTr("Nitrux Update Tool System")
                font.pointSize: Maui.Style.fontSizes.huge
                font.bold: true
            }

            // Status message
            Label {
                Layout.alignment: Qt.AlignHCenter
                Layout.fillWidth: true
                text: nutsClient.statusMessage || qsTr("Ready to update")
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                visible: text !== ""
            }

            // Progress bar
            ProgressBar {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignHCenter
                value: nutsClient.progressPercentage / 100.0
                visible: nutsClient.busy
            }

            // Progress message
            Label {
                Layout.alignment: Qt.AlignHCenter
                text: nutsClient.progressMessage
                color: Maui.Theme.textColor
                visible: nutsClient.busy && text !== ""
            }

            // Buttons
            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                spacing: Maui.Style.space.medium

                Button {
                    text: qsTr("Update System")
                    icon.name: "system-software-update"
                    enabled: nutsClient.connected && !nutsClient.busy
                    onClicked: {
                        updateDialog.open()
                    }
                }

                Button {
                    text: qsTr("Rescue")
                    icon.name: "system-reboot"
                    enabled: nutsClient.connected && !nutsClient.busy
                    onClicked: {
                        rescueDialog.open()
                    }
                }

                Button {
                    text: qsTr("Self-Update")
                    icon.name: "view-refresh"
                    enabled: nutsClient.connected && !nutsClient.busy
                    onClicked: {
                        selfUpdateDialog.open()
                    }
                }
            }

            // Cancel button (shown when busy)
            Button {
                Layout.alignment: Qt.AlignHCenter
                text: qsTr("Cancel")
                icon.name: "process-stop"
                visible: nutsClient.busy
                onClicked: nutsClient.cancel()
            }

            // Connection status
            Label {
                Layout.alignment: Qt.AlignHCenter
                text: nutsClient.connected ? qsTr("Connected to helper") : qsTr("Disconnected from helper")
                color: nutsClient.connected ? Maui.Theme.positiveTextColor : Maui.Theme.negativeTextColor
                font.pointSize: Maui.Style.fontSizes.small
            }
        }
    }

    // Update confirmation dialog
    Maui.Dialog {
        id: updateDialog

        title: qsTr("Confirm Update")
        message: qsTr("This will create a backup of your system and download the latest update. The system will reboot automatically after the update completes.\n\nDo you want to continue?")

        standardButtons: Dialog.Yes | Dialog.No

        onAccepted: {
            nutsClient.performUpdate()
        }
    }

    // Rescue confirmation dialog
    Maui.Dialog {
        id: rescueDialog

        title: qsTr("Confirm Rescue")
        message: qsTr("This operation will restore your system from a backup. This should only be done from a Live session if your system is not bootable.\n\nDo you want to continue?")

        standardButtons: Dialog.Yes | Dialog.No

        onAccepted: {
            nutsClient.performRescue()
        }
    }

    // Self-update confirmation dialog
    Maui.Dialog {
        id: selfUpdateDialog

        title: qsTr("Confirm Self-Update")
        message: qsTr("This will update the NUTS tool itself to the latest version from GitHub. You will need to reboot to load the changes.\n\nDo you want to continue?")

        standardButtons: Dialog.Yes | Dialog.No

        onAccepted: {
            nutsClient.performSelfUpdate()
        }
    }

    // About dialog
    Maui.AboutDialog {
        id: aboutDialog

        appName: "NUTS"
        appDescription: qsTr("Nitrux Update Tool System")
        appIcon: "system-software-update"
        version: "3.0.0"
        vendor: "Nitrux Latinoamericana S.C."
        website: "https://nxos.org"
        licenses: ["BSD-3-Clause"]
        credits: [
            {
                name: "Uri Herrera",
                email: "uri_herrera@nxos.org",
                year: "2023-2026"
            },
            {
                name: "Luis Lavaire",
                email: "luis_lavaire@nxos.org",
                year: "2023"
            }
        ]
    }

    // Handle completion/failure
    Connections {
        target: nutsClient

        function onOperationCompleted(success, message) {
            completionDialog.success = success
            completionDialog.message = message
            completionDialog.open()
        }

        function onOperationFailed(error) {
            errorDialog.errorMessage = error
            errorDialog.open()
        }
    }

    // Completion dialog
    Maui.Dialog {
        id: completionDialog

        property bool success: true
        property string message: ""

        title: success ? qsTr("Success") : qsTr("Completed")
        message: completionDialog.message

        standardButtons: Dialog.Ok
    }

    // Error dialog
    Maui.Dialog {
        id: errorDialog

        property string errorMessage: ""

        title: qsTr("Error")
        message: errorMessage

        standardButtons: Dialog.Ok
    }

    Component.onCompleted: {
        nutsClient.refreshSystemInfo()
    }
}
