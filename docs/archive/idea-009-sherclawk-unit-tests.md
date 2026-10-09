# 009 — Unit testing with a runner built into Sherclawk

> Historical design note. Proposals, status statements and source-line references
> describe development at the time of writing and may be superseded. Use the
> [current guides](../../README.md#documentation) for supported behavior and the
> [issue tracker](https://github.com/lumberbarons/sherclawk/issues) for active work.

**Status:** idea, not implemented. Recorded October 7, 2026. Native builds,
artifact launch and guest-written diagnostic files already exist; the test
target, result protocol and model-facing test tool proposed here still need
implementation and OS 9.2.2 acceptance.

## The idea

Build the test runner into Sherclawk. Give the agent a `test_project(path)`
tool that compiles a project's tests, launches them, collects named results
and returns a structured summary. Include runnable test examples in the
PowerPC Toolbox starter so a newly created project demonstrates the whole
edit → test → repair → test workflow.

Sherclawk owns orchestration and result interpretation. Generated test code
executes in a separate native test application, built with MrC/PPCLink/Rez.
No separate launcher service, MacRelix session or host test command is needed
when Sherclawk's native build executor owns the queue.

Executing arbitrary test functions inside Sherclawk's process would let an
invalid pointer, abort or infinite loop take down the harness and its session.
A separate application reduces that exposure, but classic OS 9 has limited
process isolation: a guest-wide crash or a non-yielding application can still
prevent observation and recovery. A timeout is an observation deadline, not
a guarantee that Sherclawk can regain control.

## What exists today

- `build_project` validates protocol-2 descriptors and publishes immutable,
  revision-bound snapshots. The integrated native executor drives ToolServer
  asynchronously; the MacRelix worker remains an exclusive-owner fallback.
- `run_application(build_id)` verifies persisted artifact authority and both
  forks, launches the exact application and records a separate run ID with a
  native process observation. It explicitly reports
  `smoke_test: not_performed`.
- Native diagnostics already write logs in the guest; for example,
  `tools/native-process-check.c` writes and closes a runtime log on the AFP
  volume. These are fixed fixtures, not a general test-result protocol.
- Project descriptors currently describe one output, with at most five
  declared C/Rez/header inputs of 4 KiB each. They cannot yet express separate
  application and test targets.

Retro68 has an established automated-test workflow using CTest and LaunchAPPL,
including a shared-folder launch backend. That is a useful precedent for
launching test applications and collecting output, rather than a drop-in
solution for Sherclawk's native MPW builds. See the
[upstream documentation](https://github.com/autc04/Retro68#launchappl-and-the-test-suite)
and the local reference checkout's `AutomatedTests/CMakeLists.txt`.

## Proposed tool contract

`test_project(path)` performs a fresh build of the declared test target and
observes one test run. The tool should reuse the existing build and launch
machinery rather than duplicate their snapshot, journal and artifact checks.

1. Validate the descriptor and test inputs, including shared application
   logic. Publish a fresh snapshot and build the test application.
2. On successful build only, reserve a unique test run ID and result folder.
   Verify the test artifact's authority and journal launch intent.
3. Launch the exact test application with a versioned configuration identifying
   the build, test run and result destination.
4. Poll results in bounded steps while servicing the normal event loop.
5. Validate a completed report, retain the evidence and return a bounded
   summary with IDs, counts and failed checks. Provide continuation for
   longer diagnostics.

Report the test outcome separately from tool execution errors:

| Outcome | Meaning |
|---|---|
| `passed` | A complete, valid report contains at least one executed check and zero failures |
| `failed` | A complete, valid report contains one or more failed checks |
| `build_error` | The test target did not build; no test application was launched |
| `incomplete` | No valid completion was observed: timeout, malformed report, missing report or observation interrupted |

An empty suite should report an explicit `no_tests` outcome rather than pass.
Preserve existing uncertainty semantics for ambiguous build/launch operations;
an incomplete test observation must not imply that the application was
cancelled or that replay is safe. Missing process evidence alone cannot
distinguish a crash from a normal exit or a very short run.

## Small C89 test support

Keep application logic independent of Toolbox UI calls where practical.
Compile the same logic sources into both the ordinary application and its
test application. Start with deterministic, pure C tests; later use the same
runner for File Manager and other Toolbox integration checks.

Supply a tiny C89 support library with named checks, failure diagnostics and
an explicit finish operation. Checks record failures and continue instead
of depending on `assert()` abort behavior. Test execution must not disappear
when `NDEBUG` is defined. The runner services events between cases; an
individual stuck case can still hang the test application.

Illustrative API, not implemented:

```c
CHECK_EQ("empty input", count_items(""), 0);
CHECK_EQ("two items", count_items("a,b"), 2);
```

The helper should evaluate each argument once and capture expected/actual
values plus the check name. Keep strings, check counts and report bytes
bounded, with explicit truncation. A failed check is evidence from the
project's tests, not proof of complete application correctness.

## Completion protocol

Use a fresh result folder per run so old output can never satisfy a new
request. A versioned report records the build ID, test run ID, named check
results and totals. Publish completion only after all required report data
has been successfully written and closed, using verified staging and rename
publication in the established File Manager style.

Sherclawk validates IDs, format, size limits, counts and completion before
accepting pass/fail. Partial reports can supply diagnostics but cannot pass.
Do not rely on Finder launch returning a Unix-style exit status, application
disappearance, or the mere existence of a result file.

Choose and guest-verify configuration delivery before committing the protocol.
A custom initial Apple event via `launchAppParameters`, or an open-document
event carrying a run configuration file, are candidates. There is no normal
Finder command line. Avoid a single global result pathname. Correlation and
artifact checks prevent accidental stale results; they do not make arbitrary
project-written tests a trusted security boundary.

Stop ends Sherclawk's observation and records that fact. It does not claim to
terminate the test application. Keep late results for inspection and never
automatically relaunch an uncertain run.

## Starter and descriptor changes

Suggested starter layout:

| File | Purpose |
|---|---|
| `main.c` | Toolbox UI calling shared application logic |
| `logic.c`, `logic.h` | Small function used by the app and its tests |
| `tests.c` | Test entry point and a few passing example checks |
| `test_support.c`, `test_support.h` | Configuration, named checks and completion reporting |
| `app.r` | Native application resources |
| `project.json` | Ordinary application and test target declarations |

Extend the descriptor with a structured test target: declared sources,
headers/resources, output and supported settings. Reuse shared sources by
reference rather than copying their implementation into tests. Do not accept
free-form shell or MPW commands. Decide whether this requires a new descriptor
protocol; unknown fields must continue to fail validation.

Increase the current five-input limit enough for this layout and verify the
snapshot/job limits together. Per-file growth belongs with
[idea 008](idea-008-sherclawk-editing.md); small examples should remain within the
current 4 KiB file limit initially. Update embedding, materialization, creation
checks and documentation together. Existing created projects stay unchanged.
Independently assembled projects must have equal access to the test tool.

## Implementation sequence and acceptance

1. **Fixed native fixture:** compile a small pure-C test application and have
   Sherclawk launch it, deliver run configuration and read a correlated,
   closed completion report. Verify one passing and one deliberately failing
   fixture in OS 9.2.2, with MacRelix quit.
2. **Result engine:** host checks for parsing, count consistency, bounds,
   missing/partial/malformed reports, stale IDs, collisions and late results.
   Guest acceptance for configuration delivery, report publication, short
   runs, launch errors, timeout and Stop. Never infer guest behavior from host
   checks.
3. **Project integration:** test target validation, snapshot/build/launch reuse,
   model-facing tool and bounded result continuation. Demonstrate a failed
   check, guarded source repair and a passing fresh build whose IDs and source
   revisions differ. Verify compiler failure launches nothing.
4. **Starter examples:** `create_project` produces a runnable passing suite.
   Also test an independently assembled project without template provenance.
   Verify ordinary app launch still works and the tests exercise the same
   logic used by the app.

Automated UI clicks, visual assertions, broad test framework compatibility and
force-termination are later work. The first useful increment is native unit
testing of shared C logic, orchestrated by Sherclawk itself.

## Local references

- [Build/run contract](../architecture.md#buildrun-contract)
- [Current project descriptor](../native-builds.md#native-project-builds)
- [Native build executor](idea-005-sherclawk-queue-mode.md)
- `build_project.c`, `selfbuild.c`, `jobs.c`, `run_application.c`
- `tools/native-process-check.c`, `tools/build-check.c`
- `templates/ppc-toolbox/`, `tools/embed-project-template.py`
