# Actual Julia routing probe — October 27, 2026

36 initial prompts; 13 ORIGINAL, 23 CLARIFY, 0 UNCERTAIN_BUFFER, 0 errors. Twenty repeat evaluations of ten initial cases reproduced exactly the same scores and routes. Three additional wording variants were each evaluated twice.

## Scope

Actual installed pinned Julia inference, CPU FP32, eight threads, temperature 1.0. No selected workspace. Used the freshly built workspace helper, metadata with extended_heads=true as the current host requests, the current Python worker, and freshly compiled current C++ sealing/routing. This is a model-and-routing probe, not an end-to-end Qt or Granite test. No generated options, rewrite, approval, insertion, or downstream task execution was performed.

The default routing thresholds are ACTION/TARGET/RULE >= 0.70, RULE_CONFLICT >= 0.60, with an uncertainty band of 0.08 below each threshold. ORIGINAL and UNCERTAIN_BUFFER skip automatic Granite discovery; explicit Review prompt remains available after valid classification. CLARIFY requests Granite review and does not guarantee a question or options.

## Initial results

| ID | Exact prompt | Route | Action | Target | Rule | Conflict |
|---|---|---|---:|---:|---:|---:|
| 1 | make my webiste cool | CLARIFY | 0.2112 | 0.0000 | 0.0029 | 0.9879 |
| 2 | make me a spoflity local app for my android | CLARIFY | 0.0815 | 0.1075 | 0.0001 | 0.9947 |
| 3 | give my app like a modern style | CLARIFY | 0.0164 | 0.0001 | 0.7405 | 0.7646 |
| 4 | make my old website modern | ORIGINAL | 0.0010 | 0.0001 | 0.0604 | 0.4195 |
| 5 | Make my website cool. | ORIGINAL | 0.3524 | 0.0000 | 0.0000 | 0.0148 |
| 6 | Build a Spotify-style local music player for Android. | CLARIFY | 0.0001 | 0.0008 | 0.9976 | 0.9980 |
| 7 | Give my app a modern style. | ORIGINAL | 0.0196 | 0.0001 | 0.1917 | 0.3325 |
| 8 | Modernize my old website. | CLARIFY | 0.1038 | 0.2045 | 0.9970 | 0.9549 |
| 9 | Make the homepage feel modern and playful. | ORIGINAL | 0.0011 | 0.0000 | 0.0218 | 0.0003 |
| 10 | Give the landing page a friendlier vibe. | CLARIFY | 0.9938 | 0.0000 | 0.5312 | 0.6136 |
| 11 | Suggest three visual directions for my personal website. Do not change any code. | ORIGINAL | 0.0006 | 0.0000 | 0.0005 | 0.0000 |
| 12 | Brainstorm five names for a local music player. | ORIGINAL | 0.0030 | 0.0013 | 0.0081 | 0.0085 |
| 13 | In styles.css, change the body background to #121212 and the text color to #ffffff. | CLARIFY | 0.0051 | 0.0010 | 0.0002 | 0.7759 |
| 14 | Change the primary button border radius to 12px in styles.css. | ORIGINAL | 0.0025 | 0.0000 | 0.0002 | 0.0001 |
| 15 | Replace the string 'v1' with 'v2' in config/version.json. | CLARIFY | 0.0000 | 0.0002 | 0.0000 | 0.9637 |
| 16 | Rename the local variable offset to cursor in readPacket. Preserve behavior. | CLARIFY | 0.9962 | 0.0029 | 0.0014 | 0.0699 |
| 17 | In README.md, correct the spelling of 'recieve' to 'receive'. | CLARIFY | 0.0000 | 0.0007 | 0.0043 | 0.9303 |
| 18 | In config/settings.json, change max_workers from 4 to 8. Leave all other keys unchanged. | ORIGINAL | 0.2808 | 0.0551 | 0.0024 | 0.3900 |
| 19 | Summarize the supplied notes in three bullet points. | ORIGINAL | 0.0096 | 0.0017 | 0.0026 | 0.0026 |
| 20 | Write a friendly two-sentence thank-you note. | ORIGINAL | 0.0114 | 0.0072 | 0.0012 | 0.1360 |
| 21 | Translate 'Good morning' into Spanish. | CLARIFY | 0.3935 | 0.0003 | 0.7855 | 0.0015 |
| 22 | Sort these numbers in ascending order: 9, 2, 5. | CLARIFY | 0.0056 | 0.9920 | 0.0067 | 0.0350 |
| 23 | What is 2 + 2? | CLARIFY | 0.9960 | 0.7309 | 0.0003 | 0.9055 |
| 24 | Explain what a Python dictionary is in one sentence. | CLARIFY | 0.0007 | 0.0000 | 0.0052 | 0.7525 |
| 25 | What does git status do? | CLARIFY | 0.0282 | 0.9996 | 0.0039 | 0.0683 |
| 26 | Explain what the calculate_checksum() function in src/crc32.c does. Do not edit code. | CLARIFY | 0.0001 | 0.0000 | 0.4353 | 0.6921 |
| 27 | Clean up this module and make it faster. | CLARIFY | 0.7227 | 0.1850 | 0.9889 | 0.5671 |
| 28 | Fix my app. | CLARIFY | 0.0231 | 0.1811 | 0.0027 | 0.7323 |
| 29 | Make the dashboard load faster and tidy the styles. | ORIGINAL | 0.3596 | 0.0003 | 0.0299 | 0.1369 |
| 30 | Add a search bar to my app. | CLARIFY | 0.0291 | 0.0592 | 0.5517 | 0.9887 |
| 31 | Add login to my app. | CLARIFY | 0.0172 | 0.9636 | 0.1669 | 0.9382 |
| 32 | Make the button blue. | CLARIFY | 0.9763 | 0.0008 | 0.6446 | 0.4048 |
| 33 | Implement step 1 from the plan. | ORIGINAL | 0.5781 | 0.0103 | 0.2557 | 0.3580 |
| 34 | Fix the checkout error: Cannot read properties of undefined (reading 'cartId'). | CLARIFY | 0.0723 | 0.9448 | 0.9954 | 0.9987 |
| 35 | Use exactly three bullet points. Use exactly five bullet points. | CLARIFY | 0.7436 | 0.0005 | 0.0038 | 0.8845 |
| 36 | Return plain text only. Wrap the answer in HTML. | ORIGINAL | 0.0071 | 0.0000 | 0.0184 | 0.0015 |

