#include "roclint/retrieval/RetrievalEngine.hpp"
#include "roclint/retrieval/IEmbeddingRuntime.hpp"
#include "roclint/core/crypto/Sha256.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace roclint::retrieval {
namespace {

// Compute SHA-256 of a file for change detection.
std::string hashFile(const std::filesystem::path& path) {
    std::error_code ec;
    auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > 64 * 1024 * 1024) return {};
    std::FILE* f = nullptr;
#if defined(_WIN32)
    _wfopen_s(&f, path.c_str(), L"rb");
#else
    f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) return {};
    std::vector<char> buf(static_cast<std::size_t>(size));
    std::size_t rd = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (rd == 0) return {};
    return core::crypto::sha256Hex(std::string_view(buf.data(), rd));
}

// Stable chunk ID: SHA-256 of "workspaceRoot:relativePath:contentHash:start:end".
//
// Including the contentHash means that re-indexing the same file with new content
// produces a new chunk_id, so the old published row is NOT overwritten by INSERT
// and coexists until commitGeneration() prunes it.  Unchanged files produce the
// same chunk_id — enabling INSERT OR IGNORE for incremental-reuse (no re-embed needed).
std::string makeChunkId(std::string_view workspaceRoot,
                         std::string_view relativePath,
                         std::string_view contentHash,
                         std::uint32_t byteStart,
                         std::uint32_t byteEnd) {
    std::string key = std::string(workspaceRoot) + ":" + std::string(relativePath) +
                      ":" + std::string(contentHash) +
                      ":" + std::to_string(byteStart) + ":" + std::to_string(byteEnd);
    return "chunk_" + core::crypto::sha256Hex(key).substr(0, 32);
}

// Read a file's text bytes (UTF-8; skip unreadable files).
std::string readFileText(const std::filesystem::path& path) {
    std::error_code ec;
    auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > 1 * 1024 * 1024) return {};
    std::FILE* f = nullptr;
#if defined(_WIN32)
    _wfopen_s(&f, path.c_str(), L"rb");
#else
    f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) return {};
    std::string buf(static_cast<std::size_t>(size), '\0');
    std::size_t rd = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    buf.resize(rd);
    // Reject binary content heuristic: null bytes in first 512 bytes.
    for (std::size_t i = 0; i < std::min(rd, std::size_t{512}); ++i) {
        if (buf[i] == '\0') return {};
    }
    return buf;
}

using Clock = std::chrono::steady_clock;

double elapsedMs(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

} // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

// Determine SourceKind from file extension.
static SourceKind kindForPath(const std::filesystem::path& p) {
    static const std::set<std::string> kDocExts{
        ".md", ".markdown", ".rst", ".txt", ".adoc", ".asciidoc"
    };
    auto ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return kDocExts.count(ext) ? SourceKind::WorkspaceDoc : SourceKind::WorkspaceCode;
}

struct RetrievalEngine::Impl {
    std::shared_ptr<IEmbeddingRuntime> runtime;
    RetrievalEngineConfig config;
    RetrievalIndex index;
    TextChunker chunker;
    std::mutex indexMutex; // Serialises write operations (indexing passes).

    Impl(std::shared_ptr<IEmbeddingRuntime> rt, RetrievalEngineConfig cfg)
        : runtime(std::move(rt))
        , config(std::move(cfg))
        , index(config.indexDatabasePath)
        , chunker(config.chunkerPolicy)
    {}
};

// ---------------------------------------------------------------------------
// RetrievalEngine
// ---------------------------------------------------------------------------

RetrievalEngine::RetrievalEngine(std::shared_ptr<IEmbeddingRuntime> runtime,
                                   RetrievalEngineConfig config)
    : impl_(std::make_unique<Impl>(std::move(runtime), std::move(config)))
{}

RetrievalEngine::~RetrievalEngine() = default;
RetrievalEngine::RetrievalEngine(RetrievalEngine&&) noexcept = default;
RetrievalEngine& RetrievalEngine::operator=(RetrievalEngine&&) noexcept = default;

