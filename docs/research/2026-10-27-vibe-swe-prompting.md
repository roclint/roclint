# Vibe coding and software engineering prompting: an evidence review

Research date: October 27, 2026 (America/Los_Angeles).

This is a qualitative desk-research snapshot for Roclint. It combines 2026 practitioner writing, public community discussions, empirical research, and direct inspection of GitHub repositories and selected prompt files. It does not measure market-wide prevalence or establish that any prompting practice causes better software outcomes.

## 1. Main finding

The sources support overlapping practices with different degrees of technical control. Both communities use natural-language goals, planning conversations, reusable instructions, visual feedback, and iteration. Experience appears to affect the context users supply and their ability to evaluate results. A prompt's length or use of aesthetic words is a weak basis for classifying its quality.

For Roclint, the useful research question is: **Which unresolved choices require human intent, given the context and authority already available to the downstream agent?**

That is a product hypothesis derived from this review, rather than a result established by a Roclint evaluation.

## 2. Scope and evidence quality

- **2026 evidence:** dated practitioner articles, 2026 community discussions, a survey with early-2026 data collection, and repository contents inspected on the research date.
- **Historical context:** the late-2025 video study associated with FSE 2026, a study explicitly about professional agent use in 2025, and older repositories still available in 2026. Publication, observation, and repository inspection dates are different things.
- **Social evidence:** public Reddit posts and comments document individual experiences. Occupation, project success, and representativeness are usually unverified. Some replies promote products.
- **Practitioner evidence:** first-person workflow descriptions and recommendations establish what their authors do or advocate. They do not establish what most developers do.
- **Vendor evidence:** product guides establish the supported or recommended workflow. Promises about speed, correctness, and business results are not independently verified here.
- **Repository evidence:** README and prompt contents establish what a project distributes. Stars are attention signals; they do not establish prompt effectiveness, active use, or which community uses it.
- **Access limits:** direct X posts by Boris Cherny and Andrej Karpathy returned access errors. Search snippets and third-party retellings were excluded from the core findings. Social conclusions below rely on accessible Reddit sources.
- **Date limits:** some cached Reddit pages expose inconsistent relative ages compared with indexed calendar dates. These discussions support qualitative examples, not a month-by-month trend analysis.

## 3. What the empirical research actually supports

### Experience changes the interaction, but the groups overlap