## Wording sensitivity

Changing capitalization alone flipped the route in these exact inputs. Both variants were evaluated twice with identical results within each variant.

| Prompt | Route |
|---|---|
| make my website cool | CLARIFY |
| Make my website cool | ORIGINAL |
| make my website cool. | CLARIFY |
| Make my website cool. | ORIGINAL |

## Findings

- Broad creative briefs can bypass review, including several examples supplied in the conversation.
- The contradictory plain-text/HTML instruction bypassed review and reproduced on both reruns.
- Clear exact edits can trigger review: the version-string replacement reproduced on both reruns.
- Some apparent conflict scores originate in the extended SECURITY_BYPASS head. The worker folds that head into RULE_CONFLICT when it scores at least 0.70. For the version-string replacement, the raw conflict head was 0.1187 but SECURITY_BYPASS was 0.9637, producing the final conflict score. This identifies the routing mechanism, not why the model made that prediction.
- Source examples describing what Granite should offer are not evidence that Julia will automatically invoke Granite for those same prompts.
- These are selected diagnostic examples, not a labeled accuracy benchmark. No population-level error rate is established.

## Reproduce and inspect

- [Initial script](../../build/research/probe_current_julia.py)
- [Current C++ policy probe](../../build/research/route_current.cpp)
- [Initial full results](../../build/research/julia-current-routing-2026-10-27.json)
- [Repeat script](../../build/research/repeat_current_julia.py)
- [Repeat full results](../../build/research/julia-current-routing-repeat-2026-10-27.json)

The raw artifact records the pinned model ID, revision, weight checksum, runtime, exact requests, atomic scores, and SHA-256 hashes of relevant source files. Source hashes were rechecked after the run and had not changed. Probe files and raw results are in the local build directory.
