#include "roclint/analysis/SourceSpan.hpp"
#include "support/check.hpp"

#include <iostream>
#include <string_view>

using namespace roclint::core::analysis;

void testAsciiSpans() {
    std::string_view prompt = "Update SessionManager timeout handling";
    // "SessionManager" 
    SourceSpan span{.startByte = 7, .endByte = 21};

    ROCLINT_CHECK(isValidSpan(prompt, span));
    ROCLINT_CHECK(span.length() == 14);
    ROCLINT_CHECK(!span.empty());

    auto slice = extractSpan(prompt, span);
    ROCLINT_CHECK(slice.has_value());
    ROCLINT_CHECK(*slice == "SessionManager");

    // Empty span at valid boundary
    SourceSpan emptySpan{.startByte = 7, .endByte = 7};
    ROCLINT_CHECK(isValidSpan(prompt, emptySpan));
    ROCLINT_CHECK(emptySpan.length() == 0);
    ROCLINT_CHECK(emptySpan.empty());
    auto emptySlice = extractSpan(prompt, emptySpan);
    ROCLINT_CHECK(emptySlice.has_value());
    ROCLINT_CHECK(emptySlice->empty());
}

void testAccentedSpans() {
   
    std::string_view prompt = "crème brûlée";

 
    SourceSpan cremeSpan{.startByte = 0, .endByte = 6};
    ROCLINT_CHECK(isValidSpan(prompt, cremeSpan));
    auto cremeSlice = extractSpan(prompt, cremeSpan);
    ROCLINT_CHECK(cremeSlice.has_value());
    ROCLINT_CHECK(*cremeSlice == "crème");

    SourceSpan eSpan{.startByte = 2, .endByte = 4};
    ROCLINT_CHECK(isValidSpan(prompt, eSpan));
    auto eSlice = extractSpan(prompt, eSpan);
    ROCLINT_CHECK(eSlice.has_value());
    ROCLINT_CHECK(*eSlice == "è");
    
    SourceSpan invalidStart{.startByte = 3, .endByte = 4};
    ROCLINT_CHECK(!isValidSpan(prompt, invalidStart));
    ROCLINT_CHECK(!extractSpan(prompt, invalidStart).has_value());

    SourceSpan invalidEnd{.startByte = 2, .endByte = 3};
    ROCLINT_CHECK(!isValidSpan(prompt, invalidEnd));
    ROCLINT_CHECK(!extractSpan(prompt, invalidEnd).has_value());
}

void testEmojiSpans() {
   
    std::string_view prompt = "Deploy 🚀 now";
    ROCLINT_CHECK(prompt.size() == 15);

   
    SourceSpan rocketSpan{.startByte = 7, .endByte = 11};
    ROCLINT_CHECK(isValidSpan(prompt, rocketSpan));
    auto rocketSlice = extractSpan(prompt, rocketSpan);
    ROCLINT_CHECK(rocketSlice.has_value());
    ROCLINT_CHECK(*rocketSlice == "🚀");

    ROCLINT_CHECK(!isUtf8Boundary(prompt, 8));
    ROCLINT_CHECK(!isUtf8Boundary(prompt, 9));
    ROCLINT_CHECK(!isUtf8Boundary(prompt, 10));
    ROCLINT_CHECK(isUtf8Boundary(prompt, 7));
    ROCLINT_CHECK(isUtf8Boundary(prompt, 11));

    SourceSpan slicedEmoji{.startByte = 7, .endByte = 9};
    ROCLINT_CHECK(!isValidSpan(prompt, slicedEmoji));
    ROCLINT_CHECK(!extractSpan(prompt, slicedEmoji).has_value());
}

void testCombiningMarksAndLF() {
    
    std::string_view prompt = "e\u0301clair\nnext line";
    SourceSpan firstChar{.startByte = 0, .endByte = 3};
    ROCLINT_CHECK(isValidSpan(prompt, firstChar));

    std::size_t lfPos = prompt.find('\n');
    ROCLINT_CHECK(lfPos != std::string_view::npos);
    ROCLINT_CHECK(isUtf8Boundary(prompt, lfPos));
    ROCLINT_CHECK(isUtf8Boundary(prompt, lfPos + 1));
}

void testInvalidBounds() {
    std::string_view prompt = "hello world"; // size 11

    
    SourceSpan inverted{.startByte = 5, .endByte = 3};
    ROCLINT_CHECK(!isValidSpan(prompt, inverted));
    ROCLINT_CHECK(!extractSpan(prompt, inverted).has_value());

    
    SourceSpan outOfBounds{.startByte = 0, .endByte = 100};
    ROCLINT_CHECK(!isValidSpan(prompt, outOfBounds));
    ROCLINT_CHECK(!extractSpan(prompt, outOfBounds).has_value());

  
    SourceSpan startOutOfBounds{.startByte = 20, .endByte = 25};
    ROCLINT_CHECK(!isValidSpan(prompt, startOutOfBounds));
}

void testAmbiguousRepeatedSubstrings() {
    
    std::string_view prompt = "Increase timeout because connection timeout failed";

    
    auto result = findUniqueSpan(prompt, "timeout");
    ROCLINT_CHECK(result.status == SpanMatchStatus::AmbiguousMultiple);
    ROCLINT_CHECK(!result.span.has_value());

    auto uniqueResult = findUniqueSpan(prompt, "connection");
    ROCLINT_CHECK(uniqueResult.status == SpanMatchStatus::ExactMatch);
    ROCLINT_CHECK(uniqueResult.span.has_value());
    ROCLINT_CHECK(uniqueResult.span->startByte == 25);
    ROCLINT_CHECK(uniqueResult.span->endByte == 35);

    auto extracted = extractSpan(prompt, *uniqueResult.span);
    ROCLINT_CHECK(extracted.has_value());
    ROCLINT_CHECK(*extracted == "connection");

    // Non-existent word -> NotFound
    auto notFound = findUniqueSpan(prompt, "database");
    ROCLINT_CHECK(notFound.status == SpanMatchStatus::NotFound);
    ROCLINT_CHECK(!notFound.span.has_value());

    // Empty excerpt -> NotFound
    auto emptyResult = findUniqueSpan(prompt, "");
    ROCLINT_CHECK(emptyResult.status == SpanMatchStatus::NotFound);
    ROCLINT_CHECK(!emptyResult.span.has_value());
}

int main() {
    std::cout << "[Test SourceSpan] Starting A05 / Lab C2 test suite...\n";
    testAsciiSpans();
    testAccentedSpans();
    testEmojiSpans();
    testCombiningMarksAndLF();
    testInvalidBounds();
    testAmbiguousRepeatedSubstrings();
    std::cout << "[Test SourceSpan] All tests PASSED successfully!\n";
    return 0;
}
