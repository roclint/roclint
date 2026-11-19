import QtQuick

// TEST DOUBLE ONLY — pre-contract host shape.
//
// Exposes only the word-refinement surface that exists on the baseline
// HostController. It verifies that CompanionContract hides the interview and
// review surfaces (instead of binding missing fields) and that Explore keeps
// the established HUD route.
QtObject {
    id: mock

    property int stateVersion: 3
    property var calls: []
    function record(kind, detail) {
        const next = calls.slice();
        next.push({ kind: kind, detail: detail });
        calls = next;
    }
    function lastCallKind() { return calls.length > 0 ? calls[calls.length - 1].kind : "" }

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
    property var wordRefinementAnchorRect: Qt.rect(40, 80, 24, 18)

    function applyWordRefinement(expectedVersion, candidateId) {
        record("applyWord", { version: expectedVersion, candidateId: String(candidateId) });
        return stateVersion;
    }
    function selectWordRefinementCandidate(candidateId) {
        record("selectWord", { candidateId: String(candidateId) });
    }
    function dismissWordRefinement() { record("dismissWord", {}) }
    function exploreInCompanion() { record("explore", {}) }
}
