# Synthetic code fixture for companion measurement

This is a disposable, synthetic C++ fixture created for the 2026-11-15
companion/intent measurement. It is not production code and is not compiled by
the Roclint build.

The `code_ast_signature` scenario asks the production host to change
`p95LatencyMs` while preserving its signature and the public API. The fixture
gives the real codebase index a symbol and a small public header to ground.
