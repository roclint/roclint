#include "roclint/analysis/DataHazardPolicy.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace roclint::core::analysis {

namespace {

[[nodiscard]] std::string normalizeLower(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (char c : text) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return result;
}

[[nodiscard]] bool containsWord(std::string_view text, std::string_view word) {
    std::size_t pos = 0;
    while ((pos = text.find(word, pos)) != std::string_view::npos) {
        const bool leftOk = (pos == 0) || !std::isalnum(static_cast<unsigned char>(text[pos - 1]));
        const std::size_t end = pos + word.size();
        const bool rightOk = (end == text.size()) || !std::isalnum(static_cast<unsigned char>(text[end]));
        if (leftOk && rightOk) return true;
        pos += word.size();
    }
    return false;
}

[[nodiscard]] bool anyWord(std::string_view text, std::initializer_list<std::string_view> words) {
    return std::any_of(words.begin(), words.end(), [text](std::string_view w) {
        return containsWord(text, w);
    });
}

} // namespace

std::vector<DataHazardResult> DataHazardPolicy::scanPrompt(std::string_view prompt) {
    std::vector<DataHazardResult> results;
    const std::string lower = normalizeLower(prompt);

    // 1. Check-Then-Act Race Condition (e.g. inventory / stock / balance check followed by decrement)
    const bool hasCheckCue = anyWord(lower, {"check", "verify", "if", "when", "ensure", "validate"});
    const bool hasStateResource = anyWord(lower, {"stock", "inventory", "balance", "tickets", "seats", "credits", "quantity", "funds"});
    const bool hasDeductAction = anyWord(lower, {"subtract", "decrement", "reduce", "deduct", "minus", "decrease", "take"});
    const bool hasBuyAction = anyWord(lower, {"buy", "purchase", "checkout", "order"});
    const bool hasConcurrencyGuard = anyWord(lower, {"atomic", "transaction", "lock", "mutex", "optimistic", "serializable", "for update", "thread-safe", "idempotent", "queue"});

    if (hasCheckCue && hasStateResource && (hasDeductAction || hasBuyAction) && !hasConcurrencyGuard) {
        results.push_back({
            .type = DataHazardType::ConcurrencyRaceCondition,
            .suggestedFocus = "concurrency synchronization",
            .suggestedQuestion = "Should stock checking and deduction be executed inside an atomic database transaction with row-level locking to prevent overselling during concurrent purchases?",
            .primaryOptionText = "Atomic transaction with row lock",
            .primaryOptionEffect = "Uses SELECT FOR UPDATE or atomic decrement to prevent overselling race conditions.",
            .secondaryOptionText = "Optimistic locking with version check",
            .secondaryOptionEffect = "Updates row only if stock version matches; fails fast or retries on collision."
        });
    }

    // 2. Wildcard keystroke search / Full Table Scan
    const bool hasKeystrokeCue = anyWord(lower, {"keystroke", "letter", "types", "typing", "real-time", "live search", "as user types"});
    const bool hasSearchCue = anyWord(lower, {"search", "filter", "lookup", "query", "find"});
    const bool hasSearchGuard = anyWord(lower, {"debounce", "throttle", "index", "elastic", "algolia", "full-text", "fts", "prefix index"});

    if (hasKeystrokeCue && hasSearchCue && !hasSearchGuard) {
        results.push_back({
            .type = DataHazardType::WildcardKeystrokeSearch,
            .suggestedFocus = "search debouncing and indexing",
            .suggestedQuestion = "Should live search include a client-side debounce delay (e.g. 300ms) and minimum character threshold to prevent full-table database overload on every keystroke?",
            .primaryOptionText = "Debounce search with minimum 3 characters",
            .primaryOptionEffect = "Throttles query execution until user pauses typing to protect database CPU.",
            .secondaryOptionText = "Full-text indexing with server-side caching",
            .secondaryOptionEffect = "Uses indexed full-text search backend with cached common prefixes."
        });
    }

    // 3. Unbounded Cascade Deletion (Audit loss)
    const bool hasDeleteCue = anyWord(lower, {"delete", "remove", "wipe", "purge"});
    const bool hasAccountEntity = anyWord(lower, {"account", "customer", "user", "client", "member"});
    const bool hasCleanCue = anyWord(lower, {"clean", "all data", "everything", "completely", "purge"});
    const bool hasAuditGuard = anyWord(lower, {"soft delete", "archive", "anonymize", "audit", "tombstone", "inactive"});

    if (hasDeleteCue && hasAccountEntity && hasCleanCue && !hasAuditGuard) {
        results.push_back({
            .type = DataHazardType::UnboundedCascadeDelete,
            .suggestedFocus = "soft deletion vs hard cascade delete",
            .suggestedQuestion = "Should account deletion use a soft-delete (flagging inactive/anonymized) to preserve financial and audit trail records, or a permanent cascade delete?",
            .primaryOptionText = "Soft delete with data anonymization",
            .primaryOptionEffect = "Marks account inactive and scrubs PII while retaining ledger and transaction history.",
            .secondaryOptionText = "Hard cascade delete with confirmation",
            .secondaryOptionEffect = "Permanently deletes user row and all associated foreign keys."
        });
    }

    // 4. Insecure Direct Object Reference (IDOR)
    const bool hasUrlParamCue = anyWord(lower, {"url", "query parameter", "query param", "parameter", "param"});
    const bool hasIdCue = anyWord(lower, {"id", "user_id", "account_id", "profile_id"});
    const bool hasDirectViewCue = anyWord(lower, {"view", "load", "show", "update", "profile", "settings"});
    const bool hasAuthGuard = anyWord(lower, {"session", "token", "auth", "authenticated", "jwt", "cookie", "current_user"});

    if (hasUrlParamCue && hasIdCue && hasDirectViewCue && !hasAuthGuard) {
        results.push_back({
            .type = DataHazardType::InsecureDirectObjectReference,
            .suggestedFocus = "session authentication vs url parameter",
            .suggestedQuestion = "Should account and profile retrieval be bound to the authenticated user session rather than an unauthenticated ID from the URL?",
            .primaryOptionText = "Session-bound authenticated profile",
            .primaryOptionEffect = "Loads profile from verified session token (/profile/me) preventing unauthorized viewing.",
            .secondaryOptionText = "URL parameter with strict ownership check",
            .secondaryOptionEffect = "Verifies requesting user ID strictly matches URL ID before returning record."
        });
    }

    return results;
}

bool DataHazardPolicy::hasHazard(std::string_view prompt, DataHazardType type) {
    const auto results = scanPrompt(prompt);
    return std::any_of(results.begin(), results.end(), [type](const DataHazardResult& r) {
        return r.type == type;
    });
}

} // namespace roclint::core::analysis
