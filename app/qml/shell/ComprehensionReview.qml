pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Independent comprehension/review surface for a substantial rewrite.
// The component distinguishes not_run / running / ready / stale / failed and
// never presents partial or unreviewed text as applicable. Applicability is
// gated by hostController.canApplyReviewedOutput, not by local optimism.
FocusScope {
    id: root
    objectName: "comprehensionReview"
    property string currentTheme: "dark"
    property bool reducedMotion: false
    property bool proposalAvailable: true

    signal applyRequested()
    signal returnToInterviewRequested()
    signal keepOriginalRequested()

    CompanionContract { id: contract }
    RocTheme { id: rocTheme; mode: root.currentTheme }
    readonly property string monoFont: Qt.platform.os === "osx" ? "Menlo" : "Consolas"
    readonly property string sansFont: "Inter Variable"
    readonly property color errorColor: "#EEA4AF"
    readonly property string reviewState: contract.reviewSurfaceAvailable ? contract.comprehensionState : "not_run"
    readonly property bool ready: contract.reviewSurfaceAvailable && reviewState === "ready"
    readonly property int decisionCount: contract.unresolvedDecisionCount
    readonly property int findingCount: contract.comprehensionFindings.length
    readonly property bool applyAllowed: root.proposalAvailable && contract.reviewSurfaceAvailable &&
                                         root.reviewState === "ready" && contract.canApplyReviewedOutput

    implicitWidth: column.implicitWidth + 24
    implicitHeight: column.implicitHeight + 24

    function reviewStateTitle(value) {
        switch (value) {
        case "not_run": return qsTr("Not independently reread");
        case "running": return qsTr("Independent reread in progress");
        case "ready": return qsTr("Independent reread complete");
        case "stale": return qsTr("This reread is out of date");
        case "failed": return qsTr("The independent reread failed");
        default: return qsTr("Review unavailable");
        }
    }
    function reviewStateDetail(value) {
        switch (value) {
        case "not_run":
            return qsTr("The rewrite has not been read back against your draft, decisions and quoted constraints yet. Apply is unavailable until that happens.");
        case "running":
            return qsTr("A separate local pass is rereading the original draft, your confirmed answers and the proposed rewrite. Apply stays unavailable until it finishes.");
        case "ready":
            return qsTr("The proposed text was reread against your confirmed wording. Resolve any finding below before applying.");
        case "stale":
            return qsTr("The draft, an answer or the proposal changed after this reread. Nothing from the earlier review can be applied.");
        case "failed":
            return qsTr("The reread failed closed. No text was approved, and the proposal is not applicable.");
        default:
            return "";
        }
    }
    function reviewStateTone(value) {
        switch (value) {
        case "ready": return rocTheme.success;
        case "running": return rocTheme.focus;
        case "stale": return rocTheme.warning;
        case "failed": return root.errorColor;
        default: return rocTheme.muted;
        }
    }
    function reviewStateBadge(value) {
        switch (value) {
        case "not_run": return qsTr("NOT RUN");
        case "running": return qsTr("REREADING");
        case "ready": return qsTr("READY");
        case "stale": return qsTr("STALE");
        case "failed": return qsTr("FAILED");
        default: return qsTr("UNKNOWN");
        }
    }
    function severityLabel(value) {
        switch (value) {
        case "blocking": return qsTr("BLOCKING");
        case "required": return qsTr("REQUIRED");
        case "advisory": return qsTr("ADVISORY");
        default: return qsTr("NOTE");
        }
    }
    function severityTone(value) {
        switch (value) {
        case "blocking": return root.errorColor;
        case "required": return rocTheme.warning;
        default: return rocTheme.muted;
        }
    }
    function kindLabel(kind) {
        switch (kind) {
        case "comprehension": return qsTr("Reread gap");
        case "omission": return qsTr("Possible omission");
        case "meaning_drift": return qsTr("Meaning may have drifted");
        case "unsupported_assumption": return qsTr("Unsupported assumption");
        case "negative_constraint": return qsTr("Constraint at risk");
        case "readiness": return qsTr("Task readiness");
        default: return qsTr("Finding");
        }
    }
    function applyReason() {
        if (!root.proposalAvailable) return qsTr("Not applicable: there is no current rewrite.");
        if (!contract.reviewSurfaceAvailable) return qsTr("Independent reread is unavailable on this host.");
        switch (reviewState) {
        case "not_run": return qsTr("Not applicable: this rewrite has not been independently reread.");
        case "running": return qsTr("Not applicable until the reread finishes.");
        case "stale": return qsTr("Not applicable: the reread belongs to an earlier draft.");
        case "failed": return qsTr("Not applicable: the reread failed.");
        }
        if (!contract.canApplyReviewedOutput) {
            if (root.decisionCount > 0)
                return qsTr("Resolve %1 finding(s) that need your decision first.").arg(root.decisionCount);
            if (contract.blockingFindingCount > 0)
                return qsTr("A blocking finding must be resolved before applying.");
            return qsTr("Review is complete, but the host still locks apply. Try rereading the current rewrite.");
        }
        return "";
    }
    function requestReview() { contract.requestReview(); }
    function resolve(finding, resolution) {
        if (!finding || !contract.reviewInvokable) return;
        contract.resolveFinding(finding.id, resolution);
        if (resolution === "return_to_interview") root.returnToInterviewRequested();
    }

    component TinyButton: RocButton {
        currentTheme: root.currentTheme
        reduceMotion: root.reducedMotion
        compact: true
    }

    component PulseDots: Row {
        id: dots
        property bool running: false
        property bool reduceMotion: false
        spacing: 3
        Repeater {
            model: 3
            Rectangle {
                required property int index
                id: pixel
                width: 5; height: 5
                radius: 1
                color: rocTheme.focus
                property real pulseOpacity: 0.35
                opacity: pulseOpacity
                SequentialAnimation {
                    running: dots.running && !dots.reduceMotion
                    loops: Animation.Infinite
                    PauseAnimation { duration: 140 * pixel.index }
                    NumberAnimation { target: pixel; property: "pulseOpacity"; to: 1.0; duration: 260 }
                    NumberAnimation { target: pixel; property: "pulseOpacity"; to: 0.35; duration: 280 }
                }
            }
        }
    }

    Rectangle {
        id: card
        objectName: "comprehensionReviewCard"
        anchors.fill: parent
        radius: 12
        color: "transparent"
        border.color: root.reviewStateTone(root.reviewState)
        border.width: 1

        ColumnLayout {
            id: column
            x: 12; y: 12
            width: Math.max(0, parent.width - 24)
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                spacing: 6
                Text {
                    text: qsTr("INDEPENDENT REREAD")
                    font.family: root.monoFont
                    font.pixelSize: 9
                    font.bold: true
                    color: rocTheme.foreground
                }
                Rectangle {
                    implicitWidth: badgeLabel.implicitWidth + 10
                    implicitHeight: 16
                    radius: 3
                    color: "transparent"
                    border.color: root.reviewStateTone(root.reviewState)
                    border.width: 1
                    Text {
                        id: badgeLabel
                        objectName: "comprehensionStateBadge"
                        anchors.centerIn: parent
                        text: root.reviewStateBadge(root.reviewState)
                        font.family: root.monoFont
                        font.pixelSize: 8
                        font.bold: true
                        color: root.reviewStateTone(root.reviewState)
                    }
                }
                Row {
                    visible: root.reviewState === "running"
                    spacing: 5
                    PulseDots { running: root.reviewState === "running"; reduceMotion: root.reducedMotion }
                }
                Item { Layout.fillWidth: true; Layout.minimumWidth: 0 }
                TinyButton {
                    objectName: "comprehensionRequestControl"
                    visible: root.reviewState === "not_run" || root.reviewState === "stale" || root.reviewState === "failed"
                    label: root.reviewState === "not_run" ? qsTr("Reread this prompt")
                                                     : root.reviewState === "stale" ? qsTr("Reread current")
                                                                              : qsTr("Try again")
                    active: contract.reviewInvokable
                    Accessible.name: qsTr("Run the independent reread again")
                    onClicked: root.requestReview()
                }
            }

            Text {
                objectName: "comprehensionStateHeadline"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                textFormat: Text.PlainText
                text: root.reviewStateTitle(root.reviewState)
                wrapMode: Text.WordWrap
                font.family: root.sansFont
                font.pixelSize: 15
                font.bold: true
                color: root.reviewStateTone(root.reviewState)
            }
            Text {
                objectName: "comprehensionStateDetail"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                textFormat: Text.PlainText
                text: root.reviewStateDetail(root.reviewState)
                wrapMode: Text.WordWrap
                font.family: root.sansFont
                font.pixelSize: 12
                color: rocTheme.muted
            }

            Text {
                Layout.fillWidth: true
                visible: root.reviewState === "not_run" || root.reviewState === "running" || root.reviewState === "stale" ||
                         root.reviewState === "failed"
                text: qsTr("The proposed text above is not applicable in this state. Your original draft is unchanged.")
                wrapMode: Text.WordWrap
                font.family: root.sansFont
                font.pixelSize: 11
                color: rocTheme.warning
            }

            // Reviewer interpretation: explicitly non-authoritative.
            ColumnLayout {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                visible: root.ready && contract.comprehensionInterpretation.length > 0
                spacing: 3
                Text {
                    text: qsTr("REREADER'S READING · NOT AUTHORITATIVE")
                    font.family: root.monoFont
                    font.pixelSize: 8
                    color: rocTheme.muted
                }
                Text {
                    objectName: "comprehensionInterpretation"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    textFormat: Text.PlainText
                    text: "“" + contract.comprehensionInterpretation + "”"
                    wrapMode: Text.WordWrap
                    font.family: root.sansFont
                    font.pixelSize: 13
                    font.italic: true
                    color: rocTheme.foreground
                }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("A second local read can share the same misreading. It is evidence for your decision, not a guarantee.")
                    wrapMode: Text.WordWrap
                    font.family: root.sansFont
                    font.pixelSize: 9
                    color: rocTheme.muted
                }
            }

            // Findings.
            ColumnLayout {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                visible: root.ready
                spacing: 6
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    Text {
                        text: qsTr("FINDINGS")
                        font.family: root.monoFont
                        font.pixelSize: 8
                        font.bold: true
                        color: rocTheme.muted
                    }
                    Text {
                        objectName: "comprehensionFindingCount"
                        Layout.fillWidth: true
                        text: qsTr("%1 · %2 need a decision").arg(root.findingCount).arg(root.decisionCount)
                        font.family: root.monoFont
                        font.pixelSize: 8
                        color: rocTheme.muted
                    }
                }
                Text {
                    objectName: "comprehensionNoFindings"
                    Layout.fillWidth: true
                    visible: root.findingCount === 0
                    text: qsTr("No unresolved findings. The reread did not find a gap in this rewrite.")
                    wrapMode: Text.WordWrap
                    font.family: root.sansFont
                    font.pixelSize: 12
                    color: rocTheme.success
                }
                Repeater {
                    model: root.ready ? contract.comprehensionFindings : []
                    delegate: Rectangle {
                        id: findingRow
                        required property var modelData
                        required property int index
                        readonly property string severity: modelData.severity !== undefined
                                                                  ? String(modelData.severity) : "advisory"
                        readonly property string excerpt: modelData.sourceExcerpt !== undefined
                                                              ? String(modelData.sourceExcerpt) : ""
                        readonly property string sourceRef: modelData.sourceRef !== undefined
                                                                ? String(modelData.sourceRef) : ""
                        readonly property string repair: modelData.proposedRepair !== undefined
                                                             ? String(modelData.proposedRepair) : ""
                        readonly property bool needsDecision: modelData.requiresUserDecision === true
                        objectName: "comprehensionFinding_" + modelData.id
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        implicitHeight: findingColumn.implicitHeight + 18
                        radius: 8
                        color: rocTheme.inset
                        border.color: root.severityTone(findingRow.severity)
                        border.width: 1
                        ColumnLayout {
                            id: findingColumn
                            x: 9; y: 8
                            width: Math.max(0, parent.width - 18)
                            spacing: 4
                            RowLayout {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                spacing: 6
                                Text {
                                    objectName: "comprehensionFindingSeverity_" + index
                                    text: root.severityLabel(findingRow.severity)
                                    font.family: root.monoFont
                                    font.pixelSize: 8
                                    font.bold: true
                                    color: root.severityTone(findingRow.severity)
                                }
                                Text {
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    text: root.kindLabel(modelData.kind)
                                    font.family: root.monoFont
                                    font.pixelSize: 8
                                    color: rocTheme.muted
                                }
                                Text {
                                    visible: findingRow.needsDecision
                                    text: qsTr("YOUR DECISION")
                                    font.family: root.monoFont
                                    font.pixelSize: 8
                                    color: rocTheme.warning
                                }
                            }
                            Text {
                                objectName: "comprehensionFindingSummary_" + index
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                textFormat: Text.PlainText
                                text: modelData.summary
                                wrapMode: Text.WordWrap
                                font.family: root.sansFont
                                font.pixelSize: 13
                                color: rocTheme.foreground
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                visible: findingRow.excerpt.length > 0 || findingRow.sourceRef.length > 0
                                spacing: 2
                                Text {
                                    objectName: "comprehensionFindingExcerpt_" + index
                                    Layout.fillWidth: true
                                    Layout.minimumWidth: 0
                                    visible: findingRow.excerpt.length > 0
                                    textFormat: Text.PlainText
                                    text: "“" + findingRow.excerpt + "”"
                                    wrapMode: Text.WrapAnywhere
                                    font.family: root.monoFont
                                    font.pixelSize: 10
                                    color: rocTheme.foreground
                                }
                                Text {
                                    objectName: "comprehensionFindingSource_" + index
                                    Layout.fillWidth: true
                                    visible: findingRow.sourceRef.length > 0
                                    text: findingRow.sourceRef
                                    wrapMode: Text.WrapAnywhere
                                    font.family: root.monoFont
                                    font.pixelSize: 9
                                    color: rocTheme.muted
                                }
                            }
                            Text {
                                Layout.fillWidth: true
                                visible: findingRow.repair.length > 0
                                textFormat: Text.PlainText
                                text: qsTr("Suggested repair (not applied automatically): ") + findingRow.repair
                                wrapMode: Text.WordWrap
                                font.family: root.sansFont
                                font.pixelSize: 11
                                color: rocTheme.muted
                            }
                            Flow {
                                Layout.fillWidth: true
                                spacing: 6
                                visible: findingRow.needsDecision || findingRow.severity !== "blocking"
                                TinyButton {
                                    objectName: "comprehensionFindingReturn_" + index
                                    visible: findingRow.needsDecision
                                    label: qsTr("Return to interview")
                                    active: contract.reviewInvokable
                                    onClicked: root.resolve(findingRow.modelData, "return_to_interview")
                                }
                                TinyButton {
                                    objectName: "comprehensionFindingAcknowledge_" + index
                                    // A blocking finding must be decided, not acknowledged away.
                                    visible: findingRow.severity !== "blocking"
                                    label: qsTr("Acknowledge")
                                    quiet: true
                                    active: contract.reviewInvokable
                                    onClicked: root.resolve(findingRow.modelData, "acknowledge")
                                }
                                TinyButton {
                                    objectName: "comprehensionFindingKeep_" + index
                                    visible: findingRow.needsDecision
                                    label: qsTr("Keep original here")
                                    quiet: true
                                    active: contract.reviewInvokable
                                    onClicked: root.resolve(findingRow.modelData, "keep_original")
                                }
                            }
                            Text {
                                Layout.fillWidth: true
                                visible: findingRow.severity === "blocking"
                                text: qsTr("Blocking findings cannot be acknowledged into applicability; choose a resolution.")
                                wrapMode: Text.WordWrap
                                font.family: root.sansFont
                                font.pixelSize: 9
                                color: root.errorColor
                            }
                        }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 1
                color: rocTheme.border
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                spacing: 8
                TinyButton {
                    objectName: "comprehensionApplyControl"
                    visible: root.applyAllowed
                    label: qsTr("Apply reviewed rewrite")
                    primary: true
                    active: root.applyAllowed
                    Accessible.name: qsTr("Apply the independently reread rewrite")
                    onClicked: if (root.applyAllowed) root.applyRequested()
                }
                TinyButton {
                    objectName: "comprehensionKeepOriginalControl"
                    label: qsTr("Keep original")
                    quiet: true
                    active: root.proposalAvailable
                    onClicked: root.keepOriginalRequested()
                }
                Item { Layout.fillWidth: true }
            }
            Text {
                objectName: "comprehensionApplyReason"
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                visible: text.length > 0
                textFormat: Text.PlainText
                text: root.applyReason()
                wrapMode: Text.WordWrap
                font.family: root.sansFont
                font.pixelSize: 10
                color: root.applyAllowed ? rocTheme.success : rocTheme.warning
            }
        }
    }
}