[From Prompting to Verification: How Experience Shapes Vibe Coding Practices](https://arxiv.org/html/2605.24521v1) is a May 2026 preprint. Its reported final sample contains 54 participants in each of three experience groups, or 162 total, recruited through Prolific; the questionnaire began in February 2026.

Non-developers reported more AI-led generation and less rich context provision. Professionals reported more interactive dialogue. Structured planning and verification-oriented prompting did **not** differ significantly across the groups in this sample. This does not prove equivalence. Experience-related differences also appeared in reprompting instead of debugging and QA confusion.

Its self-reported practices and experience categories cannot be treated as observed production outcomes or population prevalence.

### Observed vibe coding has multiple styles

[Building Software by Rolling the Dice](https://arxiv.org/html/2512.22418v1), initially posted in December 2025 and associated with FSE 2026, analyzes 20 videos, including seven live coding sessions and 254 prompts. It finds varied reliance on AI, evaluation practices, and mental models. Prompting includes personas, examples, exploration, and repeated attempts, alongside evolving requirements.

The authors identify loss of prior goals and decisions as a tool-design opportunity. This is a useful historical observational baseline, not a sample of the entire 2026 market.

### Professional control also takes multiple forms

[Professional Software Developers Don't Vibe, They Control: AI Agent Use for Coding in 2025](https://rlisahuang.com/files/agents_study_25.pdf) combines 13 field observations and 99 qualitative survey responses. It reports deliberate steering, context provision, and retention of design responsibility. Participants used screenshots as well as file references and examples.

Its 2025 scope matters: use it to understand mechanisms and older practice, not as direct evidence of current 2026 adoption. It also undermines the idea that visual context belongs exclusively to nontechnical builders.

## 4. Vibe-coding habits, prompts, and projects

### Product descriptions and visual intent are common entry points

Lovable's [May 6, 2026 finance-tracker guide](https://lovable.dev/guides/build-finance-tracker-app-no-coding) starts with a user's situation and desired behavior, then adds persistence, accounts, reports, and refinements. It recommends deciding access roles and scope early, and using direct visual edits for smaller interface changes.

The lesson is that an accessible product brief can contain meaningful requirements without file paths. This is vendor guidance, not a survey of actual behavior.

A [public animated-website project](https://www.reddit.com/r/vibecoding/comments/1q7b4c0/i_vibe_coded_this_animated_website/) describes generating visual assets, turning animation into frames, and supplying those assets to an AI coding environment. The short final request depends on a substantial visual reference. The author links a repository; the claimed build time and quality were not independently tested.

### Users ask one AI to prepare instructions for another

In [What is your workflow?](https://www.reddit.com/r/vibecoding/comments/1rw6uf8/what_is_your_workflow/), the original poster uses ChatGPT for ideation and Claude for implementation. Replies describe generated prompts, project-progress documents, lessons files, planning, and version control.

This suggests that the immediate message can be the end of an upstream specification process. It also creates a possible failure point: a generated prompt can introduce assumptions the user never chose.

### Some nontechnical builders deliberately add engineering structure

In [Tools and workflow](https://www.reddit.com/r/vibecoding/comments/1uh3zy3/tools_and_workflow/), a self-described nontechnical builder describes using layouts and palettes, trying the prototype, requesting a fresh-session audit, and fixing an issue checklist in stages.

Another [first-person workflow account](https://www.reddit.com/r/vibecoding/comments/1us85uw/i_dont_vibe_code_heres_the_actual_workflow_behind/) describes research, conversational specification, scoped implementation, and verification. Replies disagree about the terminology and review burden. These are examples of overlap, not evidence that all vibe coders follow this process.

### Repeated repair prompts and context recovery are real complaints

The [cycle-of-doom discussion](https://www.reddit.com/r/vibecoding/comments/1s5cenu/genuine_question_does_anyone_else_enter_the_cycle/) describes fixes leading to more fixes. Replies suggest reviewing changes, starting a fresh session, and preserving rollback points.

The thread supports a reported pain point. It does not establish that ambiguity caused the failures, or that a prompt rewrite would have prevented them.

### Project range

The sources include visual websites, personal budgeting tools, portfolios, hobby applications, and larger planned products. The KhazP workflow links its own portfolio, visualization, and animal-collection examples. These show that the scene extends beyond aesthetic requests, but repository showcases are not independently verified business outcomes.

## 5. SWE habits, prompts, and projects

### Plans, implementation chunks, and review

Addy Osmani's [January 4, 2026 workflow](https://addyosmani.com/blog/ai-coding-workflow/) describes requirements conversations, saved specifications, focused implementation steps, relevant context, tests, frequent commits, and project instruction files. It is a detailed first-person account, not evidence that professional chat prompts are universally short.

In an [October 2026 ExperiencedDevs discussion](https://www.reddit.com/r/ExperiencedDevs/comments/1xfhsp5/ask_experienced_devs_weekly_thread_a_weekly/), commenters describe plan–critique–implement–review workflows, narrowly directed coding, and competing views on delegation. The disagreement is important: professional users do not share one autonomy preference.

### Short prompts can invoke rich procedures

Simon Willison's [First run the tests](https://simonwillison.net/guides/agentic-engineering-patterns/first-run-the-tests/) describes a tiny prompt that establishes the test baseline and helps an agent discover project structure. His [red/green TDD chapter](https://simonwillison.net/guides/agentic-engineering-patterns/red-green-tdd/) uses a concise phrase to invoke a failing-test-then-implementation workflow.

These are counterexamples to treating brevity or several sequential actions as defects. The relevant procedure may already be understood by the agent and supported by the repository.

### Goals and rationale become durable context

Osmani's [June 5, 2026 intent-debt article](https://addyosmani.com/blog/intent-debt/) argues for externalizing goals, constraints, and reasons behind consequential choices. The article distinguishes recorded human rationale from a model's plausible reconstruction.

This aligns closely with Roclint's purpose. The potential value is preserving confirmed decisions and their reasons across revisions and sessions.

### Prompts are part of a larger operating system

Anthropic's [March 24, 2026 harness-development report](https://www.anthropic.com/research/harness-design-long-running-apps) describes decomposition, structured handoffs, context resets, and separation of generation from evaluation. It is an internal engineering experiment with specific models and tasks.

For Roclint, the implication is limited but important: better wording is only one intervention. Execution feedback, persistent state, and verification can address problems that a pre-execution prompt linter cannot observe.

### Project range

The professional sources cover existing-codebase maintenance, bounded fixes and refactors, tests, documentation, and feature development. The repositories also support greenfield prototypes and product discovery. Project type alone does not cleanly divide the communities.

## 6. Repositories and cookbook-style prompts

Counts below were read directly through the GitHub connector on October 27, 2026. Audience fit is my classification of the distributed material, **not measured adoption by a camp**. These are selected relevant repositories, not an exhaustive popularity ranking.

| Repository | Stars | Material actually inspected | Audience fit / caution |
|---|---:|---|---|
| [KhazP/vibe-coding-prompt-template](https://github.com/KhazP/vibe-coding-prompt-template) | 3,192 | README and PRD-generator prompt | Explicitly accommodates vibe coders, developers, and intermediate users; research → PRD → technical design → agent instructions. |
| [snarktank/ai-dev-tasks](https://github.com/snarktank/ai-dev-tasks) | 7,794 | README and create-prd.md | Accessible PRD and task-list recipes with review checkpoints; latest default-branch commit is from November 2025. |
| [coleam00/context-engineering-intro](https://github.com/coleam00/context-engineering-intro) | 13,893 | README and INITIAL.md | Feature requests grounded in examples, documentation, project rules, and validation. Bridges the labels. |
| [bmad-code-org/BMAD-METHOD](https://github.com/bmad-code-org/BMAD-METHOD) | 54,637 | Current main README | Product/architecture/development workflows scaled to the task. Main advertises preview work alongside a stable release line. |
| [obra/superpowers](https://github.com/obra/superpowers) | 306,215 | README and brainstorming skill | Composable workflows for intent discovery, design, plans, implementation, and review. Includes explicit user choices. |
| [github/spec-kit](https://github.com/github/spec-kit) | 143,243 | Current README and workflow examples | Specification, planning, tasks, implementation, and convergence; also separate bug-fixing and idea-assessment paths. |
| [snarktank/ralph](https://github.com/snarktank/ralph) | 22,077 | README and prompt.md | Repeated agent execution of small PRD stories with persisted progress and quality checks; delegation through a loop. |
| [PatrickJS/awesome-cursorrules](https://github.com/PatrickJS/awesome-cursorrules) | 41,023 | README and its linked rule catalog | Stack- and task-specific persistent rules; a rules library rather than a project-building cookbook. |

Two additional observations:

- [gsd-build/get-shit-done](https://github.com/gsd-build/get-shit-done) had 64,204 stars but was **archived** in the inspected metadata. Its attention is relevant, but this snapshot does not establish a maintained successor.
- [KingLeoJr/awesome-vibe-coding-prompt-code-templates](https://github.com/KingLeoJr/awesome-vibe-coding-prompt-code-templates) had 275 stars and a catalog of application structures. It is a smaller supplementary example, not comparable evidence of popularity to the large workflow repositories.

Exact counts, default-branch commit IDs, commit dates, and metadata URLs are saved in [the repository snapshot](2026-10-27-repository-snapshot.json). A push timestamp is distinct from the latest commit timestamp.

## 7. What the actual prompt files reveal

### Clarification is already a reusable recipe

The [KhazP PRD generator](https://github.com/KhazP/vibe-coding-prompt-template/blob/d26fb0d8a9cdac94cf99f004087620487e4da33d/part2-prd-mvp.md) adapts its questions to the user's experience, reads supplied handoff context first, and preserves decisions and open questions.

[AI Dev Tasks' PRD prompt](https://github.com/snarktank/ai-dev-tasks/blob/efbffaac10e68c94e14aaa587c79b7d5015b5ebd/create-prd.md) asks essential questions about goals, scope, and success, using selectable answers. It tells the assistant to avoid questions whose answers are reasonably inferable. These are close precedents for lightweight intent elicitation.

### Context templates are more than prose polishing

[INITIAL.md](https://github.com/coleam00/context-engineering-intro/blob/a2d84b021cee1e2f4e77ba854bba0be8cb319035/INITIAL.md) separates the feature, examples, documentation, and other considerations. Its surrounding workflow uses that material to generate and execute a requirements prompt.

The distinction matters: a clear sentence can still lack crucial external context; a short sentence can be fully grounded by attached material.

### Even structured workflows distinguish obvious tasks from missing intent

The inspected [Superpowers brainstorming skill](https://github.com/obra/superpowers/blob/2ce8aa15d05bd57213e6f746f953aafb7ded7a8b/skills/brainstorming/SKILL.md) tells the agent to inspect existing context before asking, proceed directly on a clear simple request, and discuss consequential unknowns. It also distinguishes the agent's implementation choices from user agreement.

This is useful comparison material for Roclint's question burden and confirmation semantics.

### Multiple operations can form one coherent task

[Ralph's execution prompt](https://github.com/snarktank/ralph/blob/6c53cb0b831ebe8739c6a003e22af14902d8b0b5/prompt.md) combines reading requirements, selecting a story, implementing, checking, recording progress, and committing.

This directly challenges a blanket rule that analysis plus editing plus verification constitutes conflicting intent. Whether permissions and goals are compatible requires semantic assessment.

## 8. Comparison useful for Roclint

The following is my synthesis of the sources, not a measured segmentation model.

| Dimension | More product-led interaction | More implementation-led interaction |
|---|---|---|
| Starting description | Audience, workflow, screens, desired experience | Existing behavior, interfaces, invariants, change scope |
| Context | Visual references, examples, product notes | Relevant code, tests, docs, rules; sometimes visuals |
| Iteration | Try the result and describe the next change | Inspect behavior/diffs, verify, correct, commit |
| Clarification need | Product behavior and meaningful preferences | Compatibility, acceptance criteria, authority, tradeoffs |
| Common vulnerability | Difficulty diagnosing or evaluating generated systems | Review load, missed rationale, conflicting or stale context |
| Shared practices | Planning, small steps, reusable instructions, feedback, rollback | Planning, small steps, reusable instructions, feedback, rollback |

The same person can occupy either column on different tasks.

## 9. Implications and hypotheses to test

These are research-informed recommendations, not implementation changes made by this task.

1. **Assess material unknowns in context.** Distinguish unavailable context from genuinely absent user decisions. An omitted file path can be discoverable by the downstream agent.
2. **Preserve deliberate creative freedom.** Optional exploration should stay optional. A request can intentionally delegate visual choices.
3. **Separate sequential work from contradiction.** Investigate → edit → test can be a coherent authorized task.
4. **Carry decision provenance.** Preserve who supplied a requirement, the original wording, confirmed answers, reasons, and changes. Do not upgrade model suggestions into user decisions.
5. **Measure interruption cost.** A useful clarification should earn the user's attention by affecting a meaningful outcome.
6. **Account for persistent instructions.** Short follow-ups and slash commands may depend on supplied specs or rules. A standalone-text score cannot establish contextual ambiguity.
7. **Compare against existing clarification.** Lovable announced [planning conversations and follow-up questions in December 2025](https://lovable.dev/blog/chat-mode-and-questions), and [reusable skills in May 2026](https://lovable.dev/blog/introducing-skills). Asking questions alone is already available. Roclint's proposed differentiation needs to be demonstrated in decision fidelity, cross-tool use, and deterministic review/delivery controls.
8. **Keep prompt quality separate from execution quality.** A clearer instruction cannot by itself establish correct code, secure deployment, or faithful agent execution.

## 10. A better evaluation corpus

Build an independently labeled corpus before using these findings to tune routing. Keep repository-derived and synthetic cases separate from observed user prompts.

Include these task families:

- Product briefs with intentional creative latitude.
- Product briefs missing one consequential behavior.
- Symptom-based bugs where the agent can investigate the target.
- Genuine missing-target cases with no available grounding.
- Coherent multi-step engineering requests.
- Actual contradictory requirements.
- Short follow-ups grounded by prior specs or rules.
- Audit-only, implement-only, and explicitly audit-and-fix requests.
- Recovery after repeated failed edits.
- Optional wording improvement with no unresolved decision.

For each case, record the original message, accessible context, downstream agent capabilities, authorization, expected result, and which unknowns the agent can resolve itself. Label required clarification, optional help, or proceed separately from the classifier's categories.

Then compare the original, the user's normal tool workflow, and Roclint-assisted workflow on intent preservation, unnecessary interruptions, user effort, completion quality, and regressions. Preserve source/author grouping across train and held-out sets to reduce leakage. Keep classifier scores out of the human reference-labeling process.

No inference runs, model calibration, application edits, or downstream execution were performed for this research.
