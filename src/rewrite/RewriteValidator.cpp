#include "roclint/rewrite/RewriteValidator.hpp"

#include <regex>

#include "roclint/core/prompt_state/TextNormalization.hpp"

#include <algorithm>
#include <cctype>
#include <vector>

namespace roclint::core::rewrite {

namespace {

[[nodiscard]] std::string toLower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

[[nodiscard]] bool containsCaseInsensitive(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return false;
    }
    return toLower(haystack).find(toLower(needle)) != std::string::npos;
}

[[nodiscard]] bool isBlank(std::string_view s) {
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isspace(c) != 0; });
}

[[nodiscard]] std::vector<std::string_view> splitLines(std::string_view text) {
    std::vector<std::string_view> lines;
    if (text.empty()) {
        return lines;
    }
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto nl = text.find('\n', start);
        if (nl == std::string_view::npos) {
            if (start < text.size()) {
                lines.push_back(text.substr(start));
            }
            break;
        }
        lines.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    return lines;
}

ValidationResult reject(ProposalStatus status, std::string reason) {
    ValidationResult r;
    r.structurallyValid = false;
    r.isValid = false;
    r.finalStatus = status;
    r.rejectionReason = std::move(reason);
    return r;
}

} // namespace

Status RewriteValidator::validateFidelity(std::string_view original,
                                          const intent::ConfirmedIntent& intent,
                                          std::string_view rewritten) {
    std::string authorized(original);
    for (const auto& item : intent.items) {
        if (item.kind == intent::IntentResolutionKind::SelectedAlternative ||
            item.kind == intent::IntentResolutionKind::CustomText) {
            authorized += "\n" + (item.kind == intent::IntentResolutionKind::CustomText ? item.customText : item.chosenValue);
        }
    }
    const auto source = toLower(authorized);
    const auto output = toLower(rewritten);
    static const std::regex limited(R"(\b(minor|small|limited)\s+semantic\s+changes?\b)");
    static const std::regex broadened(R"(\b(broad|unrestricted|major)\s+semantic\s+(changes?|reinterpretation)\b)");
    const auto permitsBroadChanges = [&](const std::string& text) {
        static const std::regex negated(R"((?:do\s+not|don't|never|avoid|prohibit|forbid|no)\s+(?:(?:allow|permit|make|any)\s+)?$)");
        for (std::sregex_iterator it(text.begin(), text.end(), broadened), end; it != end; ++it) {
            const auto position = static_cast<std::size_t>(it->position());
            const auto prefix = text.substr(position > 80 ? position - 80 : 0, std::min<std::size_t>(80, position));
            if (!std::regex_search(prefix, negated)) return true;
        }
        return false;
    };
    if (std::regex_search(source, limited) && !permitsBroadChanges(source) && permitsBroadChanges(output))
        return makeError(ErrorCode::ValidationFailed, "Rewrite broadens the confirmed semantic-change limit");

    // Exact protected clauses are conservative evidence. Failure asks for another wording,
    // rather than claiming that a paraphrase has been semantically disproved.
    static const std::regex apiInvariant(R"(\b(?:without changing (?:its|the|my|our|their|your|the public|the existing) api|do not (?:change|alter|modify) (?:its|the|my|our|their|your|the public|the existing) api)\b)");
    for (std::sregex_iterator it(source.begin(), source.end(), apiInvariant), end; it != end; ++it) {
        if (output.find(it->str()) == std::string::npos)
            return makeError(ErrorCode::ValidationFailed, "Rewrite did not retain the explicit API invariant verbatim");
    }
    // New quantified timing/size/percentage requirements need a source in the prompt or
    // a confirmed answer. Formatting numbers and unrelated identifiers are not targets.
    static const std::regex quantity(R"(\b[0-9]+(?:\.[0-9]+)?\s*(?:ms|milliseconds?|seconds?|minutes?|kb|kib|mb|mib|gb|gib)\b|\b[0-9]+(?:\.[0-9]+)?\s*%)");
    auto compact = [](std::string value) {
        value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c); }), value.end());
        return value;
    };
    std::set<std::string> allowed;
    for (std::sregex_iterator it(source.begin(), source.end(), quantity), end; it != end; ++it) allowed.insert(compact(it->str()));
    for (std::sregex_iterator it(output.begin(), output.end(), quantity), end; it != end; ++it) {
        if (!allowed.contains(compact(it->str())))
            return makeError(ErrorCode::ValidationFailed, "Rewrite introduced an unsupported quantified requirement");
    }
    return Status::success();
}

