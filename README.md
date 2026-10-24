# Roclint

Roclint is the existing C++20 / Qt Quick resident HUD, with SQLite history, local Julia classification and local Granite choices/rewrites. This branch preserves the upstream HUD/dashboard design and build targets while fixing stale results, fabricated scores, implicit choices and unreviewed delivery.

The user's latest required model order is **Julia/S1 triage → unchanged original when clear, or Granite options → explicit choice → Granite rewrite when ambiguous**. [Model routing](docs/MODEL_ROUTING.md) records the implemented gate, persistence boundaries and open quality/performance requirements. The classifier is real local Julia inference; its current development results are not release-calibrated.

## Build

Required: CMake 3.25+, a C++20 compiler, SQLite3 development files, and Qt 6.2+ with Core, Gui, Quick, Qml, Widgets, Network and Concurrent. Desktop tests additionally require Qt Test. Use a matching compiler/Qt kit; the Windows development build uses Visual Studio 2022 and Qt 6.8.2 MSVC x64.

```sh
python tools/setup_granite.py --runtime-only
cmake -S . -B build -DCMAKE_PREFIX_PATH="<your Qt kit>"
cmake --build build --config Release --parallel
```

The first command fetches the exact clean llama.cpp revision declared in `app/runtime/granite-models.json`; it does not download weights. Native Granite is built by default. `ROCLINT_GRANITE_BACKEND=AUTO` selects Metal on macOS, Vulkan when its SDK is available, otherwise CPU. Use an explicit `-DROCLINT_GRANITE_BACKEND=CPU|VULKAN|METAL` for a reproducible build. A GPU build requires that backend on the target computer and reports failure if it is unavailable. `-DROCLINT_BUILD_NATIVE_GRANITE=OFF` is an explicit Ollama development build.

If CMake cannot find SQLite, supply `-DSQLite3_INCLUDE_DIR=<directory containing sqlite3.h>` and `-DSQLite3_LIBRARY=<SQLite library>`, or use your normal toolchain/package configuration. A missing Qt kit causes CMake to skip the desktop target; check that `roclint_host` is built. Headless builds can explicitly use `-DBUILD_DESKTOP=OFF`. Production builds can disable tests with `-DBUILD_TESTING=OFF`.

Run `build/Release/roclint_host.exe` with Visual Studio generators, or `build/roclint_host` with single-configuration generators. Qt's runtime libraries/plugins must be on the runtime search path or deployed beside the executable. On Windows, use the matching kit's `windeployqt --release --qmldir app/qml/shell build/Release/roclint_host.exe`.

## Local models

1. Native Granite uses pinned **Granite 4.2 3B** weights with the packaged llama.cpp helper. The user-selected default is **Q8_0**. Choose **Native Granite** in Settings, explicitly download the pinned model, or select its verified local folder. Q4_K_M and Q5_K_M remain available for explicit development comparisons. **Load model** verifies and warms it; Analyze also warms it on first use. Q8_0 is the default; model-quality and release acceptance remain open. See [native runtime](docs/NATIVE_GRANITE.md). **Ollama development** retains the legacy adapter, installed-model tools and local GGUF import; the current S1-first generative workflow requires pinned Native Granite.
2. Prepare Julia with Python 3.11+:

   ```sh
   python tools/setup_julia.py --build-dir build
   ```

   This creates an isolated environment under `build/runtime/julia-python` and installs dependencies. It downloads Python packages, not the model. An existing compatible environment can instead be selected with **Python…** in Settings. `--reuse-system-packages` is an explicit development shortcut.

3. Select **Julia** in Settings and use **Download / repair** once, or **Load Julia** for an existing verified model. Julia uses actual local inference with CUDA BF16 auto-selection and CPU FP32 fallback. It classifies the frozen prompt first. Scores below 0.70 preserve the original; scores at or above 0.70 request Granite choices. Julia does not generate rewrites or choose the user's intent. See [Julia runtime details](docs/JULIA_LOCAL_SCORER.md).

No model is silently downloaded on startup. Jev remains a legacy selector with no production cloud transport; the required S1-first flow requires local Julia and fails visibly when it is unavailable.

Rebuild after updating runtime sources so the host and packaged helpers use matching protocol/configuration versions. Incompatible helpers fail visibly. The [Julia lifecycle notes](docs/JULIA_LOCAL_SCORER.md) describe cancellation, shutdown, Windows process ownership and optional real-model verification.

## Workflow

