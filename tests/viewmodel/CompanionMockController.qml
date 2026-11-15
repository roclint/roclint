import QtQuick

// TEST DOUBLE ONLY — not shipped, not linked into the app resource.
//
// Mirrors the frozen Host/UI v1 property and invokable names so the QML
// components can be exercised without Agent 2's host implementation. The
// production components bind the real hostController context property; this
// object is registered under that same name by test_product_alignment.cpp and
// records every invocation for assertions. It intentionally contains no
// generated text or product intelligence.
QtObject {
    id: mock

    property int stateVersion: 11
    property var calls: []

    function record(kind, detail) {
        const next = calls.slice();
        next.push({ kind: kind, detail: detail });
        calls = next;
    }
    function clearCalls() { calls = [] }
    function callCount() { return calls.length }
    function lastCallKind() { return calls.length > 0 ? calls[calls.length - 1].kind : "" }
    function lastCallDetail() { return calls.length > 0 ? calls[calls.length - 1].detail : ({}) }

    // ------------------------------------------------- word surface (existing)
    property bool hasWordRefinement: false
    property string wordRefinementOriginal: ""
    property string wordRefinementReplacement: ""
    property string wordRefinementExplanation: ""
    property var wordRefinementAlternatives: []
    property var wordRefinementCandidates: []
    property string wordRefinementSelectedCandidateId: ""
    property string wordRefinementAssumption: ""
    property bool wordRefinementNeedsClarification: false
    property string wordRefinementClarificationQuestion: ""
    property bool wordRefinementApplying: false
    property var wordRefinementAnchorRect: Qt.rect(120, 240, 48, 18)

    function applyWordRefinement(expectedVersion, candidateId) {
        record("applyWord", { version: expectedVersion, candidateId: String(candidateId) });
        return stateVersion;
    }
    function selectWordRefinementCandidate(candidateId) {
        record("selectWord", { candidateId: String(candidateId) });
    }
    function dismissWordRefinement() { record("dismissWord", {}) }
    function exploreInCompanion() { record("explore", {}) }

    // ----------------------------------------------------- intent/interview
    property bool hasIntentBrief: false
    property var intentBriefItems: []
    property var intentOpenChoiceIds: []
    property string intentBriefSummary: ""
    property bool isInterviewActive: false
    property bool canExploreInterview: true
    property int interviewTurnsCompleted: 0
    property string interviewQuestion: ""
    property string interviewConsequence: ""
    property var interviewChoices: []
    property bool isInterviewBusy: false
    property string interviewFeedback: ""

    function openInterviewFromCompanion(expectedVersion) {
        record("openInterview", { version: expectedVersion });
        return stateVersion;
    }
    function submitInterviewResponse(text, disposition, expectedVersion) {
        record("submitInterview", {
            text: String(text),
            disposition: String(disposition),
            version: expectedVersion
        });
        return stateVersion;
    }
    function changeIntentDecision(itemId, newText, expectedVersion) {
        record("changeDecision", {
            itemId: String(itemId),
            text: String(newText),
            version: expectedVersion
        });
        return stateVersion;
    }
    function keepOriginalPrompt(expectedVersion) {
        record("keepOriginal", { version: expectedVersion });
        return stateVersion;
    }
    function rewriteWithoutQuestions(expectedVersion) {
        record("rewriteWithoutQuestions", { version: expectedVersion });
        return stateVersion;
    }

    // -------------------------------------------------------- comprehension
    property string comprehensionReviewState: "not_run"
    property string comprehensionInterpretation: ""
    property var comprehensionFindings: []
    property bool canApplyReviewedOutput: false

    function requestComprehensionReview(expectedVersion) {
        record("requestReview", { version: expectedVersion });
        return stateVersion;
    }
    function resolveComprehensionFinding(findingId, resolution, expectedVersion) {
        record("resolveFinding", {
            id: String(findingId),
            resolution: String(resolution),
            version: expectedVersion
        });
        return stateVersion;
    }
}
