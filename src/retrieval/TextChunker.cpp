#include "roclint/retrieval/TextChunker.hpp"
#include "roclint/core/crypto/Sha256.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace roclint::retrieval {
namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

bool isCppExtension(const std::filesystem::path& p) {
    static const std::set<std::string> kCppExts{
        ".c", ".cc", ".cpp", ".cxx", ".c++",
        ".h", ".hh", ".hpp", ".hxx", ".h++"
    };
    auto ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return kCppExts.count(ext) > 0;
}

// Count UTF-8 newlines up to byte offset (for approximate line numbers).
std::uint32_t lineAtOffset(std::string_view text, std::uint32_t offset) {
    std::uint32_t line = 1;
    for (std::uint32_t i = 0; i < offset && i < text.size(); ++i) {
        if (text[i] == '\n') ++line;
    }
    return line;
}

// Split text into paragraphs at double-newlines (or single newline runs).
// Each paragraph retains its trailing newlines.
std::vector<std::pair<std::uint32_t, std::string_view>> splitParagraphs(std::string_view text) {
    std::vector<std::pair<std::uint32_t, std::string_view>> result;
    std::size_t start = 0;
    while (start < text.size()) {
        // Find the end of the current paragraph (double newline or end of text).
        std::size_t pos = start;
        while (pos < text.size()) {
            if (text[pos] == '\n') {
                if (pos + 1 < text.size() && text[pos + 1] == '\n') {
                    pos += 2;
                    // Consume any further blank lines.
                    while (pos < text.size() && text[pos] == '\n') ++pos;
                    break;
                }
            }
            ++pos;
        }
        auto para = text.substr(start, pos - start);
        // Trim leading/trailing whitespace only from the view, keep the byte offsets correct.
        if (!para.empty()) {
            result.emplace_back(static_cast<std::uint32_t>(start), para);
        }
        start = pos;
    }
    return result;
}

// Extract the first line (heading or signature) from a chunk of text.
std::string firstLine(std::string_view text) {
    auto nl = text.find('\n');
    auto line = nl != std::string_view::npos ? text.substr(0, nl) : text;
    // Strip comment markers and markdown headings.
    std::size_t start = 0;
    while (start < line.size() && (line[start] == '#' || line[start] == '/' || line[start] == '*' || line[start] == ' '))
        ++start;
    return std::string(line.substr(0, std::min(line.size(), std::size_t{120})));
}