- Default invocation: **Ctrl/Cmd+Shift+L**, retained from upstream and configurable in Settings.
- Enable **Live prompt capture** to invoke Roclint from a focused supported editor. An empty composer can also be captured as the destination for a draft authored in the HUD. Alternatively, type or paste a draft explicitly and choose **Analyze**. Opening the HUD never imports clipboard text automatically; passive typing capture does not run inference.
- The open HUD follows the focused supported text field without repeating the shortcut, including when the editor becomes available after invocation. Draft or field changes invalidate previous analysis; choose **Analyze** when ready. Editing in the HUD or confirming a clarification pauses following to preserve that draft and destination. While analysis or model loading is active, **Cancel analysis** preserves the draft and lets you retry.
- The original workspace and single-file controls use real folders/files. **Apply scope** adds the selected boundary to the visible draft. No project source is read or indexed for RAG.
- Resolve material choices explicitly; the final answer starts a rewrite preview. Review the diff and constraints, then **Approve & Insert** into the captured field or **Approve & Copy** in manual mode. **Insert as-is** delivers the literal draft to a bound field; **Copy as-is** copies in manual mode and leaves the HUD open with an explicit status. If the guided question budget is spent, an unresolved decision stays visible for an explicit manual answer.
- If a rewrite has no destination, choose **Connect editor…**, click the destination prompt box, and press the configured shortcut. Roclint reopens the same review with **Approve & Insert**, keeping the rewrite, answers and review checks. History's **Restore** also opens the saved review directly without regenerating it.
- Optional capture/direct delivery requires OS accessibility support and a captured eligible field. Delivery checks the captured text and falls back to the clipboard when the field is stale or unsupported.
- Generated options that directly reverse a grounded explicit color requirement are blocked with a reason. This narrow guard does not make the model a complete constraint verifier; review remains required.

## Verify

```sh
python tools/check_structure.py
python -m unittest discover -s tools -p test_check_structure.py
ctest --test-dir build -C Release --output-on-failure
python tools/merge_preview.py --fetch
```

The Qt runtime must be available when running desktop tests. GUI tests use temporary history/settings and offscreen rendering. Merge preview checks the entire working tree, including new files, against the freshly fetched upstream ref using a temporary index. It does not stage files, change branches, make a named commit or publish anything. A clean result applies only to that recorded upstream revision; rerun before opening or updating a PR.

`roclint_host --dry-run --db :memory:` checks QML startup without hotkey registration, accessibility observation or model startup. Quit any older resident instance before launching the updated normal app.

To compile a preview while the current executable is still open, set `-DROCLINT_HOST_OUTPUT_DIRECTORY="<absolute preview folder>"` when configuring the existing build. CMake places the host and its runtime files there (with a `Release` subfolder for Visual Studio); tests remain in the build's usual output directory. Deploy the matching Qt runtime to the preview folder or provide its `bin` directory on PATH. Set this option back to an empty string to restore the default output location. Building a preview does not stop the running app; it also does not replace it with the new code.

Upstream diagnostic flags are retained: `--scope global|local`, `--open-file-dropdown`, `--resolve N` and `--enhance`. `--workspace PATH` and `--file RELATIVE_PATH` supply real scope targets. A requested choice waits for completed analysis; `--enhance` requests a proposal and stops for review, without approving or copying generated text.

## Review and compatibility

[Structure and vision audit](docs/STRUCTURE_AND_VISION.md) maps the retained upstream layout to the supplied Stardust contracts, records all 96 original paths, and distinguishes implemented features from required gaps and planned RAG. [Bounded assumption scoring](docs/ASSUMPTION_SCORING.md) records the mixed-batch limits, v3 artifact and compatible history reader. [Final scored analysis](docs/FINAL_ANALYSIS_SNAPSHOT.md) describes the post-scorer artifact, downstream binding and schema-v5 history migration.

Run `python tools/check_structure.py --details` for the file-by-file comparison with the supplied upstream snapshot: modified originals, unchanged originals, additions and missing paths. New source directories must stay within the reviewed module boundaries recorded in `docs/alignment_baseline.json`.

[Versioned commands](docs/VERSIONED_COMMANDS.md) records the observed-version API and atomic state/artifact writes for contributors. [Upstream integration](docs/UPSTREAM_INTEGRATION.md) records retained features, intentional behavior/API changes, coordinated patch boundaries and remaining release gaps. [Current implementation](docs/CURRENT_IMPLEMENTATION.md) describes the development runtime. The upstream [project overview](ROCLINT_PROJECT_OVERVIEW.md) and [architecture dossier](docs/ROCLINT_ARCHITECTURE_AND_GOALS.md) are preserved as upstream planning/history; their latency, inference, privacy and platform-validation claims are not evidence for this build.
