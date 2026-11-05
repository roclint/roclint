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
        std::cout << "files_visited=" << refresh.value().filesVisited
                  << " files_indexed=" << refresh.value().filesIndexed
                  << " files_recovered=" << refresh.value().filesRecovered
                  << " symbols=" << refresh.value().symbolsIndexed
                  << " index_ms=" << refresh.value().elapsedMs
                  << " incomplete=" << refresh.value().incomplete << "\n";

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