// Detect lines that look like C++ top-level declaration starts (function,
// class, struct, namespace, using, typedef, enum).
bool looksLikeCppTopLevelBoundary(std::string_view line) {
    // Simple heuristic: starts with one of these keywords at column 0.
    static const std::string_view kKeywords[] = {
        "void ", "bool ", "int ", "char ", "float ", "double ", "auto ",
        "class ", "struct ", "namespace ", "enum ", "union ",
        "using ", "typedef ", "template ", "inline ", "static ",
        "constexpr ", "explicit ", "virtual ", "override ", "final "
    };
    for (auto kw : kKeywords) {
        if (line.substr(0, kw.size()) == kw) return true;
    }
    // Also match things like `Type::Method(` at column 0
    if (!line.empty() && (std::isupper(static_cast<unsigned char>(line[0])) ||
                          line[0] == '_'))
        return true;
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// TextChunker implementation
// ---------------------------------------------------------------------------

TextChunker::TextChunker(ChunkerPolicy policy) : policy_(std::move(policy)) {}

std::string_view TextChunker::version() noexcept {
    return ChunkerPolicy::kVersion;
}

std::vector<Chunk> TextChunker::chunk(std::string_view text,
                                       const std::filesystem::path& filePath) const {
    if (text.empty()) return {};
    if (isCppExtension(filePath)) return chunkCpp(text);
    return chunkText(text);
}

std::vector<Chunk> TextChunker::chunkText(std::string_view text) const {
    // Split into paragraphs, then accumulate them into chunks of approximately
    // targetChunkChars, with overlapChars overlap.
    auto paragraphs = splitParagraphs(text);
    if (paragraphs.empty()) return {};

    std::vector<Chunk> result;
    std::string accumulated;
    std::uint32_t chunkStart = 0;
    accumulated.reserve(policy_.targetChunkChars + policy_.overlapChars);

    auto flush = [&](std::uint32_t endByte) {
        if (accumulated.size() < policy_.minChunkChars) return;
        Chunk c;
        c.byteStart = chunkStart;
        c.byteEnd = endByte;
        c.lineStart = lineAtOffset(text, chunkStart);
        c.lineEnd = lineAtOffset(text, endByte);
        c.text = accumulated;
        c.headingContext = firstLine(accumulated);
        result.push_back(std::move(c));
    };

    for (std::size_t i = 0; i < paragraphs.size(); ++i) {
        const auto [paraOffset, paraText] = paragraphs[i];
        const std::uint32_t paraEnd = paraOffset + static_cast<std::uint32_t>(paraText.size());

        if (accumulated.empty()) {
            chunkStart = paraOffset;
            accumulated = std::string(paraText);
        } else if (accumulated.size() + paraText.size() > policy_.maxChunkChars) {
            // Current chunk would exceed hard limit: flush first, then start new.
            flush(paraOffset);
            // Carry over overlap: last overlapChars of accumulated text.
            std::string overlap;
            if (accumulated.size() > policy_.overlapChars) {
                overlap = accumulated.substr(accumulated.size() - policy_.overlapChars);
                // Find the overlap start byte in the original text.
                chunkStart = paraOffset > static_cast<std::uint32_t>(policy_.overlapChars)
                    ? paraOffset - static_cast<std::uint32_t>(policy_.overlapChars) : 0;
            } else {
                overlap = accumulated;
                chunkStart = paraOffset - static_cast<std::uint32_t>(accumulated.size());
            }
            accumulated = std::move(overlap);
            accumulated += paraText;
        } else {
            accumulated += paraText;
        }

        if (accumulated.size() >= policy_.targetChunkChars) {
            flush(paraEnd);
            // Start overlap for next chunk.
            std::string overlap;
            std::uint32_t overlapStart = paraEnd;
            if (accumulated.size() > policy_.overlapChars) {
                overlap = accumulated.substr(accumulated.size() - policy_.overlapChars);
                overlapStart = paraEnd > static_cast<std::uint32_t>(policy_.overlapChars)
                    ? paraEnd - static_cast<std::uint32_t>(policy_.overlapChars) : 0;
            }
            accumulated = std::move(overlap);
            chunkStart = overlapStart;
        }
    }

    // Flush any remaining text.
    if (!accumulated.empty()) {
        const std::uint32_t lastEnd = !paragraphs.empty()
            ? (paragraphs.back().first + static_cast<std::uint32_t>(paragraphs.back().second.size()))
            : 0;
        flush(lastEnd);
    }

    return result;
}

std::vector<Chunk> TextChunker::chunkCpp(std::string_view text) const {
    // Split into logical lines, detect top-level boundaries, then accumulate
    // into chunks bounded by policy_.targetChunkChars.
    std::vector<Chunk> result;

    // Build a list of (offset, line_text) pairs.
    std::vector<std::pair<std::uint32_t, std::string_view>> lines;
    lines.reserve(1024);
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        lines.emplace_back(static_cast<std::uint32_t>(pos), text.substr(pos, nl - pos + (nl < text.size() ? 1u : 0u)));
        pos = nl + 1;
    }
    if (lines.empty()) return {};

    std::string accumulated;
    std::uint32_t chunkStart = 0;
    std::string lastBoundaryHeading;
    bool firstLine_ = true;
    accumulated.reserve(policy_.targetChunkChars + policy_.overlapChars);

    auto flush = [&](std::uint32_t endByte) {
        if (accumulated.size() < policy_.minChunkChars) return;
        Chunk c;
        c.byteStart = chunkStart;
        c.byteEnd = endByte;
        c.lineStart = lineAtOffset(text, chunkStart);
        c.lineEnd = lineAtOffset(text, endByte);
        c.text = accumulated;
        c.headingContext = lastBoundaryHeading.empty()
            ? firstLine(accumulated) : lastBoundaryHeading;
        c.hasAstHint = !lastBoundaryHeading.empty();
        result.push_back(std::move(c));
    };

    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto [lineOffset, lineText] = lines[i];
        const std::uint32_t lineEnd = lineOffset + static_cast<std::uint32_t>(lineText.size());

        bool isBoundary = !firstLine_ && looksLikeCppTopLevelBoundary(lineText);

        if (isBoundary && accumulated.size() >= policy_.targetChunkChars / 2) {
            // Flush current chunk and start a new one at this boundary.
            flush(lineOffset);
            // No overlap for C++ chunks: boundaries are precise.
            accumulated.clear();
            chunkStart = lineOffset;
        } else if (accumulated.size() + lineText.size() > policy_.maxChunkChars) {
            // Hard limit: flush and continue.
            flush(lineOffset);
            accumulated.clear();
            chunkStart = lineOffset;
        }

        if (isBoundary) lastBoundaryHeading = std::string(lineText.substr(0, std::min(lineText.size(), std::size_t{120})));
        accumulated += lineText;
        firstLine_ = false;

        // Soft target: flush at a blank line after reaching target size.
        if (accumulated.size() >= policy_.targetChunkChars &&
            (lineText.empty() || (lineText.size() == 1 && lineText[0] == '\n'))) {
            flush(lineEnd);
            accumulated.clear();
            chunkStart = lineEnd;
            lastBoundaryHeading.clear();
        }
    }

    if (!accumulated.empty()) {
        const std::uint32_t lastEnd = !lines.empty()
            ? (lines.back().first + static_cast<std::uint32_t>(lines.back().second.size()))
            : 0;
        flush(lastEnd);
    }

    return result;
}

