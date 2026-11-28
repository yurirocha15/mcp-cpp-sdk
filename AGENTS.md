These rules apply to every change to this repository, whoever or whatever writes it. The person who opens a pull request is responsible for every line in it and must be able to explain each one to a reviewer without help from an agent. `docs/contributing.rst` holds the setup, style and process details.

## Before writing code

- Check that nobody is already working on the same thing: `gh pr list --repo yurirocha15/mcp-cpp-sdk --state open --search "<keywords>"` and `gh search issues --repo yurirocha15/mcp-cpp-sdk "<keywords>"`. If an open pull request covers it, stop and point the user to it.
- Open an issue first for a new public API (anything under `include/mcp/`), a new dependency, a new transport, or a change that spans several subsystems. Small fixes need no issue.
- A bug fix starts with a test that fails on `main`. A suspected bug that no test or program reproduces is not ready for a pull request.
- Do not open a pull request for a lone typo, rename, reformat or style change. Bundle mechanical cleanups with substantive work.
- Do not open many pull requests or issues in a short time, drafts included. A contributor with no merged pull request keeps at most one pull request open, and waits for it to be merged or closed before opening the next; check with `gh pr list --repo yurirocha15/mcp-cpp-sdk --author @me --state open`. Batches of automated submissions are closed as spam.
- File one issue per problem you have reproduced. Do not file issues for findings you have not reproduced, and do not post comments asking to be assigned.
- If the work is a duplicate, busywork or unreproduced, do not start it. Tell the user why, with a link to the existing issue or pull request.
- Read the code around the change and follow its patterns. Search for an existing helper before writing a new one.

## Build and test

- Build and run the tests with `python scripts/build.py --test`.
- Format with `make format` and lint with `make lint` before committing.
- Add or update tests for any code you change.

## Code

- Let errors reach the caller. Do not add fallbacks, default values or catch-all handlers that turn a failure into a silent success.
- Never fix a race with a sleep or a longer timeout. Find the missing ordering and enforce it.
- Validate untrusted input once, at the boundary that receives it. Do not re-check values that a type or an earlier check already guarantees.
- Add an option, a state, an abstraction or a guard only for a requirement or a failing test, not for a case that might happen.
- Do not create a helper for one or two lines that are used once.
- Do not remove or weaken an assertion, a test or a check to make the build pass.
- Do not silence clang-tidy when the code can be fixed. When a suppression is needed, name the check and give the reason: `// NOLINTNEXTLINE(concurrency-mt-unsafe): read once, under the cache's mutex.`
- Before writing coroutine code, read "Known Issues" in `docs/contributing.rst`: GCC 11, 12 and 13, which the SDK supports, miscompile or crash on several coroutine patterns.

## Code comments

A comment says what the code cannot: a reason that is not obvious, an invariant, or a protocol or compiler constraint. Write the code first, then add a comment only where a reader who knows C++ and MCP would otherwise get it wrong.

