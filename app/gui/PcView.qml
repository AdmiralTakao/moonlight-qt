import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3

import ComputerModel 1.0

import ComputerManager 1.0
import StreamingPreferences 1.0
import SystemProperties 1.0
import SdlGamepadKeyNavigation 1.0

CenteredGridView {
    property ComputerModel computerModel : createModel()

    id: pcGrid
    focus: true
    activeFocusOnTab: true
    topMargin: 20
    bottomMargin: 5
    cellWidth: 310; cellHeight: 330;
    objectName: qsTr("Computers")

    Component.onCompleted: {
        // Don't show any highlighted item until interacting with them.
        // We do this here instead of onActivated to avoid losing the user's
        // selection when backing out of a different page of the app.
        currentIndex = -1
    }

    // Note: Any initialization done here that is critical for streaming must
    // also be done in CliStartStreamSegue.qml, since this code does not run
    // for command-line initiated streams.
    StackView.onActivated: {
        // Setup signals on CM
        ComputerManager.computerAddCompleted.connect(addComplete)

        // Highlight the first item if a gamepad is connected
        if (currentIndex === -1 && SdlGamepadKeyNavigation.getConnectedGamepads() > 0) {
            currentIndex = 0
        }
    }

    StackView.onDeactivating: {
        ComputerManager.computerAddCompleted.disconnect(addComplete)
    }

    function pairingComplete(error)
    {
        // Close the PIN dialog
        pairDialog.close()

        // Display a failed dialog if we got an error
        if (error !== undefined) {
            errorDialog.text = error
            errorDialog.helpText = ""
            errorDialog.open()
        }
    }

    function addComplete(success, detectedPortBlocking)
    {
        if (!success) {
            errorDialog.text = qsTr("Unable to connect to the specified PC.")

            if (detectedPortBlocking) {
                errorDialog.text += "\n\n" + qsTr("This PC's Internet connection is blocking Moonlight. Streaming over the Internet may not work while connected to this network.")
            }
            else {
                errorDialog.helpText = qsTr("Click the Help button for possible solutions.")
            }

            errorDialog.open()
        }
    }

    function createModel()
    {
        var model = Qt.createQmlObject('import ComputerModel 1.0; ComputerModel {}', parent, '')
        model.initialize(ComputerManager)
        model.pairingCompleted.connect(pairingComplete)
        model.connectionTestCompleted.connect(testConnectionDialog.connectionTestComplete)
        model.wakeUrlCalled.connect(wakeAndConnectDialog.wakeUrlCalled)
        return model
    }

    // GFN fork: "Start mit einem Tipp" - opens the app list (or pairing) for an online PC
    function openComputer(index, name, paired, serverSupported)
    {
        if (!serverSupported) {
            errorDialog.text = qsTr("The version of GeForce Experience on %1 is not supported by this build of Moonlight. You must update Moonlight to stream from %1.").arg(name)
            errorDialog.helpText = ""
            errorDialog.open()
        }
        else if (paired) {
            // go to game view
            var component = Qt.createComponent("AppView.qml")
            var appView = component.createObject(stackView, {"computerIndex": index, "objectName": name})
            stackView.push(appView)
        }
        else {
            var pin = computerModel.generatePinString()

            // Kick off pairing in the background
            computerModel.pairComputer(index, pin)

            // Display the pairing dialog
            pairDialog.pin = pin
            pairDialog.open()
        }
    }

    function startWakeAndConnect(index, name, wakeable)
    {
        wakeAndConnectDialog.pcIndex = index
        wakeAndConnectDialog.pcName = name
        wakeAndConnectDialog.wakeable = wakeable
        wakeAndConnectDialog.open()
    }

    Row {
        anchors.centerIn: parent
        spacing: 5
        visible: pcGrid.count === 0

        BusyIndicator {
            id: searchSpinner
            visible: StreamingPreferences.enableMdns
            running: visible
        }

        Label {
            height: searchSpinner.height
            elide: Label.ElideRight
            text: StreamingPreferences.enableMdns ? qsTr("Searching for compatible hosts on your local network...")
                                                  : qsTr("Automatic PC discovery is disabled. Add your PC manually.")
            font.pointSize: 20
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.Wrap
        }
    }

    model: computerModel

    delegate: NavigableItemDelegate {
        width: 300; height: 320;
        grid: pcGrid

        property alias pcContextMenu : pcContextMenuLoader.item

        Image {
            id: pcIcon
            anchors.horizontalCenter: parent.horizontalCenter
            source: "qrc:/res/desktop_windows-48px.svg"
            sourceSize {
                width: 200
                height: 200
            }
        }

        Image {
            // TODO: Tooltip
            id: stateIcon
            anchors.horizontalCenter: pcIcon.horizontalCenter
            anchors.verticalCenter: pcIcon.verticalCenter
            anchors.verticalCenterOffset: !model.online ? -18 : -16
            visible: !model.statusUnknown && (!model.online || !model.paired)
            source: !model.online ? "qrc:/res/warning_FILL1_wght300_GRAD200_opsz24.svg" : "qrc:/res/baseline-lock-24px.svg"
            sourceSize {
                width: !model.online ? 75 : 70
                height: !model.online ? 75 : 70
            }
        }

        BusyIndicator {
            id: statusUnknownSpinner
            anchors.horizontalCenter: pcIcon.horizontalCenter
            anchors.verticalCenter: pcIcon.verticalCenter
            anchors.verticalCenterOffset: -15
            width: 75
            height: 75
            visible: model.statusUnknown
            running: visible
        }

        Label {
            id: pcNameText
            text: model.name

            width: parent.width
            anchors.top: pcIcon.bottom
            anchors.bottom: parent.bottom
            font.pointSize: 30
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            elide: Text.ElideRight
        }

        // Display a tooltip with the full name if it's truncated
        ToolTip.text: model.name
        ToolTip.delay: 1000
        ToolTip.timeout: 5000
        ToolTip.visible: (hovered || highlighted) && pcNameText.truncated

        Loader {
            id: pcContextMenuLoader
            asynchronous: true
            sourceComponent: NavigableMenu {
                id: pcContextMenu
                initiator: pcContextMenuLoader.parent
                MenuItem {
                    text: qsTr("PC Status: %1").arg(model.online ? qsTr("Online") : qsTr("Offline"))
                    font.bold: true
                    enabled: false
                }
                NavigableMenuItem {
                    text: qsTr("View All Apps")
                    onTriggered: {
                        var component = Qt.createComponent("AppView.qml")
                        var appView = component.createObject(stackView, {"computerIndex": index, "objectName": model.name, "showHiddenGames": true})
                        stackView.push(appView)
                    }
                    visible: model.online && model.paired
                }
                NavigableMenuItem {
                    text: qsTr("Wake PC")
                    onTriggered: computerModel.wakeComputer(index)
                    visible: !model.online && model.wakeable
                }
                NavigableMenuItem {
                    text: "Wecken und verbinden"
                    onTriggered: pcGrid.startWakeAndConnect(index, model.name, model.wakeable)
                    visible: !model.online && (model.wakeable || model.wakeUrl !== "")
                }
                NavigableMenuItem {
                    text: "Weck-Adresse festlegen…"
                    onTriggered: {
                        wakeUrlDialog.pcIndex = index
                        wakeUrlDialog.pcName = model.name
                        wakeUrlDialog.currentUrl = model.wakeUrl
                        wakeUrlDialog.open()
                    }
                }
                NavigableMenuItem {
                    text: qsTr("Test Network")
                    onTriggered: {
                        computerModel.testConnectionForComputer(index)
                        testConnectionDialog.open()
                    }
                }

                NavigableMenuItem {
                    text: qsTr("Rename PC")
                    onTriggered: {
                        renamePcDialog.pcIndex = index
                        renamePcDialog.originalName = model.name
                        renamePcDialog.open()
                    }
                }
                NavigableMenuItem {
                    text: qsTr("Delete PC")
                    onTriggered: {
                        deletePcDialog.pcIndex = index
                        deletePcDialog.pcName = model.name
                        deletePcDialog.open()
                    }
                }
                NavigableMenuItem {
                    text: qsTr("View Details")
                    onTriggered: {
                        showPcDetailsDialog.pcDetails = model.details
                        showPcDetailsDialog.open()
                    }
                }
            }
        }

        onClicked: {
            if (model.online) {
                pcGrid.openComputer(index, model.name, model.paired, model.serverSupported)
            } else if (!model.statusUnknown && model.wakeUrl !== "") {
                // GFN fork: one tap wakes the PC through its wake address and connects
                pcGrid.startWakeAndConnect(index, model.name, model.wakeable)
            } else if (!model.online) {
                // Using open() here because it may be activated by keyboard
                pcContextMenu.open()
            }
        }

        onPressAndHold: {
            // popup() ensures the menu appears under the mouse cursor
            if (pcContextMenu.popup) {
                pcContextMenu.popup()
            }
            else {
                // Qt 5.9 doesn't have popup()
                pcContextMenu.open()
            }
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.RightButton;
            onClicked: {
                parent.pressAndHold()
            }
        }

        Keys.onMenuPressed: {
            // We must use open() here so the menu is positioned on
            // the ItemDelegate and not where the mouse cursor is
            pcContextMenu.open()
        }

        Keys.onDeletePressed: {
            deletePcDialog.pcIndex = index
            deletePcDialog.pcName = model.name
            deletePcDialog.open()
        }
    }

    ErrorMessageDialog {
        id: errorDialog

        // Using Setup-Guide here instead of Troubleshooting because it's likely that users
        // will arrive here by forgetting to enable GameStream or not forwarding ports.
        helpUrl: "https://github.com/moonlight-stream/moonlight-docs/wiki/Setup-Guide"
    }

    NavigableMessageDialog {
        id: pairDialog
        closePolicy: Popup.CloseOnEscape

        // don't allow edits to the rest of the window while open
        property string pin : "0000"
        text:qsTr("Please enter %1 on your host PC. This dialog will close when pairing is completed.").arg(pin)+"\n\n"+
             qsTr("If your host PC is running Sunshine, navigate to the Sunshine web UI to enter the PIN.")
        standardButtons: Dialog.Cancel
        onRejected: {
            // FIXME: We should interrupt pairing here
        }
    }

    NavigableMessageDialog {
        id: deletePcDialog
        // don't allow edits to the rest of the window while open
        property int pcIndex : -1
        property string pcName : ""
        text: qsTr("Are you sure you want to remove '%1'?").arg(pcName)
        standardButtons: Dialog.Yes | Dialog.No

        onAccepted: {
            computerModel.deleteComputer(pcIndex)
        }
    }

    NavigableMessageDialog {
        id: testConnectionDialog
        closePolicy: Popup.CloseOnEscape
        standardButtons: Dialog.Ok

        onAboutToShow: {
            testConnectionDialog.text = qsTr("Moonlight is testing your network connection to determine if any required ports are blocked.") + "\n\n" + qsTr("This may take a few seconds…")
            showSpinner = true
        }

        function connectionTestComplete(result, blockedPorts)
        {
            if (result === -1) {
                text = qsTr("The network test could not be performed because none of Moonlight's connection testing servers were reachable from this PC. Check your Internet connection or try again later.")
                imageSrc = "qrc:/res/baseline-warning-24px.svg"
            }
            else if (result === 0) {
                text = qsTr("This network does not appear to be blocking Moonlight. If you still have trouble connecting, check your PC's firewall settings.") + "\n\n" + qsTr("If you are trying to stream over the Internet, install the Moonlight Internet Hosting Tool on your gaming PC and run the included Internet Streaming Tester to check your gaming PC's Internet connection.")
                imageSrc = "qrc:/res/baseline-check_circle_outline-24px.svg"
            }
            else {
                text = qsTr("Your PC's current network connection seems to be blocking Moonlight. Streaming over the Internet may not work while connected to this network.") + "\n\n" + qsTr("The following network ports were blocked:") + "\n"
                text += blockedPorts
                imageSrc = "qrc:/res/baseline-error_outline-24px.svg"
            }

            // Stop showing the spinner and show the image instead
            showSpinner = false
        }
    }

    NavigableDialog {
        id: renamePcDialog
        property string label: qsTr("Enter the new name for this PC:")
        property string originalName
        property int pcIndex : -1;

        standardButtons: Dialog.Ok | Dialog.Cancel

        onOpened: {
            // Force keyboard focus on the textbox so keyboard navigation works
            editText.forceActiveFocus()
        }

        onClosed: {
            editText.clear()
        }

        onAccepted: {
            if (editText.text) {
                computerModel.renameComputer(pcIndex, editText.text)
            }
        }

        ColumnLayout {
            Label {
                text: renamePcDialog.label
                font.bold: true
            }

            TextField {
                id: editText
                placeholderText: renamePcDialog.originalName
                Layout.fillWidth: true
                focus: true

                Keys.onReturnPressed: {
                    renamePcDialog.accept()
                }

                Keys.onEnterPressed: {
                    renamePcDialog.accept()
                }
            }
        }
    }

    // GFN fork: wakes the PC (wake address + Wake-on-LAN), waits until it is online and opens it
    NavigableMessageDialog {
        id: wakeAndConnectDialog
        closePolicy: Popup.CloseOnEscape
        standardButtons: Dialog.Cancel

        property int pcIndex : -1
        property string pcName : ""
        property bool wakeable : false
        property int elapsedSeconds : 0
        property string wakeMessage : ""
        readonly property int timeoutSeconds : 180

        function updateText() {
            text = "%1 wird geweckt… (%2 s)".arg(pcName).arg(elapsedSeconds) +
                   (wakeMessage !== "" ? "\n\n" + wakeMessage : "") +
                   "\n\nSobald der PC online ist, geht es automatisch weiter."
        }

        function wakeUrlCalled(computerIndex, success, message) {
            // NB: may arrive while the dialog is still opening (Wake-on-LAN only)
            if (computerIndex !== pcIndex) {
                return
            }
            wakeMessage = message
            updateText()
        }

        onAboutToShow: {
            elapsedSeconds = 0
            wakeMessage = ""
            showSpinner = true
            standardButtons = Dialog.Cancel
            updateText()
            computerModel.wakeAndConnect(pcIndex)
            wakePollTimer.start()
        }

        onClosed: {
            wakePollTimer.stop()
        }

        Timer {
            id: wakePollTimer
            interval: 1000
            repeat: true

            onTriggered: {
                var dialog = wakeAndConnectDialog

                if (computerModel.isComputerOnline(dialog.pcIndex)) {
                    stop()
                    dialog.close()

                    pcGrid.openComputer(dialog.pcIndex,
                                        dialog.pcName,
                                        computerModel.isComputerPaired(dialog.pcIndex),
                                        computerModel.isComputerServerSupported(dialog.pcIndex))
                    return
                }

                dialog.elapsedSeconds++

                if (dialog.elapsedSeconds >= dialog.timeoutSeconds) {
                    stop()
                    dialog.showSpinner = false
                    dialog.imageSrc = "qrc:/res/baseline-error_outline-24px.svg"
                    dialog.text = "%1 ist nach %2 Sekunden nicht online gekommen.".arg(dialog.pcName).arg(dialog.timeoutSeconds) +
                                  (dialog.wakeMessage !== "" ? "\n\n" + dialog.wakeMessage : "") +
                                  "\n\nPrüfe die Weck-Adresse, Wake-on-LAN im BIOS und ob die VPN-Verbindung (z. B. Tailscale) steht."
                    dialog.standardButtons = Dialog.Ok
                    return
                }

                // Repeat Wake-on-LAN now and then; the wake address is only called once
                if (dialog.wakeable && dialog.elapsedSeconds % 15 === 0) {
                    computerModel.wakeComputer(dialog.pcIndex)
                }

                dialog.updateText()
            }
        }
    }

    // GFN fork: per-PC wake address for "Start mit einem Tipp"
    NavigableDialog {
        id: wakeUrlDialog
        property int pcIndex : -1
        property string pcName : ""
        property string currentUrl : ""

        standardButtons: Dialog.Save | Dialog.Cancel

        onOpened: {
            wakeUrlText.text = currentUrl
            wakeUrlText.forceActiveFocus()
        }

        onAccepted: {
            computerModel.setWakeUrl(pcIndex, wakeUrlText.text)
        }

        ColumnLayout {
            Label {
                text: "Weck-Adresse für %1".arg(wakeUrlDialog.pcName)
                font.bold: true
            }

            Label {
                text: "Wird beim Antippen des ausgeschalteten PCs aufgerufen, danach wartet Moonlight, bis er online ist. " +
                      "Beispiele:\n" +
                      "Shelly: http://192.168.178.50/relay/0?turn=on\n" +
                      "Tasmota: http://192.168.178.51/cm?cmnd=Power%20On\n" +
                      "UpSnap/Home Assistant: deren Weck-URL (für Webhooks „POST “ voranstellen)\n" +
                      "Leer lassen zum Entfernen. Die Adresse muss von unterwegs erreichbar sein, z. B. über Tailscale."
                wrapMode: Text.Wrap
                font.pointSize: 9
                Layout.maximumWidth: 450
            }

            TextField {
                id: wakeUrlText
                placeholderText: "http://…"
                Layout.fillWidth: true
                Layout.minimumWidth: 400
                focus: true

                Keys.onReturnPressed: {
                    wakeUrlDialog.accept()
                }

                Keys.onEnterPressed: {
                    wakeUrlDialog.accept()
                }
            }
        }
    }

    NavigableMessageDialog {
        id: showPcDetailsDialog
        property string pcDetails : "";
        text: showPcDetailsDialog.pcDetails
        imageSrc: "qrc:/res/baseline-help_outline-24px.svg"
        standardButtons: Dialog.Ok
    }

    ScrollBar.vertical: ScrollBar {}
}
