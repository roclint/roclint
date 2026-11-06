#pragma once

#include "roclint/rewrite/CanonicalIntent.hpp"
#include "roclint/storage/CodebaseIndex.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace roclint::core::rewrite {

// Result of bounded prompt rendering. `rendered` contains exactly the intents
// whose evidence was emitted in `xml`, in the same order. Callers must hash and
// store only this rendered subset; intents omitted to satisfy the C10 byte cap
// or because no useful target identity fit were not used as prompt evidence and
// must not enter the AST snapshot hash.
struct PromptRenderResult {
    std::string xml{};
    std::vector<CanonicalIntent> rendered{};
};

class AstGroundingEngine {
public:
    // Matches raw canonical intents to indexed AST symbols. Unresolved targets fail
    // closed instead of being promoted to grounded model evidence.
    //
    // Supported exact request forms (all matching is exact; FTS/fuzzy fallback is
    // deliberately disabled for grounding):
    //   * target `name` or `qualifier::name` (`alpha::Widget::run`). A qualified
    //     target is matched against the index-qualified name. Under an explicit
    //     scope it may also use the relative form `<Scope>::member`, which
    //     resolves to `<resolved-scope>::member`; arbitrary suffixes are never
    //     accepted.
    //   * condition `in <scope>`, `within <scope>` or `inside <scope>`, where the
    //     scope is an exact qualified name or an exact simple name. Backticks,
    //     bold, underline and quote wrappers are accepted and stripped.
    //   * condition `file <relative-path>`; the path is slash-normalized and
    //     compared case-sensitively with the indexed workspace-relative path.
    //   * property `parameter` with value `name` or `#N` (1-based declared
    //     parameter position). The generic extractor states `present`, `absent`,
    //     `changed`, `unchanged` and `required` do not constrain by parameter.
    //   * explicit overload signature in the target, for example
    //     `Engine::run(int,double)` or `Engine::run(const std::string&)`. The
    //     argument text must be declared type syntax without parameter names,
    //     defaults, nested parentheses or a trailing qualifier, and is matched
    //     against the indexed syntactic `signatureKey`.
    //
    // Explicit file, scope, signature and parameter constraints are applied before
    // ranking. `activeFile` is only a tie-breaker among otherwise eligible exact
    // candidates and can never widen or override those constraints. Explicit
    // template arguments (for example `Engine::run<int>(int)`) are unsupported and
    // fail closed as incomplete ambiguity.
    //
    // `AstGroundingEvidence::matchReason` is stable and precedence-ordered:
    // "file", "signature", "parameter", "scope", "qualified", then
    // "active-file" (unconstrained focus tie-break among multiple candidates),
    // then "unique". It is empty when no match strategy applies (for example an
    // unsupported template target or an unconstrained ADD).
    //
    // All intents are resolved against the same index generation. An incomplete
    // snapshot or an incomplete query result makes the candidate universe
    // non-authoritative: evidence is downgraded to Ambiguous with complete=false
    // (ADD-style non-existence claims stay Ungrounded with complete=false), no
    // absence is proven and no AST hash is produced. If the snapshot generation
    // changes while the batch is enriched, newly grounded evidence is downgraded
    // the same way so the host cannot promote a mixed-snapshot result.
    [[nodiscard]] static std::vector<CanonicalIntent> groundIntents(
        const std::vector<CanonicalIntent>& raw,
        std::string_view activeFile,
        const storage::CodebaseIndex& index);

    // Renders a bounded XML block for the local model. The signature payload
    // defaults to a separate 300-byte sub-limit, and the complete document,
    // including framing, attributes, paths, signatures and candidates, is capped
    // to kMaximumPromptBytes. Function bodies are never included. Long values are
    // compacted at structural boundaries while the result remains well-formed
    // XML and valid UTF-8.
    //
    // A Verified record is emitted only when its target attribute carries a
    // useful identity: the complete requested target, or a middle compaction
    // with an explicit "..." truncation marker. Optional grounded-symbol,
    // location and signature fields are dropped before the required target
    // identity is omitted. If even the marked target cannot fit, the whole
    // Verified record is omitted. A Verified record is therefore never emitted
    // as `target=""` or with a whitespace-only target, and no unmarked prefix is
    // presented as a complete identifier. Omitted records never enter
    // `rendered`, the AST snapshot hash or HUD evidence.
    //
    // The pinned Granite tokenizer is a byte-level BPE: every token covers at
    // least one UTF-8 byte. A complete document of at most 350 bytes is therefore
    // guaranteed to contain at most 350 tokens. This is a worst-case bound, not a
    // bytes-per-token estimate, and it holds even when the tokenizer is not
    // available to the caller.
    //
    // Render entry point that also reports the exact intents/evidence emitted.
    // The rendered subset can be strictly smaller than `intents` when the byte
    // cap requires omission; omitted intents were not used as model evidence.
    [[nodiscard]] static PromptRenderResult renderPromptContext(
        const std::vector<CanonicalIntent>& intents, std::size_t signatureBudget = 300);

    // Compatibility wrapper for callers that only need the XML string.
    [[nodiscard]] static std::string renderPromptXml(const std::vector<CanonicalIntent>& intents,
                                                     std::size_t signatureBudget = 300);

    // SHA-256 over workspace identity plus every verified symbol's file hash,
    // byte span and signature. Empty when no intent is verified, which keeps
    // proposals on the legacy v1 hash domain. Incomplete evidence is never
    // Verified and is therefore never included. Callers should pass the
    // PromptRenderResult::rendered subset so evidence omitted by the prompt
    // budget cannot enter the proposal hash.
    [[nodiscard]] static std::string computeAstSnapshotHash(
        const std::vector<CanonicalIntent>& grounded, std::string_view workspaceIdentity);

    static constexpr std::size_t kMaximumCandidates = 8;
    // C10 budget: 350 Granite tokens. Because the pinned vocabulary is a
    // byte-level BPE, one UTF-8 byte is the worst-case floor per token, so a
    // complete 350-byte document is a mathematically valid 350-token bound.
    static constexpr std::size_t kMaximumPromptTokens = 350;
    static constexpr std::size_t kMaximumPromptBytes = 350;
};

} // namespace roclint::core::rewrite