- One short line is the norm. Use several lines only for an invariant or a compiler workaround that needs them.
- Describe the code as it is, in the present tense. Do not tell its history: what it used to do, what was fixed, what a review asked for. That belongs in the commit message.
- Do not refer to the task, the issue, the conversation or the user. The comment must make sense to a reader who never saw them.
- Be neutral and specific: name the type, function, constant or spec field, and the reason. No narrative, personification or praise.
- Do not repeat the code, the function name or the types. Do not add section or divider comments.
- Explain a rule once, where it is defined, not at every call site.
- Do not add comments to code you move or copy. Keep existing comments that are still correct, and fix or delete the ones your change makes wrong.
- Check every fact in a comment (a size, a limit, a library's behaviour, a spec rule) before writing it. Point to the constant or the document instead of repeating its values.
- Doxygen on the public API follows `docs/contributing.rst`. `@brief` is one line, and `@param` and `@return` say what the declaration does not: units, ownership, valid range, what zero or empty means.
- Use ASCII in new and changed comments: `-`, `->` and `...`, not dash, arrow or ellipsis characters.
- Before keeping a comment, ask whether a reader would need real effort to work out what it says. If not, delete it.

```cpp
// BAD: repeats the code
// Get the next request id
const int64_t id = state->next_request_id.fetch_add(1, std::memory_order_relaxed);

// GOOD: the code needs no comment
const int64_t id = state->next_request_id.fetch_add(1, std::memory_order_relaxed);
```

```cpp
// BAD: history of the change, written to the person who asked for it
// Fixed the hang you reported: before this change a cancelled request kept the replay waiting.
outstanding.erase(found);

// GOOD: the reason, in the present tense
// A cancelled request may never be answered, so stop waiting for it.
outstanding.erase(found);
```

```cpp
// BAD: narrative
// Now that the message is ready, we hand it over to the transport to work its magic.
co_await transport_->write_message(wire);

// GOOD: the call says what happens
co_await transport_->write_message(wire);
```

```cpp
// BAD: repeats the declaration
/**
 * @brief Sets the capacity.
 * @param capacity The capacity.
 */
void set_capacity(std::size_t capacity);

// GOOD: says what the declaration does not
/**
 * @brief Limits how many messages may wait to be sent.
 * @param capacity Zero means no limit.
 */
void set_capacity(std::size_t capacity);
```

## Tests

- A new test must fail without the change. For a bug fix, run it against the unchanged code first and see it fail.
- Add the test to the existing file for the code under test, as `docs/contributing.rst` describes. Create a file only when none fits.
- Test through the public API (`mcp::Client`, `mcp::Server`, the transports) unless the behaviour cannot be reached from it.
- Assert the observable result. A test that would still pass with the code broken, such as one that only checks that nothing throws or only checks a mock, does not count.
- Do not wait for a condition with a fixed sleep. Wait for the event, or poll for it with a deadline. Timers are fine when time is what the test is about, such as a timeout.
- Do not add tests only to raise coverage, and do not add tests that check that a removed feature is gone.
- Test one behaviour per test, with the smallest setup that shows it. If the test diff is much larger than the code change, cut its scope.

## Commit messages

- Use conventional commit format for every commit subject, matching `docs/contributing.rst`: `feat:`, `fix:`, `docs:`, `style:`, `refactor:`, `perf:`, `test:`, `build:`, `ci:`, or `chore:` followed by an imperative summary under 80 characters.
- Before pushing, check branch commit subjects with `git log --format='%s' <base>..HEAD` and reword any non-conforming commits.
- The body explains why, and describes the final change. Do not narrate how it was made: no review rounds, fixes to earlier commits, dropped attempts, tools or agents.

## Pull requests and other public text

These rules cover pull request titles and descriptions, issues, review comments, commit messages, `CHANGELOG.md` and the documentation.

- Start with what changed and why, in two short sentences. Add detail only where a reviewer needs it.
- Describe the final change and the evidence for it. Do not mention discarded approaches, intermediate edits, review history, tools, agents or the session that produced it.
- Check every factual claim before writing it: behaviour, compatibility, limits, spec rules, performance. A performance claim says what was measured, how, and on which workload, and reports regressions as well as gains.
- The Testing section names the commands that ran, and what did not run with the reason. Do not give test counts.
- Refer to code by name (type, function, file), not by line number.
- For a security fix, describe what the code now does, not how the weakness could be used.
- Do not use headings, bold text or emoji in comments.
- An agent never pushes, opens a pull request or an issue, or posts a comment or a review without the user approving the exact text and action first.

## Before pushing

1. Review the whole diff against `origin/main` as a strict reviewer would: what are its flaws, and what would a maintainer push back on? Fix them, or say in the pull request why they stay.
2. Check the added lines for leftovers with `git diff origin/main... | grep -nP '^\+.*(NOLINT|TODO|FIXME|XXX|[^\x00-\x7F])'`. Fix each hit unless it is intended.
3. Remove files that are not part of the change: notes, plans, logs, scratch scripts and test output.
4. Keep one logical change per pull request, and split unrelated changes.

## Changelog

- Give each change users notice one entry under `## [Unreleased]` in `CHANGELOG.md`, as `docs/contributing.rst` describes: one sentence of at most 200 characters about what changed for users. The documentation holds the details.

## CI guardrails

- Keep example runner status output ASCII-only. Windows GitHub runners can use legacy console encodings such as cp1252, so Unicode symbols in `scripts/run_examples.py` can fail before examples finish.
- When discovering example binaries on Windows, run only `.exe` files. MSVC can emit `.exe.manifest` sidecars next to examples, and trying to execute those manifests fails with `WinError 193`.

## Changing this file

- Keep this file short and specific. A new rule must be non-obvious, must have come up more than once, and must say exactly what to do. Describe traps to avoid, not the architecture: agents can read the code.
- Propose a new rule in the pull request description of the change that ran into it. Maintainers decide whether it goes here.