// ---------------------------------------------------------------------------
// Workspace walker
// ---------------------------------------------------------------------------

namespace {

// Directories whose contents should always be excluded.
const std::set<std::string> kExcludedDirNames{
    ".git", ".hg", ".svn", ".bzr",
    "node_modules", "__pycache__", ".pytest_cache",
    "build", "Build", "_build", "out", "Out",
    "dist", "Dist", ".dist",
    "_deps", ".deps",
    "vendor", "third_party", "third-party", "thirdparty",
    ".venv", "venv", "env",
    "target",   // Rust/cargo
    "pkg",      // Go
    ".gradle", ".idea", ".vscode", ".vs",
    "Debug", "Release", "RelWithDebInfo", "MinSizeRel",
    "x64", "x86", "Win32", "ARM64",
    "CMakeFiles",
    "buck-out",
    "bazel-bin", "bazel-out", "bazel-testlogs",
    ".cache", ".ccache",
    "site-packages",
};

// File extensions that are definitely binary or generated and should be skipped.
const std::set<std::string> kExcludedExtensions{
    // Compiled outputs
    ".o", ".obj", ".a", ".lib", ".so", ".dylib", ".dll", ".exe", ".pdb",
    ".class", ".jar", ".war", ".ear",
    ".pyc", ".pyo", ".pyd",
    ".wasm", ".wasm.map",
    // Model/data files
    ".gguf", ".safetensors", ".bin", ".pt", ".onnx",
    ".npy", ".npz", ".parquet", ".arrow", ".feather",
    ".pb", ".tflite",
    // Archives
    ".zip", ".tar", ".gz", ".bz2", ".xz", ".7z", ".rar",
    ".dmg", ".pkg", ".deb", ".rpm",
    // Images
    ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".tiff", ".webp", ".ico", ".svg",
    // Audio/video
    ".mp3", ".mp4", ".wav", ".flac", ".mov", ".avi", ".mkv",
    // Fonts
    ".ttf", ".otf", ".woff", ".woff2", ".eot",
    // Lock files and generated
    ".lock",
    // SQLite databases
    ".db", ".sqlite", ".sqlite3",
    // PDF
    ".pdf",
    // Office docs
    ".docx", ".xlsx", ".pptx", ".doc", ".xls", ".ppt",
    // Map/minified
    ".map",
};

// Determine source kind from extension.
SourceKind kindForPath(const std::filesystem::path& p) {
    static const std::set<std::string> kDocExts{
        ".md", ".markdown", ".rst", ".txt", ".adoc", ".asciidoc"
    };
    auto ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    if (kDocExts.count(ext)) return SourceKind::WorkspaceDoc;
    return SourceKind::WorkspaceCode;
}

bool isExcludedDir(const std::string& name) {
    return kExcludedDirNames.count(name) > 0 ||
           (!name.empty() && name[0] == '.') || // hidden dirs
           (name.size() > 3 && name.substr(0, 3) == "CMake");
}

bool isExcludedFile(const std::filesystem::path& p) {
    auto ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    if (kExcludedExtensions.count(ext)) return true;
    // Hidden files
    auto fname = p.filename().string();
    if (!fname.empty() && fname[0] == '.') return true;
    // Generated C++ pattern: .moc files
    if (ext == ".moc") return true;
    // Lock files by name
    if (fname == "package-lock.json" || fname == "yarn.lock" ||
        fname == "Cargo.lock" || fname == "poetry.lock" ||
        fname == "Pipfile.lock" || fname == "composer.lock")
        return true;
    return false;
}

} // namespace

