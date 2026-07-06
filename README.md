# TH8

**A small, embeddable, security-hardened Tcl-compatible scripting
language for production use.**

TH8 takes the language that has run shipping software for thirty
years and rebuilds it around three commitments: the embedder
controls every capability the script can reach, every script that
runs in production can be cryptographically pinned to its author if
the embedder asks for that, and the C ABI is small enough that a
single team can keep it in their head.

It is the fourth member of a family.  **TH1** -- the scripting
engine inside the Fossil source control system -- proved fifteen
years ago that a minimal Tcl interpreter can safely process
untrusted input in an embedded context.  **Tcl 8.x** contributes the
language definition itself, eighty thousand existing tests, and the
well-worn "everything is a string" data model.  **JimTcl**
demonstrated that a clean-room re-implementation of Tcl can stay
single-file embeddable while picking up modern features (object
references, lambdas, native arrays) without abandoning the
language's surface contract -- TH8 follows the same shape (compact
C core, no required external dependencies, BSD-style license) while
applying its own discipline to security and conformance.  **Eagle**,
written and shipped by the same author since 2008, contributed the
security thinking, the Spilornis list parser, the Harpy
code-signing concept, the `ConvertUTF_v2` UTF-8 layer, and eighteen
years of lessons learned running untrusted code inside other
people's products.

TH8 is what you get when you take those four lineages, throw nothing
away, and rewrite the interpreter from scratch with the assumption
that the script *will* be hostile and the embedder *will* care.

---

## Why might you want this?

  * **You ship a product** and you want an extension language that
    is not a one-way door to your customer's machine.  TH8's default
    is *zero* capability: a freshly created interpreter cannot read
    a file, open a socket, allocate without a quota, or call into a
    plugin you did not register.  You opt features in by name.

  * **You sign your scripts.**  TH8 ships with an optional
    "signed-only" mode that refuses to evaluate any script whose
    `.b64sig` companion does not verify against a key you trust.
    The signing pipeline is built-in (see `tools/signScript.sh`);
    the verification path is part of the policy callback every
    interpreter carries.

  * **You audit your dependencies.**  Around 65,000 lines of C.
    No bytecode VM.  No threading model in the core.  Every public
    function in `src/th8.h` carries a header comment with its
    contract, its allocation behaviour, and its failure modes.
    The combined CPU surface for *all* commands fits in roughly
    1,200 source lines per plugin.

  * **You need it to actually build on the platforms you actually
    ship to.**  macOS (the reference platform), Linux, Windows,
    and Cosmopolitan APE (one binary that runs on Linux + macOS +
    Windows + the BSDs) are all first-class targets.  iOS and
    Android build cleanly; CI for those two is in progress.  See
    `docs/public/RELEASE_NOTES.md` for the current support matrix.

  * **You want a thing you can hold in your head.**  The full Tcl
    language standard for TH8 is `docs/public/tcl_language_standard_v1.md`.
    The full embedding API is `docs/public/th8_api.3` (a single man
    page).  The full scripting surface is `docs/public/th8.1`.  That
    is the contract.

---

## Key features

  * **Tcl 8.6 source compatibility** with the language standard
    written down explicitly, requirement by requirement, and tested
    against both TH8 and reference Tcl as part of the conformance
    suite.  Documented extensions (`nproc`, `napply`, `downlevel`,
    structured `binary` for bignums, cancel-with-unwind, etc.) live
    in `docs/public/th8_language_extensions.md`.

  * **Signed-script gate** ("Harpy"): RSA-16384 by default; SHA-256
    over canonicalised script bytes; per-script annotations
    (`<<notBefore:...>>`, `<<notAfter:...>>`, `<<flags:...>>`) parsed
    by the loader.

  * **Capability-based plugin model.**  The host calls
    `Th8_RegisterPlugin(interp, "io")` to expose I/O,
    `Th8_RegisterPlugin(interp, "filesystems")` to expose the
    filesystem, and so on.  An interpreter that has registered no
    plugins is genuinely sandboxed.

  * **Thread-safety where it matters, not where it doesn't.**  An
    interpreter is single-threaded by contract.  Cross-thread
    primitives (cancellation, async I/O dispatch) are explicit, own
    their synchronisation, and cache their function pointers.

  * **Fault injection built in.**  Every allocation, every platform
    callback, every channel read can be made to fail under test
    control via `Th8_FaultConfig`.  The conformance suite exercises
    error paths the same way it exercises success paths.

  * **MC/DC-instrumented coverage** as a first-class concern, not a
    final-mile afterthought.  `make mcdc` produces a per-condition
    coverage report.  The convention for decomposing compounds so
    clang can attribute MC/DC is written down in
    `docs/internal/FINDINGS.md` Finding 005.  As of the 1.0.0
    release, lib-wide MC/DC sits at roughly **87%** with the
    long-tail uncovered arms documented by category.

  * **One-binary deployment via Cosmopolitan.**  `make cosmo` builds
    `th8sh.com`, a single Actually Portable Executable that runs on
    Linux, macOS, Windows, FreeBSD, OpenBSD, and NetBSD.