std::vector<ConstraintItem> RewriteValidator::deriveConstraints(const intent::ConfirmedIntent& intent,
                                                                const RewriteProfileConfig& profile,
                                                                std::string_view rewrittenText) {
    std::vector<ConstraintItem> constraints;

    constraints.push_back(ConstraintItem{
        "rewrite_nonempty", "The rewrite contains text",
        isBlank(rewrittenText) ? ConstraintStatus::Failed : ConstraintStatus::MechanicallyVerified, ""});

    for (const auto& item : intent.items) {
        ConstraintItem c;
        c.id = "intent_item_" + item.itemId;
        switch (item.kind) {
            case intent::IntentResolutionKind::SelectedAlternative:
            case intent::IntentResolutionKind::CustomText: {
                const std::string& wording =
                    item.kind == intent::IntentResolutionKind::CustomText ? item.customText : item.chosenValue;
                c.evidence = wording;
                if (containsCaseInsensitive(rewrittenText, wording)) {
                    c.description = "The wording of your answer appears in the rewrite";
                    c.status = ConstraintStatus::MechanicallyVerified;
                } else {
                    c.description = "Check that the rewrite reflects your answer";
                    c.status = ConstraintStatus::SemanticReviewRequired;
                }
                break;
            }
            case intent::IntentResolutionKind::UseOriginal:
                c.description = "Check that this point keeps your original wording";
                c.status = ConstraintStatus::SemanticReviewRequired;
                break;
            case intent::IntentResolutionKind::AcceptedAssumption:
                c.description = "Check that the rewrite keeps this assumption";
                c.evidence = item.chosenValue;
                c.status = ConstraintStatus::SemanticReviewRequired;
                break;
            case intent::IntentResolutionKind::DeclinedAssumption:
                c.description = "Check that the rewrite does not rely on this assumption";
                c.evidence = item.chosenValue;
                c.status = ConstraintStatus::SemanticReviewRequired;
                break;
        }
        constraints.push_back(std::move(c));
    }

    for (const auto& id : intent.unresolvedItemIds) {
        constraints.push_back(ConstraintItem{"unresolved_" + id,
                                             "This question was left open; check the rewrite does not decide it",
                                             ConstraintStatus::SemanticReviewRequired, id});
    }

    switch (profile.family) {
        case ProfileFamily::AnthropicStructured: {
            const bool ok = rewrittenText.find("<task>") != std::string_view::npos &&
                            rewrittenText.find("</task>") != std::string_view::npos;
            constraints.push_back(ConstraintItem{
                "profile_structure",
                ok ? "The rewrite contains the <task> section this profile uses"
                   : "Check the layout: the <task> section this profile uses is missing",
                ok ? ConstraintStatus::MechanicallyVerified : ConstraintStatus::SemanticReviewRequired, profile.id});
            break;
        }
        case ProfileFamily::OpenAiStructured: {
            bool ok = false;
            for (const auto line : splitLines(rewrittenText)) {
                if (line.size() > 2 && line[0] == '#' && line[1] == ' ') {
                    ok = true;
                    break;
                }
            }
            constraints.push_back(ConstraintItem{
                "profile_structure",
                ok ? "The rewrite uses Markdown headings as this profile requires"
                   : "Check the layout: this profile expects Markdown headings",
                ok ? ConstraintStatus::MechanicallyVerified : ConstraintStatus::SemanticReviewRequired, profile.id});
            break;
        }
        case ProfileFamily::Generic:
        case ProfileFamily::LocalCompact:
            break;
    }

    return constraints;
}