RetrievalStatus RetrievalEngine::status() const noexcept {
    if (!impl_) return RetrievalStatus::Unavailable;
    if (impl_->runtime->state() == EmbeddingRuntimeState::Unavailable ||
        impl_->runtime->state() == EmbeddingRuntimeState::Failed)
        return RetrievalStatus::Unavailable;
    if (!impl_->index.isOpen()) return RetrievalStatus::Unavailable;
    const auto cov = impl_->index.snapshotCoverage();
    if (!cov.ok()) return RetrievalStatus::Unavailable;
    if (!cov.value().coverageComplete) return RetrievalStatus::Partial;
    return RetrievalStatus::Ready;
}

std::string RetrievalEngine::statusMessage() const {
    if (!impl_) return "RetrievalEngine not initialized";
    return impl_->runtime->statusMessage();
}

IndexPolicy RetrievalEngine::activePolicy() const {
    if (!impl_) return {};
    return impl_->index.policy();
}

// ---------------------------------------------------------------------------
// indexWorkspace
// ---------------------------------------------------------------------------

core::Result<IndexStats> RetrievalEngine::indexWorkspace(
    const std::filesystem::path& workspaceRoot,
    const std::string& /*workspaceIdentity*/,
    const IndexLimits& limits,
    std::shared_ptr<std::atomic_bool> cancellation,
    IndexingProgressCallback progressCallback)
{
    if (!impl_) return core::makeError(core::ErrorCode::PreconditionFailed, "Engine not initialized");
    if (impl_->runtime->state() == EmbeddingRuntimeState::Unavailable)
        return core::makeError(core::ErrorCode::ProviderUnavailable, "Embedding model unavailable");

    std::unique_lock<std::mutex> lock(impl_->indexMutex);

    auto t0 = Clock::now();
    IndexStats stats{};

    // Open the index for this workspace.
    auto& idx = impl_->index;
    if (auto s = idx.open(workspaceRoot, impl_->config.policy); !s.ok()) return s.error();

    // Walk workspace files. The WalkResult tells us whether the iterator was
    // exhausted (true) or capped by maximumFiles (false). Only an exhausted,
    // uncancelled, fully-indexed walk may claim complete coverage.
    auto walkResult = walkWorkspace(workspaceRoot, limits.maximumFiles, limits.maximumFileBytes);
    stats.filesVisited = walkResult.files.size();

    if (auto s = idx.beginGeneration(RefreshType::Full); !s.ok()) return s.error();

    std::size_t filesDone = 0;
    bool cancelled = false;
    std::size_t embedFailures = 0;

    for (const auto& sf : walkResult.files) {
        if (cancellation && cancellation->load()) {
            cancelled = true;
            break;
        }
        if (limits.scanBudgetMs > 0 && elapsedMs(t0) >= limits.scanBudgetMs) {
            stats.incomplete = true;
            stats.skippedPaths.push_back(sf.relativePath);
            break;
        }

        ++filesDone;

        // Progress callback.
        if (progressCallback) {
            IndexingProgress prog;
            prog.filesProcessed = filesDone;
            prog.filesTotal = walkResult.files.size();
            prog.currentFile = sf.relativePath;
            progressCallback(prog);
        }

        // Check if the file changed.
        const auto hash = hashFile(sf.absolutePath);
        if (hash.empty()) {
            stats.incomplete = true;
            stats.skippedPaths.push_back(sf.relativePath);
            continue;
        }

        if (idx.isSourceUnchanged(sf.relativePath, hash)) {
            if (auto s = idx.recordSource(sf.relativePath, hash, sf.kind); !s.ok()) {
                idx.rollbackGeneration();
                return s.error();
            }
            ++stats.filesIndexed;
            continue;
        }

        // Read text.
        auto text = readFileText(sf.absolutePath);
        if (text.empty()) {
            stats.incomplete = true;
            stats.skippedPaths.push_back(sf.relativePath);
            continue;
        }

        // Chunk.
        auto chunks = impl_->chunker.chunk(text, sf.absolutePath);
        if (chunks.empty()) {
            stats.incomplete = true;
            stats.skippedPaths.push_back(sf.relativePath);
            continue;
        }

        // Limit chunks per file.
        const bool fileComplete = chunks.size() <= limits.maximumChunksPerFile;
        if (!fileComplete) {
            chunks.resize(limits.maximumChunksPerFile);
            stats.incomplete = true;
            stats.skippedPaths.push_back(sf.relativePath);
        }

        // Build IndexedChunk list.
        std::vector<IndexedChunk> indexedChunks;
        std::vector<std::string> chunkTexts;
        indexedChunks.reserve(chunks.size());
        chunkTexts.reserve(chunks.size());

        for (const auto& ch : chunks) {
            IndexedChunk ic;
            ic.chunkId.value = makeChunkId(workspaceRoot.generic_string(),
                                            sf.relativePath, hash, ch.byteStart, ch.byteEnd);
            ic.sourceKind = sf.kind;
            ic.location.sourcePath = sf.relativePath;
            ic.location.contentHash = hash;
            ic.location.byteStart = ch.byteStart;
            ic.location.byteEnd = ch.byteEnd;
            ic.location.lineStart = ch.lineStart;
            ic.location.lineEnd = ch.lineEnd;
            ic.text = ch.text;
            ic.indexedAtGen = idx.currentGeneration() + 1;
            if (ch.hasAstHint && !ch.astSymbolId.empty()) {
                AstLink link;
                link.symbolId = ch.astSymbolId;
                link.status = AstLinkStatus::Linked;
                ic.astLink = link;
            }
            chunkTexts.push_back(ch.text);
            indexedChunks.push_back(std::move(ic));
        }

        // Embed chunks.
        EmbedOptions embedOpts;
        embedOpts.timeout = std::chrono::seconds{60};
        embedOpts.cancellation = cancellation;
        auto embedResult = impl_->runtime->embed(chunkTexts, embedOpts);
        if (!embedResult.ok()) {
            // Embedding failed for this file; count it as a failure.
            // The file is NOT counted in filesIndexed (honest coverage tracking).
            ++embedFailures;
            stats.skippedPaths.push_back(sf.relativePath);
            continue;
        }

        // Store chunks.
        if (auto s = idx.insertChunks(indexedChunks, embedResult.value()); !s.ok()) {
            idx.rollbackGeneration();
            return s.error();
        }
        // A reusable source is published only after all of its vectors exist.
        if (auto s = idx.recordSource(sf.relativePath, hash, sf.kind, fileComplete); !s.ok()) {
            idx.rollbackGeneration();
            return s.error();
        }

        ++stats.filesIndexed;
        stats.chunksAdded += indexedChunks.size();
    }

    // Coverage is only complete when:
    //   - Not cancelled
    //   - The iterator was fully exhausted (no cap was hit)
    //   - No embedding failures (every eligible file was successfully indexed)
    //   - No per-file truncation or unreadable files occurred
    bool complete = !cancelled && walkResult.exhausted && (embedFailures == 0) && !stats.incomplete;
    if (auto s = idx.commitGeneration(complete); !s.ok()) {
        idx.rollbackGeneration();
        return s.error();
    }

    if (progressCallback) {
        IndexingProgress prog;
        prog.filesProcessed = filesDone;
        prog.filesTotal = walkResult.files.size();
        prog.complete = true;
        progressCallback(prog);
    }

    stats.incomplete = !complete || (embedFailures > 0);
    stats.elapsedMs = elapsedMs(t0);
    if (auto cov = idx.snapshotCoverage(); cov.ok())
        stats.snapshotGeneration = cov.value().snapshotGeneration;

    return stats;
}