---

## Quick taste -- ten lines of embedding

```c
#include "th8.h"

int main(void) {
    Th8_Interp *interp = Th8_CreateInterp(Th8_GetPosixPlatform());
    Th8_RegisterLanguage(interp);            /* core language only */
    Th8_RegisterPlugin(interp, "lists");     /* opt in to [list]/[lindex]/... */

    if (Th8_Eval(interp, "lindex {a b c d} 2", -1) != TH8_OK) { /* "c" */
        fprintf(stderr, "th8: %s\n", Th8_GetResult(interp, NULL));
    } else {
        printf("result: %s\n", Th8_GetResult(interp, NULL));
    }
    Th8_DeleteInterp(interp);
    return 0;
}
```

Compile against the static library:

```
clang -Isrc -o demo demo.c bin/libth8.a -lpthread -ldl
```

The full embedding walkthrough lives in
[`docs/public/quickstart_embedding.md`](https://github.com/mistachkin/th8/blob/trunk/docs/public/quickstart_embedding.md).
The scripting-side walkthrough is
[`docs/public/quickstart_scripting.md`](https://github.com/mistachkin/th8/blob/trunk/docs/public/quickstart_scripting.md).

---

## A quick taste from the scripting side

```tcl
proc fact n {
    if {$n < 2} { return 1 }
    expr {$n * [fact [expr {$n - 1}]]}
}

puts [fact 12]                       ;# -> 479001600
puts [string toupper "hello, th8"]   ;# -> HELLO, TH8

# Channels and signed-only loads cooperate.  This refuses to run
# unless source.tcl has a valid source.tcl.b64sig alongside it
# AND a signed-only key fingerprint registered with the interp.
source source.tcl
```

---

## Getting it built

```
git clone --recurse-submodules https://github.com/mistachkin/th8.git
cd th8
make fresh           # release: clean build + audit + tests/all.tcl
make ENABLE_TEST_KEY=1 clean debug   # debug build (for hacking)
make mcdc-report     # MC/DC coverage summary
```

If you forgot `--recurse-submodules`, run
`git submodule update --init --recursive` once.

For Cosmopolitan (one binary, every desktop OS):

```
bash tools/bootstrap_cosmo.sh
make -f Makefile.cosmopolitan
```

For Windows (MSVC nmake):

```
# Build the Win32 third-party libraries first.  Each external's
# README in externals/<name>/ documents the upstream source URL
# and the build_win64.bat / build_win32.bat invocation.
cd externals\openssl && build_win64.bat
cd externals\zlib    && build_win64.bat
cd externals\curl    && build_win64.bat
cd externals\tcl     && build_win64.bat
cd ..\..
nmake /f Makefile.msc
```

The CI matrix in
[`.github/workflows/ci.yml`](https://github.com/mistachkin/th8/blob/trunk/.github/workflows/ci.yml) is the
authoritative build recipe and runs nightly.

---

## Project mission and standards

A core part of why this project exists is to maintain and continually
improve **the highest level of coding quality, formatting discipline,
documentation completeness, test coverage, and security posture we
can reach.**  That sentence is meant literally; the day-to-day
consequences are:

  * **No bug is too small.**  A wrong word in a comment, a stray
    blank line, an unchecked return code in a path the compiler
    "can never reach" -- all of these are real issues that get
    real fixes.
  * **No detail of formatting is minor enough to ignore.**  The
    `.clang-format` configuration, the Tcl style guide, the
    requirement-marker discipline (`R-NNNNN-NNNNN`), the test-file
    prologue/epilogue convention, the audit-pattern banned-construct
    list -- all of these are project standards, and they apply
    uniformly to human-authored and AI-authored changes alike.
  * **Code is meant to be beautiful.**  Easy to read.  Easy to
    audit.  Well-documented function by function.  Tested by
    purpose, not by accident.  Full test coverage as a baseline,
    MC/DC coverage as the target, with intentional exemptions
    written down and justified.
  * **Security posture improves continuously.**  TH8 is intended to
    serve as a trusted, capability-restricted scripting sandbox for
    serious workloads -- including agentic collaboration contexts
    where the script source is opaque or generated, the host
    embedder is the only authority, and a compromise of the
    interpreter is a compromise of the entire embedding product.
    Every release should narrow the trusted computing base, not
    widen it.

This is not perfectionism for its own sake.  It is what it takes
for an embedded scripting language to be safe to run in production
in 2026 and beyond.

---

## Contributing

**All pull requests will be considered seriously**, provided the
project rules are followed (and any CLA that has been adopted is
signed).  Concretely:

  * **Use whatever AI tools you want.**  Claude, Copilot, Cursor,
    or your own home-grown agent: all are welcome, as long as the
    final output meets the project's quality bar.  We do not gate
    on tool choice; we gate on outcome.  See
    [`CLAUDE.md`](https://github.com/mistachkin/th8/blob/trunk/CLAUDE.md) for the kind of standing-rules document
    that helps an agent stay aligned.
  * **Bug fixes** are always welcome.  See
    [`CONTRIBUTING.md`](https://github.com/mistachkin/th8/blob/trunk/CONTRIBUTING.md) for the workflow, the
    R-marker / signing dance, and the audit / coverage gates a
    change has to pass before merge.
  * **New features** are welcome too, as long as they follow
    relevant industry standards.  If you want to add an XSLT 2.0
    parser, it had better follow the W3C XSLT 2.0 specification.
    If you want to add a new language construct, it had better
    follow the Tcl language standard and have the corresponding
    R-markers and conformance tests.  Features are not accepted
    "approximately right and we will polish later"; they are
    accepted in finished form, with tests, with documentation, and
    with the same formatting discipline as the rest of the tree.
  * **Discussion of the Tcl Language Standard itself is welcome.**
    [`docs/public/tcl_language_standard_v1.md`](https://github.com/mistachkin/th8/blob/trunk/docs/public/tcl_language_standard_v1.md)
    is the working text that the conformance suite tests against.
    We want it to be of the highest possible quality before we
    submit it to ISO.  Proposals -- clarifications, additional
    R-markers for edge cases the current draft does not cover,
    rewording for precision -- are first-class contributions and
    should be filed as pull requests against the standard text
    itself, with rationale.

The project author (Joe Mistachkin) is the final authority on
whether a contribution lands, and on how the project's rules
apply to any individual case.  He is generally fair-minded; he is
not infinitely patient with rule-bending or with changes that
treat formatting, documentation, or test coverage as
afterthoughts.  Reading [`CONTRIBUTING.md`](https://github.com/mistachkin/th8/blob/trunk/CONTRIBUTING.md) before
your first pull request makes the review go faster.

---

## How this codebase was authored

TH8's design is human-driven.  The architecture, the security
model, the language-standard discipline, the
"every public function has a header" rule, the "Harpy / signed-only"
worldview, the choice to keep Tcl semantics rather than invent a
new language -- those are all calls the project's author made, often
informed by twenty-plus years of running embedded scripting languages
in other people's products.

That said, **a substantial fraction of the C, the Tcl test code, the
internal Findings notes, and the audit tooling was written in
collaboration with Claude (Anthropic's Claude Code agent).**  The
working pattern looked like this:

  * Joe picked a problem, sketched the shape of the answer, and laid
    down the invariants (e.g. "split per Finding 005 sec. 5b",
    "every public function gets a per-function header", "MC/DC must
    measurably move per iteration or the iteration is reverted").
  * Claude executed within those invariants: wrote the code, wrote
    the tests, kept the bug ledger in `docs/internal/incomplete.md`
    and the per-iteration analysis in `docs/internal/FINDINGS.md`,
    re-ran the suite, signed the scripts, and reported the delta.
  * Joe reviewed, corrected, sometimes reverted, sometimes said
    "yes exactly, keep doing that", and the loop continued.

The artefacts this style produced are visible in the repository:
  * `CLAUDE.md` at the repository root is the AI-agent guide.  It lays out
    how the codebase is organised, what conventions an agent must
    follow, where the standing rules live, and how the
    `tools/audit_patterns.tcl` gate works.  It is the single most
    important document for any AI agent (or human) coming to TH8
    fresh.
  * `docs/internal/FINDINGS.md` (excluded from the public release;
    kept in the contributor's tree) captures each engineering
    decision with a *why* and a *how-to-apply*, so the next
    iteration -- human or agent -- starts with full context rather
    than reconstructed guesswork.
  * The Tcl conference paper in `docs/public/tcl_conference_2026_paper.md`
    documents the agent/human interaction case studies in detail.

We think this style of collaboration is going to matter.  TH8 is one
data point: a security-conscious systems C project of nontrivial size,
co-authored end-to-end with an AI agent, shipped as 1.0.0 with the
discipline and the auditability intact.  If you are building
something similar, the artefact you should read first is `CLAUDE.md`
-- it is a working example of the kind of standing-rules document
that lets an agent stay aligned across hundreds of iterations.

---

## Where to read next

  * [`CLAUDE.md`](https://github.com/mistachkin/th8/blob/trunk/CLAUDE.md) -- AI-agent guide; also the densest
    single-document tour of the codebase for any new human reader.
  * [`docs/public/quickstart_embedding.md`](https://github.com/mistachkin/th8/blob/trunk/docs/public/quickstart_embedding.md)
    -- "I want to embed TH8 in my C app."
  * [`docs/public/quickstart_scripting.md`](https://github.com/mistachkin/th8/blob/trunk/docs/public/quickstart_scripting.md)
    -- "I want to write TH8 scripts."
  * [`docs/public/security_model.md`](https://github.com/mistachkin/th8/blob/trunk/docs/public/security_model.md)
    -- the threat model and what TH8 will and will not defend
    against.
  * [`docs/public/tcl_language_standard_v1.md`](https://github.com/mistachkin/th8/blob/trunk/docs/public/tcl_language_standard_v1.md)
    -- the language standard, with R-marker requirements for every
    behaviour the conformance suite tests.
  * [`docs/public/th8_api.3`](https://github.com/mistachkin/th8/blob/trunk/docs/public/th8_api.3) -- the full C
    embedding API in one man page.
  * [`docs/public/th8_public_c_api_specification.md`](https://github.com/mistachkin/th8/blob/trunk/docs/public/th8_public_c_api_specification.md)
    -- the normative `TH8_API` specification, with R-marker
    requirements.  Companion:
    [`docs/public/th8_internal_api_specification.md`](https://github.com/mistachkin/th8/blob/trunk/docs/public/th8_internal_api_specification.md)
    documents the `TH8_INTERNAL` helpers exposed to test code
    and stub-linked extensions via `Th8_GetInternalStubs`.
  * [`docs/public/th8.1`](https://github.com/mistachkin/th8/blob/trunk/docs/public/th8.1) -- the `th8sh` shell man
    page.
  * [`docs/public/RELEASE_NOTES.md`](https://github.com/mistachkin/th8/blob/trunk/docs/public/RELEASE_NOTES.md) --
    what is in 1.0.0, what is intentionally deferred, what is
    Tier-1 vs Tier-2 supported.
  * [`CONTRIBUTING.md`](https://github.com/mistachkin/th8/blob/trunk/CONTRIBUTING.md) -- how to build, how the
    test suite is organised, how R-markers and signed scripts work,
    how to file a useful bug report.
  * [`SECURITY.md`](https://github.com/mistachkin/th8/blob/trunk/SECURITY.md) -- how to report a security issue
    responsibly.

---

## License

TH8 is distributed under a Tcl/BSD-style permissive license with an
explicit commercial-use transparency clause.  See
[`license.terms`](https://github.com/mistachkin/th8/blob/trunk/license.terms) for the exact wording.

Short version: use it, modify it, ship it.  If you are a large
commercial operator building a substantial commercial product on
top of it, the author reserves the right to publicly identify you
as such and to ask you to enter a paid commercial agreement.  Read
the license file before you ship.

---

*TH8 is written by Joe Mistachkin (Eagle, Mistachkin Systems) and a
long string of Claude agent sessions, in approximately that order of
authority.*