WalkResult walkWorkspace(
    const std::filesystem::path& workspaceRoot,
    std::size_t maxFiles,
    std::size_t maxFileBytes,
    const ExclusionPredicate& extra)
{
    WalkResult result;
    result.files.reserve(512);

    std::error_code ec;
    if (!std::filesystem::is_directory(workspaceRoot, ec)) {
        result.exhausted = true; // No files to enumerate.
        return result;
    }

    // Use recursive_directory_iterator with error_code overload for safety.
    auto it = std::filesystem::recursive_directory_iterator(
        workspaceRoot,
        std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) {
        result.exhausted = false; // Could not start: treat as incomplete.
        return result;
    }

    const auto end = std::filesystem::recursive_directory_iterator();
    for (; it != end; ++it) {
        // Check the file limit BEFORE incrementing so we can tell whether
        // the iterator was exhausted naturally or capped.
        if (result.files.size() >= maxFiles) {
            // We hit the cap without exhausting the iterator.
            result.exhausted = false;
            return result;
        }

        const auto& entry = *it;
        const auto& absPath = entry.path();

        if (entry.is_directory(ec)) {
            const auto dirName = absPath.filename().string();
            if (isExcludedDir(dirName) || (extra && extra(absPath, true))) {
                it.disable_recursion_pending();
            }
            continue;
        }

        if (!entry.is_regular_file(ec) || ec) continue;
        if (isExcludedFile(absPath)) continue;
        if (extra && extra(absPath, false)) continue;

        const auto size = entry.file_size(ec);
        if (ec || size == 0 || size > maxFileBytes) continue;

        // Build relative path with forward slashes.
        auto relPath = std::filesystem::relative(absPath, workspaceRoot, ec);
        if (ec || relPath.empty()) continue;
        auto relStr = relPath.generic_string();

        SourceFile sf;
        sf.absolutePath = absPath;
        sf.relativePath = std::move(relStr);
        sf.kind = kindForPath(absPath);
        sf.sizeBytes = size;
        result.files.push_back(std::move(sf));
    }

    // Iterator reached end without hitting the cap.
    result.exhausted = true;
    return result;
}

std::vector<SourceFile> walkWorkspaceFiles(
    const std::filesystem::path& workspaceRoot,
    std::size_t maxFiles,
    std::size_t maxFileBytes,
    const ExclusionPredicate& extra)
{
    return walkWorkspace(workspaceRoot, maxFiles, maxFileBytes, extra).files;
}

} // namespace roclint::retrieval