ValidationResult RewriteValidator::validateProposal(const RewriteProposal& proposal,
                                                    std::string_view currentSessionId,
                                                    std::string_view currentRevisionId,
                                                    const intent::ConfirmedIntent& currentIntent,
                                                    const std::set<std::string>& acknowledgedConstraintIds) {
    const std::string recomputed = ProposalHasher::computeProposalHash(proposal);
    if (proposal.proposalHash != recomputed || proposal.id != "prop_" + recomputed) {
        return reject(ProposalStatus::Rejected, "Proposal hash does not match its contents");
    }

    switch (proposal.status) {
        case ProposalStatus::ProposalReady:
            break;
        case ProposalStatus::NeedsClarification:
            return reject(ProposalStatus::NeedsClarification,
                          proposal.clarificationQuestion.empty() ? "The model asked for clarification"
                                                                 : proposal.clarificationQuestion);
        case ProposalStatus::StaleProposal:
            return reject(ProposalStatus::StaleProposal, "Proposal is stale");
        case ProposalStatus::Rejected:
            return reject(ProposalStatus::Rejected, "Proposal was rejected");
    }

    if (proposal.sessionId != currentSessionId || currentIntent.sessionId != currentSessionId) {
        return reject(ProposalStatus::Rejected, "Proposal belongs to a different session");
    }
    if (proposal.baseRevisionId != currentRevisionId || currentIntent.baseRevisionId != currentRevisionId) {
        return reject(ProposalStatus::StaleProposal, "Proposal was produced for a different revision");
    }
    if (intent::IntentProvenanceBuilder::computeIntentHash(currentIntent) != currentIntent.intentHash) {
        return reject(ProposalStatus::Rejected, "Current intent hash does not match its contents");
    }
    if (proposal.intentId != currentIntent.id || proposal.intentHash != currentIntent.intentHash) {
        return reject(ProposalStatus::StaleProposal, "Proposal was produced for a different confirmed intent");
    }
    if (proposal.snapshotId != currentIntent.snapshotId || proposal.snapshotHash != currentIntent.snapshotHash) {
        return reject(ProposalStatus::StaleProposal, "Proposal was produced for a different analysis snapshot");
    }

    const auto profile = ProfileRegistry::getProfile(proposal.rewriteProfileId);
    if (!profile.has_value()) {
        return reject(ProposalStatus::Rejected, "Unknown rewrite profile '" + proposal.rewriteProfileId + "'");
    }
    if (isBlank(proposal.rewrittenText)) {
        return reject(ProposalStatus::Rejected, "Rewritten prompt is empty");
    }
    if (!hasCanonicalLineEndings(proposal.rewrittenText) || !hasCanonicalLineEndings(proposal.originalText)) {
        return reject(ProposalStatus::Rejected, "Proposal text must use canonical LF line endings");
    }
    if (proposal.diffText != computeAccessibleDiff(proposal.originalText, proposal.rewrittenText)) {
        return reject(ProposalStatus::Rejected, "Proposal diff was not computed locally from its texts");
    }
    if (const auto status = validateFidelity(proposal.originalText, currentIntent, proposal.rewrittenText); !status.ok())
        return reject(ProposalStatus::Rejected, status.error().message);
    if (proposal.constraints != deriveConstraints(currentIntent, *profile, proposal.rewrittenText)) {
        return reject(ProposalStatus::Rejected, "Proposal constraint claims are not reproducible by the validator");
    }
    for (const auto& c : proposal.constraints) {
        if (c.status == ConstraintStatus::Failed) {
            return reject(ProposalStatus::Rejected, "Check failed: " + c.description);
        }
    }

    ValidationResult result;
    result.structurallyValid = true;
    result.finalStatus = ProposalStatus::ProposalReady;
    for (const auto& c : proposal.constraints) {
        if (c.status == ConstraintStatus::SemanticReviewRequired && acknowledgedConstraintIds.count(c.id) == 0) {
            result.pendingAcknowledgments.push_back(c.id);
        }
    }
    result.isValid = result.pendingAcknowledgments.empty();
    if (!result.isValid) {
        result.rejectionReason = "Review and acknowledge each item marked for review";
    }
    return result;
}

std::string RewriteValidator::computeAccessibleDiff(std::string_view originalPrompt, std::string_view rewrittenPrompt) {
    const auto a = splitLines(originalPrompt);
    const auto b = splitLines(rewrittenPrompt);
    const std::size_t n = a.size();
    const std::size_t m = b.size();

    enum class Op { Equal, Delete, Insert };
    std::vector<std::pair<Op, std::string_view>> ops;

    constexpr std::size_t kMaxCells = 4'000'000;
    if (n * m <= kMaxCells) {
        std::vector<std::vector<std::size_t>> lcs(n + 1, std::vector<std::size_t>(m + 1, 0));
        for (std::size_t i = n; i-- > 0;) {
            for (std::size_t j = m; j-- > 0;) {
                lcs[i][j] = (a[i] == b[j]) ? lcs[i + 1][j + 1] + 1 : std::max(lcs[i + 1][j], lcs[i][j + 1]);
            }
        }
        std::size_t i = 0;
        std::size_t j = 0;
        while (i < n && j < m) {
            if (a[i] == b[j]) {
                ops.emplace_back(Op::Equal, a[i]);
                ++i;
                ++j;
            } else if (lcs[i + 1][j] >= lcs[i][j + 1]) {
                ops.emplace_back(Op::Delete, a[i++]);
            } else {
                ops.emplace_back(Op::Insert, b[j++]);
            }
        }
        while (i < n) ops.emplace_back(Op::Delete, a[i++]);
        while (j < m) ops.emplace_back(Op::Insert, b[j++]);
    } else {
        for (const auto line : a) ops.emplace_back(Op::Delete, line);
        for (const auto line : b) ops.emplace_back(Op::Insert, line);
    }

    std::string out;
    std::size_t k = 0;
    while (k < ops.size()) {
        if (ops[k].first == Op::Equal) {
            out += "  ";
            out += ops[k].second;
            out += '\n';
            ++k;
            continue;
        }
        std::vector<std::string_view> deleted;
        std::vector<std::string_view> inserted;
        while (k < ops.size() && ops[k].first != Op::Equal) {
            (ops[k].first == Op::Delete ? deleted : inserted).push_back(ops[k].second);
            ++k;
        }
        const std::size_t paired = std::min(deleted.size(), inserted.size());
        for (std::size_t p = 0; p < paired; ++p) {
            out += "~ was: ";
            out += deleted[p];
            out += "\n~ now: ";
            out += inserted[p];
            out += '\n';
        }
        for (std::size_t p = paired; p < deleted.size(); ++p) {
            out += "- ";
            out += deleted[p];
            out += '\n';
        }
        for (std::size_t p = paired; p < inserted.size(); ++p) {
            out += "+ ";
            out += inserted[p];
            out += '\n';
        }
    }
    return out;
}

} // namespace roclint::core::rewrite
