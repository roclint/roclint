#pragma once

// MockEmbeddingRuntime — deterministic test double for IEmbeddingRuntime.
//
// Produces deterministic pseudo-embeddings from text hashes. Each embedding is
// L2-normalized and reproducible for the same input text.
//
// When real_model_path is non-empty and ROCLINT_RETRIEVAL_REAL_EMBEDDINGS is
// set, the mock delegates to the actual embedding_worker.py subprocess.
// Unit tests MUST use the hash-based mock (no real model required).
// Integration tests that test retrieval quality MUST use real embeddings.

#include "roclint/retrieval/IEmbeddingRuntime.hpp"
#include "roclint/retrieval/RetrievalTypes.hpp"
#include "roclint/core/Result.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace retrieval_test {

class MockEmbeddingRuntime final : public roclint::retrieval::IEmbeddingRuntime {
public:
    static constexpr int kMockDim = 768;

    explicit MockEmbeddingRuntime(bool ready = true)
        : state_(ready
            ? roclint::retrieval::EmbeddingRuntimeState::Ready
            : roclint::retrieval::EmbeddingRuntimeState::Unavailable)
    {
        policy_.modelId = "test-mock-embedding";
        policy_.modelRevision = "mock-rev-0000000000000000000000000000000000000000";
        policy_.dimensions = kMockDim;
        policy_.normalized = true;
        policy_.chunkerVersion = "text_chunker.v1.target512.overlap64";
        policy_.preprocessVersion = "mock.v1";
    }

    roclint::core::Result<roclint::retrieval::EmbeddingBatch> embed(
        const std::vector<std::string>& texts,
        const roclint::retrieval::EmbedOptions& /*opts*/ = {}) override
    {
        if (state_.load() != roclint::retrieval::EmbeddingRuntimeState::Ready)
            return roclint::core::makeError(roclint::core::ErrorCode::ProviderUnavailable,
                "Mock runtime not ready");

        roclint::retrieval::EmbeddingBatch batch;
        batch.dimensions = kMockDim;
        batch.rows = texts.size();
        batch.data.resize(texts.size() * static_cast<std::size_t>(kMockDim), 0.0f);

        for (std::size_t i = 0; i < texts.size(); ++i) {
            float* row = batch.data.data() + i * static_cast<std::size_t>(kMockDim);
            hashEmbed(texts[i], row, kMockDim);
        }
        return batch;
    }

    roclint::retrieval::EmbeddingRuntimeState state() const noexcept override {
        return state_.load();
    }

    roclint::retrieval::IndexPolicy policy() const override { return policy_; }

    std::string statusMessage() const override {
        return state_.load() == roclint::retrieval::EmbeddingRuntimeState::Ready
            ? "Mock ready" : "Mock unavailable";
    }

    void setReady(bool ready) {
        state_.store(ready
            ? roclint::retrieval::EmbeddingRuntimeState::Ready
            : roclint::retrieval::EmbeddingRuntimeState::Unavailable);
    }

    // Compute the expected embedding for a text string deterministically.
    // Used in tests to verify that specific chunks appear in results.
    static std::vector<float> expectedEmbedding(const std::string& text) {
        std::vector<float> v(kMockDim, 0.0f);
        hashEmbed(text, v.data(), kMockDim);
        return v;
    }

private:
    // Deterministic hash-based embedding: seed a simple LCG with a string hash,
    // fill the vector, then L2-normalize. Similar texts will have similar hashes
    // only incidentally; this is for testing correctness, not retrieval quality.
    static void hashEmbed(const std::string& text, float* out, int dims) {
        // FNV-1a hash of the text bytes.
        std::uint64_t hash = 14695981039346656037ULL;
        for (unsigned char c : text) {
            hash ^= c;
            hash *= 1099511628211ULL;
        }
        // LCG to fill the vector.
        std::uint64_t state = hash ^ 0xDEADBEEFCAFEBABEULL;
        for (int i = 0; i < dims; ++i) {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            // Map to [-1, 1].
            out[i] = static_cast<float>(static_cast<std::int32_t>(state >> 32)) / 2147483648.0f;
        }
        // L2 normalize.
        float norm2 = 0.0f;
        for (int i = 0; i < dims; ++i) norm2 += out[i] * out[i];
        if (norm2 > 0.0f) {
            float invNorm = 1.0f / std::sqrt(norm2);
            for (int i = 0; i < dims; ++i) out[i] *= invNorm;
        }
    }

    std::atomic<roclint::retrieval::EmbeddingRuntimeState> state_;
    roclint::retrieval::IndexPolicy policy_{};
};

} // namespace retrieval_test
