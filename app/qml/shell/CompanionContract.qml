import QtQuick

// Read-only adapter over the frozen Host/UI contract v1
// (handoffs/DeepSeek/D00_interface_contract_2026-11-15.md).
//
// The shell must keep loading on hosts that have not implemented the
// intent/interview/comprehension surfaces yet. Every access is therefore
// guarded by an availability flag; when a field is absent the dependent UI
// is hidden rather than filled with invented data. Values are direct
// HostController property reads so notify signals keep QML bindings live.
QtObject {
    id: contract

    readonly property var host: (typeof hostController !== "undefined" && hostController !== null)
                                 ? hostController : null
    readonly property bool present: host !== null

    function has(name) {
        return host !== null && typeof host[name] !== "undefined";
    }
    function callable(name) {
        return host !== null && typeof host[name] === "function";
    }

    readonly property bool stateVersionAvailable: present && has("stateVersion")
    readonly property int stateVersion: stateVersionAvailable ? host.stateVersion : 0

    // ---------------------------------------------------------------- word
    readonly property bool wordSurfaceAvailable: present && has("hasWordRefinement") &&
                                                 has("wordRefinementOriginal") &&
                                                 has("wordRefinementReplacement") &&
                                                 has("wordRefinementExplanation") &&
                                                 has("wordRefinementCandidates") &&
                                                 has("wordRefinementSelectedCandidateId") &&
                                                 has("wordRefinementAssumption") &&
                                                 has("wordRefinementNeedsClarification") &&
                                                 has("wordRefinementClarificationQuestion") &&
                                                 has("wordRefinementApplying")

    readonly property bool hasWordRefinement: wordSurfaceAvailable ? host.hasWordRefinement : false
    readonly property string wordOriginal: wordSurfaceAvailable ? host.wordRefinementOriginal : ""
    readonly property string wordReplacement: wordSurfaceAvailable ? host.wordRefinementReplacement : ""
    readonly property string wordExplanation: wordSurfaceAvailable ? host.wordRefinementExplanation : ""
    readonly property var wordCandidates: wordSurfaceAvailable ? host.wordRefinementCandidates : []
    readonly property string wordSelectedCandidateId: wordSurfaceAvailable
                                                        ? host.wordRefinementSelectedCandidateId : ""
    readonly property string wordAssumption: wordSurfaceAvailable ? host.wordRefinementAssumption : ""
    readonly property bool wordNeedsClarification: wordSurfaceAvailable ? host.wordRefinementNeedsClarification : false
    readonly property string wordClarificationQuestion: wordSurfaceAvailable
                                                          ? host.wordRefinementClarificationQuestion : ""
    readonly property bool wordApplying: wordSurfaceAvailable ? host.wordRefinementApplying : false
    readonly property bool wordHasProposal: wordReplacement.length > 0 || wordCandidates.length > 0
    readonly property bool wordWorking: hasWordRefinement && !wordHasProposal && !wordNeedsClarification &&
                                        !wordApplying
    readonly property bool wordCanApply: hasWordRefinement && wordReplacement.length > 0 &&
                                         !wordNeedsClarification && !wordApplying
    readonly property bool anchorAvailable: wordSurfaceAvailable && has("wordRefinementAnchorRect")
    readonly property var wordAnchorRect: anchorAvailable ? host.wordRefinementAnchorRect : Qt.rect(0, 0, 0, 0)

    function selectWordCandidate(candidateId) {
        if (wordSurfaceAvailable && callable("selectWordRefinementCandidate"))
            host.selectWordRefinementCandidate(candidateId);
    }
    function applyWord(candidateId) {
        if (!wordCanApply || !callable("applyWordRefinement")) return 0;
        return host.applyWordRefinement(host.stateVersion, candidateId);
    }
    function dismissWord() {
        if (wordSurfaceAvailable && callable("dismissWordRefinement"))
            host.dismissWordRefinement();
    }

    // ------------------------------------------------------- intent/interview
    readonly property bool intentSurfaceAvailable: present && has("hasIntentBrief") &&
                                                   has("intentBriefItems") &&
                                                   has("isInterviewActive") &&
                                                   has("interviewQuestion") &&
                                                   has("interviewChoices")

    readonly property bool hasIntentBrief: intentSurfaceAvailable ? host.hasIntentBrief : false
    readonly property var intentItems: intentSurfaceAvailable ? host.intentBriefItems : []
    readonly property var openChoiceIds: intentSurfaceAvailable && has("intentOpenChoiceIds")
                                           ? host.intentOpenChoiceIds : []
    readonly property string briefSummary: intentSurfaceAvailable && has("intentBriefSummary")
                                             ? host.intentBriefSummary : ""
    readonly property bool isInterviewActive: intentSurfaceAvailable ? host.isInterviewActive : false
    readonly property bool canExploreInterview: intentSurfaceAvailable && has("canExploreInterview")
                                                  ? host.canExploreInterview : false
    readonly property int interviewTurnsCompleted: intentSurfaceAvailable && has("interviewTurnsCompleted")
                                                      ? host.interviewTurnsCompleted : 0
    readonly property string interviewQuestion: intentSurfaceAvailable ? host.interviewQuestion : ""
    readonly property string interviewConsequence: intentSurfaceAvailable && has("interviewConsequence")
                                                     ? host.interviewConsequence : ""
    readonly property var interviewChoices: intentSurfaceAvailable ? host.interviewChoices : []
    readonly property bool isInterviewBusy: intentSurfaceAvailable && has("isInterviewBusy")
                                              ? host.isInterviewBusy : false
    readonly property string interviewFeedback: intentSurfaceAvailable && has("interviewFeedback")
                                                  ? host.interviewFeedback : ""
    readonly property bool interviewInvokable: intentSurfaceAvailable &&
                                               callable("openInterviewFromCompanion") &&
                                               callable("submitInterviewResponse")
    readonly property bool canChangeDecisions: intentSurfaceAvailable && callable("changeIntentDecision")

    function openInterview() {
        if (interviewInvokable)
            return host.openInterviewFromCompanion(host.stateVersion);
        // Pre-contract hosts still have the explicit HUD route.
        if (callable("exploreInCompanion")) host.exploreInCompanion();
        return 0;
    }
    function submitInterview(text, disposition) {
        if (!interviewInvokable) return 0;
        return host.submitInterviewResponse(text, disposition, host.stateVersion);
    }
    function changeDecision(itemId, newText) {
        if (!canChangeDecisions) return 0;
        return host.changeIntentDecision(itemId, newText, host.stateVersion);
    }
    function keepOriginal() {
        if (callable("keepOriginalPrompt")) return host.keepOriginalPrompt(host.stateVersion);
        return 0;
    }

    // -------------------------------------------------------------- review
    readonly property bool reviewSurfaceAvailable: present && has("comprehensionReviewState") &&
                                                   has("comprehensionFindings") &&
                                                   has("canApplyReviewedOutput")

    readonly property string comprehensionState: reviewSurfaceAvailable
                                                   ? host.comprehensionReviewState : "not_run"
    readonly property string comprehensionInterpretation: reviewSurfaceAvailable &&
                                                           has("comprehensionInterpretation")
                                                             ? host.comprehensionInterpretation : ""
    readonly property var comprehensionFindings: reviewSurfaceAvailable ? host.comprehensionFindings : []
    readonly property bool canApplyReviewedOutput: reviewSurfaceAvailable && host.canApplyReviewedOutput
    readonly property bool reviewReady: reviewSurfaceAvailable && comprehensionState === "ready"
    readonly property bool reviewStale: reviewSurfaceAvailable && comprehensionState === "stale"
    readonly property bool reviewRunning: reviewSurfaceAvailable && comprehensionState === "running"
    readonly property bool reviewFailed: reviewSurfaceAvailable && comprehensionState === "failed"
    readonly property bool reviewNotRun: reviewSurfaceAvailable && comprehensionState === "not_run"
    readonly property bool reviewInvokable: reviewSurfaceAvailable &&
                                            callable("requestComprehensionReview") &&
                                            callable("resolveComprehensionFinding")
    // A blocking finding may never be acknowledged into applicability by the UI.
    readonly property int unresolvedDecisionCount: {
        let count = 0;
        for (let i = 0; i < comprehensionFindings.length; ++i) {
            const finding = comprehensionFindings[i];
            if (finding && finding.requiresUserDecision) ++count;
        }
        return count;
    }
    readonly property int blockingFindingCount: {
        let count = 0;
        for (let i = 0; i < comprehensionFindings.length; ++i) {
            const finding = comprehensionFindings[i];
            if (finding && finding.severity === "blocking") ++count;
        }
        return count;
    }

    function requestReview() {
        if (reviewInvokable) return host.requestComprehensionReview(host.stateVersion);
        return 0;
    }
    function resolveFinding(findingId, resolution) {
        if (!reviewInvokable) return 0;
        return host.resolveComprehensionFinding(findingId, resolution, host.stateVersion);
    }
}