// ---------------------------------------------------------------------------
// indexPaths
// ---------------------------------------------------------------------------

core::Result<IndexStats> RetrievalEngine::indexPaths(
    const std::filesystem::path& workspaceRoot,
    const std::string& /*workspaceIdentity*/,
    const std::vector<std::string>& relativePaths,
    const IndexLimits& limits,
    std::shared_ptr<std::atomic_bool> cancellation)
{
    if (!impl_) return core::makeError(core::ErrorCode::PreconditionFailed, "Engine not initialized");
    if (impl_->runtime->state() == EmbeddingRuntimeState::Unavailable)
        return core::makeError(core::ErrorCode::ProviderUnavailable, "Embedding model unavailable");

    std::unique_lock<std::mutex> lock(impl_->indexMutex);
    auto t0 = Clock::now();
    IndexStats stats{};

    auto& idx = impl_->index;
    if (auto s = idx.open(workspaceRoot, impl_->config.policy); !s.ok()) return s.error();
    if (auto s = idx.beginGeneration(RefreshType::Incremental); !s.ok()) return s.error();

    for (const auto& relPath : relativePaths) {
        if ((cancellation && cancellation->load()) ||
            (limits.scanBudgetMs > 0 && elapsedMs(t0) >= limits.scanBudgetMs)) {
            stats.incomplete = true;
            stats.skippedPaths.push_back(relPath);
            break;
        }
        ++stats.filesVisited;

        const auto absPath = workspaceRoot / relPath;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(absPath, ec) || ec) {
            // File deleted or moved: remove its chunks.
            if (auto s = idx.removeSourceChunks(relPath); !s.ok()) {
                idx.rollbackGeneration();
                return s.error();
            }
            if (ec) {
                stats.incomplete = true;
                stats.skippedPaths.push_back(relPath);
            }
            continue;
        }

        const auto fsize = std::filesystem::file_size(absPath, ec);
        if (ec || fsize > limits.maximumFileBytes) {
            stats.incomplete = true;
            stats.skippedPaths.push_back(relPath);
            continue;
        }

        const auto hash = hashFile(absPath);
        if (hash.empty()) {
            if (fsize == 0) {
                if (auto s = idx.removeSourceChunks(relPath); !s.ok()) {
                    idx.rollbackGeneration();
                    return s.error();
                }
            } else {
                stats.incomplete = true;
                stats.skippedPaths.push_back(relPath);
            }
            continue;
        }

        if (idx.isSourceUnchanged(relPath, hash)) {
            if (auto s = idx.recordSource(relPath, hash, kindForPath(absPath)); !s.ok()) {
                idx.rollbackGeneration();
                return s.error();
            }
            ++stats.filesIndexed;
            continue;
        }

        auto text = readFileText(absPath);
        if (text.empty()) {
            stats.incomplete = true;
            stats.skippedPaths.push_back(relPath);
            continue;
        }

        auto chunks = impl_->chunker.chunk(text, absPath);
        if (chunks.empty()) {
            stats.incomplete = true;
            stats.skippedPaths.push_back(relPath);
            continue;
        }
        const bool fileComplete = chunks.size() <= limits.maximumChunksPerFile;
        if (!fileComplete) {
            chunks.resize(limits.maximumChunksPerFile);
            stats.incomplete = true;
            stats.skippedPaths.push_back(relPath);
        }

        std::vector<IndexedChunk> indexedChunks;
        std::vector<std::string> chunkTexts;
        for (const auto& ch : chunks) {
            IndexedChunk ic;
            ic.chunkId.value = makeChunkId(workspaceRoot.generic_string(),
                                            relPath, hash, ch.byteStart, ch.byteEnd);
            ic.sourceKind = kindForPath(absPath);
            ic.location.sourcePath = relPath;
            ic.location.contentHash = hash;
            ic.location.byteStart = ch.byteStart;
            ic.location.byteEnd = ch.byteEnd;
            ic.location.lineStart = ch.lineStart;
            ic.location.lineEnd = ch.lineEnd;
            ic.text = ch.text;
            chunkTexts.push_back(ch.text);
            indexedChunks.push_back(std::move(ic));
        }

        EmbedOptions opts;
        opts.cancellation = cancellation;
        auto emb = impl_->runtime->embed(chunkTexts, opts);
        if (!emb.ok()) {
            stats.incomplete = true;
            stats.skippedPaths.push_back(relPath);
            continue;
        }
        if (auto s = idx.insertChunks(indexedChunks, emb.value()); !s.ok()) {
            idx.rollbackGeneration();
            return s.error();
        }
        if (auto s = idx.recordSource(relPath, hash, kindForPath(absPath), fileComplete); !s.ok()) {
            idx.rollbackGeneration();
            return s.error();
        }
        ++stats.filesIndexed;
        stats.chunksAdded += indexedChunks.size();
    }

    if (auto s = idx.commitGeneration(!stats.incomplete); !s.ok()) {
        idx.rollbackGeneration();
        return s.error();
    }
    if (auto cov = idx.snapshotCoverage(); cov.ok()) stats.snapshotGeneration = cov.value().snapshotGeneration;
    stats.elapsedMs = elapsedMs(t0);
    return stats;
}

