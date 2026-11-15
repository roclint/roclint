#include "roclint/rewrite/AstGroundingEngine.hpp"
#include "roclint/storage/CodebaseIndex.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: roclint_ast_grounding_benchmark <workspace-root> [active-file]\n";
        return 2;
    }
    const std::filesystem::path root = std::filesystem::path(argv[1]);
    const std::string activeFile = argc >= 3 ? argv[2] : "";
    const auto database = std::filesystem::temp_directory_path() /
        ("roclint-ast-benchmark-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
    {
        roclint::storage::CodebaseIndex index(database);
        const roclint::storage::WorkspaceContext workspace{root, activeFile, true};
        const auto refresh = index.refresh(workspace);
        if (!refresh.ok()) {
            std::cerr << "index refresh failed: " << refresh.error().message << "\n";
            return 1;
        }
        auto complete = refresh.value();
        std::size_t indexedSymbols = complete.symbolsIndexed;
        double totalIndexMs = complete.elapsedMs;
        int indexPasses = 1;
        std::cout << "files_visited=" << complete.filesVisited
                  << " files_indexed=" << complete.filesIndexed
                  << " files_recovered=" << complete.filesRecovered
                  << " symbols=" << complete.symbolsIndexed
                  << " index_ms=" << complete.elapsedMs
                  << " incomplete=" << complete.incomplete
                  << " incomplete_reason=" << static_cast<int>(complete.incompleteReason) << "\n";
        // The default 2 s scan budget can stop a cold pass on a large tree.
        // Retry with the same bounded budget; unchanged files are hash-skipped
        // instead of re-parsed. Report the complete operation, not just the
        // first chunk.
        while (complete.incomplete && indexPasses < 3) {
            const auto retry = index.refresh(workspace);
            if (!retry.ok()) {
                std::cerr << "index retry failed: " << retry.error().message << "\n";
                return 1;
            }
            totalIndexMs += retry.value().elapsedMs;
            indexedSymbols += retry.value().symbolsIndexed;
            complete = retry.value();
            ++indexPasses;
        }
        std::cout << "complete_index_ms=" << totalIndexMs
                  << " complete_passes=" << indexPasses
                  << " complete_files_visited=" << complete.filesVisited
                  << " complete_symbols_indexed=" << indexedSymbols
                  << " complete_incomplete=" << complete.incomplete
                  << " incomplete_reason=" << static_cast<int>(complete.incompleteReason) << "\n";

        const auto unchanged = index.refresh(workspace);
        if (!unchanged.ok()) {
            std::cerr << "unchanged refresh failed: " << unchanged.error().message << "\n";
            return 1;
        }
        std::cout << "unchanged_refresh_ms=" << unchanged.value().elapsedMs
                  << " unchanged_files=" << unchanged.value().filesVisited
                  << " unchanged_symbols=" << unchanged.value().symbolsIndexed
                  << " unchanged_incomplete=" << unchanged.value().incomplete << "\n";

        std::vector<roclint::core::rewrite::CanonicalIntent> intents;
        for (const auto* target : {"readPacket", "offset", "GraniteAdapter", "HostController"}) {
            roclint::core::rewrite::CanonicalIntent intent;
            intent.action = roclint::core::rewrite::IntentAction::MODIFY;
            intent.target = target;
            intent.nodeId = std::string("nid_") + target;
            intents.push_back(std::move(intent));
        }
        constexpr int kSamples = 200;
        double totalMs = 0;
        for (int sample = 0; sample < kSamples; ++sample) {
            const auto started = std::chrono::steady_clock::now();
            const auto grounded = roclint::core::rewrite::AstGroundingEngine::groundIntents(intents, activeFile, index);
            totalMs += static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - started).count()) / 1e6;
            if (sample == 0) {
                for (const auto& intent : grounded) {
                    std::cout << intent.target << "=";
                    if (intent.grounding.has_value()) {
                        std::cout << static_cast<int>(intent.grounding->status) << ":"
                                  << intent.grounding->qualifiedTarget;
                    } else {
                        std::cout << "none";
                    }
                    std::cout << "\n";
                }
            }
        }
        std::cout << "grounding_average_ms=" << totalMs / kSamples << "\n";

        const auto groundedSample =
            roclint::core::rewrite::AstGroundingEngine::groundIntents(intents, activeFile, index);
        std::string evidenceFile;
        std::string evidenceHash;
        for (const auto& intent : groundedSample) {
            if (!intent.grounding.has_value() || intent.grounding->filePath.empty()) continue;
            evidenceFile = intent.grounding->filePath;
            evidenceHash = intent.grounding->fileHash;
            break;
        }

        if (!evidenceFile.empty()) {
            const auto singleStarted = std::chrono::steady_clock::now();
            const auto single = index.refreshPaths(workspace, {evidenceFile});
            const double singleMs = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - singleStarted).count()) / 1e6;
            if (!single.ok()) {
                std::cerr << "single-file refresh failed: " << single.error().message << "\n";
                return 1;
            }
            std::cout << "single_file_refresh_ms=" << singleMs
                      << " single_file_incomplete=" << single.value().incomplete << "\n";

            if (!evidenceHash.empty()) {
                constexpr int kHashSamples = 100;
                const auto hashStarted = std::chrono::steady_clock::now();
                bool match = true;
                for (int sample = 0; sample < kHashSamples; ++sample) {
                    const auto checked = index.validateFileHash(evidenceFile, evidenceHash);
                    match = match && checked.ok() && checked.value();
                }
                const double hashMs = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - hashStarted).count()) / 1e6 / kHashSamples;
                std::cout << "hash_validation_ms=" << hashMs << " hash_match=" << match << "\n";
            }

            // Use a stable simple name: a fully qualified anonymous-namespace
            // label is not a valid exact-name lookup key.
            const std::string lookupName = "HostController";
            constexpr int kLookupSamples = 500;
            const auto lookupStarted = std::chrono::steady_clock::now();
            bool found = false;
            for (int sample = 0; sample < kLookupSamples; ++sample) {
                const auto queried = index.findByName(lookupName, {}, {}, 16, false);
                found = found || (queried.ok() && !queried.value().symbols.empty());
            }
            const double lookupMs = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - lookupStarted).count()) / 1e6 / kLookupSamples;
            std::cout << "warm_lookup_ms=" << lookupMs << " warm_lookup_found=" << found
                      << " warm_lookup_name=" << lookupName << "\n";
        }

        constexpr int kRenderSamples = 100;
        const auto renderStarted = std::chrono::steady_clock::now();
        std::size_t renderedBytes = 0;
        for (int sample = 0; sample < kRenderSamples; ++sample)
            renderedBytes = roclint::core::rewrite::AstGroundingEngine::renderPromptXml(groundedSample).size();
        const double renderMs = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - renderStarted).count()) / 1e6 / kRenderSamples;
        std::cout << "render_xml_ms=" << renderMs << " rendered_bytes=" << renderedBytes << "\n";

        const auto snapshot = index.snapshotInfo();
        if (snapshot.ok())
            std::cout << "snapshot_generation=" << snapshot.value().generation
                      << " snapshot_complete=" << snapshot.value().complete
                      << " snapshot_available=" << snapshot.value().available << "\n";
    }
    bool keep = false;
#ifdef _WIN32
    char* keepValue = nullptr;
    std::size_t keepLength = 0;
    _dupenv_s(&keepValue, &keepLength, "ROCLINT_KEEP_AST_DB");
    keep = keepValue != nullptr;
    std::free(keepValue);
#else
    keep = std::getenv("ROCLINT_KEEP_AST_DB") != nullptr;
#endif
    if (keep) std::cout << "database=" << database.string() << "\n";
    if (!keep) {
        std::error_code error;
        std::filesystem::remove(database, error);
        std::filesystem::remove(database.string() + "-wal", error);
        std::filesystem::remove(database.string() + "-shm", error);
    }
    return 0;
}