// ---------------------------------------------------------------------------
// retrieve
// ---------------------------------------------------------------------------

RetrievalResponse RetrievalEngine::retrieve(const RetrievalRequest& request) {
    RetrievalResponse resp;
    resp.requestId = request.requestId;
    resp.revisionId = request.revisionId;
    resp.promptHash = request.promptHash;
    resp.workspaceEpoch = request.workspace.epoch;

    if (!impl_) {
        resp.status = RetrievalStatus::Unavailable;
        resp.failureReason = "Engine not initialized";
        return resp;
    }

    const auto runtimeState = impl_->runtime->state();
    if (runtimeState == EmbeddingRuntimeState::Unavailable ||
        runtimeState == EmbeddingRuntimeState::Failed ||
        runtimeState == EmbeddingRuntimeState::Unloaded) {
        resp.status = RetrievalStatus::Unavailable;
        resp.failureReason = impl_->runtime->statusMessage();
        return resp;
    }

    if (!impl_->index.isOpen()) {
        resp.status = RetrievalStatus::Unavailable;
        resp.failureReason = "Index not open";
        return resp;
    }

    auto t0 = Clock::now();

    // Build query text: use selection text if present, otherwise use prompt.
    const std::string queryText = request.selection.has_value()
        ? request.selection->text : request.promptText;

    if (queryText.empty()) {
        resp.status = RetrievalStatus::NoRelevantContext;
        return resp;
    }

    // Embed the query.
    auto t1 = Clock::now();
    EmbedOptions embedOpts;
    embedOpts.timeout = std::chrono::seconds{10};
    auto embResult = impl_->runtime->embed({queryText}, embedOpts);
    resp.embeddingMs = elapsedMs(t1);

    if (!embResult.ok()) {
        resp.status = RetrievalStatus::Unavailable;
        resp.failureReason = embResult.error().message;
        return resp;
    }

    const auto& batch = embResult.value();
    if (batch.rows == 0 || batch.data.empty()) {
        resp.status = RetrievalStatus::Unavailable;
        resp.failureReason = "Empty embedding returned";
        return resp;
    }

    std::vector<float> queryVec(batch.data.begin(), batch.data.end());

    // Build filter.
    RetrievalFilter filter;
    filter.workspaceRoot = request.workspace.workspaceRoot;
    filter.sources = request.allowedSources;
    filter.activeFile = request.workspace.activeFile;

    RetrievalOptions opts;
    opts.maxHits = request.maxHits > 0 ? request.maxHits : impl_->config.defaultMaxHits;
    opts.minFusedScore = impl_->config.minFusedScore;
    opts.revalidateHashes = impl_->config.revalidateSourceHashes;

    // Convert token budget to character budget using a ~4 chars/token heuristic.
    // This budget is now ENFORCED: excerpts are truncated to fit within it.
    // Once the actual Granite tokenizer is integrated, replace this with an exact
    // token count. The budget accounts for excerpts only; provenance labels and
    // delimiters consume additional overhead (see R04 handoff for allocation notes).
    const std::size_t tokenBudget = request.contextBudgetTokens > 0
        ? request.contextBudgetTokens
        : impl_->config.defaultContextBudgetTokens;
    opts.contextBudgetChars = tokenBudget * 4;

    // Retrieve.
    auto hitResult = impl_->index.retrieve(queryVec, queryText, filter, opts);
    resp.retrievalMs = elapsedMs(t0);

    if (!hitResult.ok()) {
        resp.status = RetrievalStatus::Unavailable;
        resp.failureReason = hitResult.error().message;
        return resp;
    }

    resp.hits = std::move(hitResult.value());

    // Revalidate source hashes if requested.
    if (opts.revalidateHashes && !request.workspace.workspaceRoot.empty()) {
        const auto wsRoot = std::filesystem::path(request.workspace.workspaceRoot);
        for (auto& hit : resp.hits) {
            if (hit.location.sourcePath.empty() || hit.location.contentHash.empty()) continue;
            auto valid = impl_->index.validateSourceHash(
                hit.location.sourcePath, hit.location.contentHash, wsRoot);
            hit.sourceHashValid = valid.ok() && valid.value();
        }
    }

    // Prompt injection check: flag chunks that contain instruction-like patterns.
    for (auto& hit : resp.hits) {
        // Simple heuristic: excerpts that start with common injection markers.
        static const std::string_view kInjectionPatterns[] = {
            "SYSTEM:", "ASSISTANT:", "USER:", "INSTRUCTIONS:",
            "ignore previous", "disregard", "you are now", "pretend you are"
        };
        const auto& ex = hit.excerpt;
        for (auto pat : kInjectionPatterns) {
            if (ex.size() >= pat.size()) {
                std::string head = ex.substr(0, pat.size());
                std::transform(head.begin(), head.end(), head.begin(),
                               [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
                std::string patLow(pat);
                std::transform(patLow.begin(), patLow.end(), patLow.begin(),
                               [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
                if (head == patLow) { hit.promptInjectionFlagged = true; break; }
            }
        }
    }

    // Compute coverage.
    if (auto cov = impl_->index.snapshotCoverage(); cov.ok())
        resp.coverage = cov.value();

    resp.policy = impl_->config.policy;

    if (!resp.coverage.coverageComplete) {
        resp.status = RetrievalStatus::Partial;
    } else if (resp.hits.empty()) {
        resp.status = RetrievalStatus::NoRelevantContext;
    } else {
        resp.status = RetrievalStatus::Ready;
    }

    // Compute context fingerprint over emitted excerpts.
    resp.contextFingerprint = computeContextFingerprint(request.requestId, resp.hits);

    return resp;
}

// ---------------------------------------------------------------------------
// Evidence revalidation
// ---------------------------------------------------------------------------

std::vector<ChunkId> RetrievalEngine::revalidateHits(
    const std::vector<RetrievedHit>& hits,
    const std::filesystem::path& workspaceRoot) const
{
    std::vector<ChunkId> stale;
    if (!impl_ || !impl_->index.isOpen()) return stale;
    for (const auto& hit : hits) {
        if (hit.location.sourcePath.empty() || hit.location.contentHash.empty()) continue;
        auto valid = impl_->index.validateSourceHash(
            hit.location.sourcePath, hit.location.contentHash, workspaceRoot);
        if (!valid.ok() || !valid.value())
            stale.push_back(hit.chunkId);
    }
    return stale;
}

std::string RetrievalEngine::computeContextFingerprint(
    std::string_view requestId,
    const std::vector<RetrievedHit>& emittedHits)
{
    // Fingerprint = SHA-256 over: requestId + sorted(chunkId.value + excerpt).
    std::vector<std::string> parts;
    parts.reserve(emittedHits.size());
    for (const auto& hit : emittedHits)
        parts.push_back(hit.chunkId.value + "|" + hit.excerpt);
    std::sort(parts.begin(), parts.end());

    std::string input = std::string(requestId);
    for (const auto& p : parts) input += p;
    return core::crypto::sha256Hex(input);
}

// ---------------------------------------------------------------------------
// forgetWorkspace
// ---------------------------------------------------------------------------

core::Status RetrievalEngine::forgetWorkspace(const std::filesystem::path& workspaceRoot) {
    if (!impl_) return core::Status::success();
    return impl_->index.forgetWorkspace(workspaceRoot.generic_string());
}

#ifdef ROCLINT_ENABLE_TEST_MOCKS
RetrievalIndex* RetrievalEngine::indexForTesting() noexcept {
    return impl_ ? &impl_->index : nullptr;
}
IEmbeddingRuntime* RetrievalEngine::runtimeForTesting() noexcept {
    return impl_ ? impl_->runtime.get() : nullptr;
}
#endif

} // namespace roclint::retrieval
