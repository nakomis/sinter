# Multi-LLM consultation — workflow notes

A pattern that worked well during the SINT-34/SINT-39 design phase: bouncing
the same design questions off several free-tier LLMs in parallel, treating
the agreements as evidence and the disagreements as flags for things that
need outside verification.

Companion file: [[verify-against-silicon.md]] — what "outside verification"
actually means once you've stopped trusting the LLMs.

## Why it works

A single LLM, no matter how confident, is one source. Four LLMs that
independently arrive at the same answer to the same question give you four
failure modes between them and the truth — *if* the question was posed in a
way that doesn't telegraph the desired answer. The technique only works
when the framing actively invites disagreement.

What this surfaced during the Sinter work that a single-model consultation
wouldn't have:

- **Gemini** confidently invented a DualBIOS hardware checksum gate and an
  "ATX standby live-ground trick" that would have damaged the board.
- **Grok** suggested `invd` for self-modifying code, which discards dirty
  cache lines without write-back and is a footgun.
- **Copilot** produced confident-but-arithmetically-wrong MSR values for a
  worked MTRR example.
- **Claude** (this assistant) "corrected" Copilot's MSR values with an
  equally-confident derivation that also wants independent verification.

Each error was caught because the *other* models, asked the same question,
gave a different answer — never because any single LLM noticed its own
error.

## The prompt template

Three structural choices that made the cross-referencing tractable:

1. **A single preamble describing the project and the immediate blocker.**
   Same paragraph fed to every model, no per-model tailoring. This makes
   the responses comparable.
2. **Numbered specific questions, not free-form discussion.** Each model
   answers question 5 in a way that can be put next to the other three
   answers to question 5.
3. **Explicit permission to be blunt.** The closing line —
   *"Be blunt about anything that's myth, half-true, or specific to a
   different chipset / situation than the one described. Call out anything
   that's wrong, unverified, or actively dangerous rather than telling me
   what I want to hear"* — is the single most important sentence in the
   prompt. Without it, you get four flavours of yes-and-here's-some-extra-
   confidence. With it, models actively disagree with the framing, which
   is where the value comes from.

The full preamble + 9 questions used for Sinter is preserved verbatim in
`research/sinter-project-assessment-chatgpt.md` as a template.

## The workflow

1. **Draft the preamble + numbered questions** with the blunt-framing closer.
   Iterate on it once locally; don't tweak per model.
2. **Copy to clipboard** via `pbcopy` (so the same text goes to each model
   verbatim — no accidental drift from retyping).
3. **Paste into each model in turn.** Don't share earlier responses between
   models; you want independent samples, not a relay.
4. **Save each response verbatim** to `docs/notes/research/<topic>-<model>.md`
   with provenance (source file, conversion method if a PDF, original
   deletion date).
5. **Read all the responses together** before drawing conclusions. The
   four-up comparison is the actual analytical step; reading one at a time
   means each one shapes how you read the next.
6. **Identify the consensus** — points where 3+ models independently agree.
   These have the most failure modes between them and the truth. Fold into
   distilled notes.
7. **Identify the single-source claims** — points only one model made.
   These are *not* automatically wrong but they haven't survived
   cross-reference. Either flag them in the distilled notes as
   "single-source, unverified" or hold them back until verified.
8. **Identify the disagreements** — points where models gave contradictory
   answers. These are the most interesting; one or more is wrong. Resolve
   against outside sources, not by picking the model you trust most.

## What not to do

- **Don't tell model B what model A said.** That collapses the consultation
  into a relay where model B is anchored on A's framing. The whole point is
  independent samples.
- **Don't soften the blunt-framing closer.** Polite questions get polite
  answers; the value is in the disagreement.
- **Don't treat any single LLM as the tiebreaker for any other LLM.** That
  ends in the trap from [[verify-against-silicon.md]] where every
  "verification" is just another LLM. The disagreement-resolution step has
  to go outside the LLM stack.
- **Don't skip saving the verbatim transcripts.** The distilled notes are
  the artefact for *use*, but the raw transcripts are the artefact for
  *audit*. When a later session questions a decision, "here's what each
  model actually said" is the only honest reply.

## When the technique fails

- **When all models share a common training bias.** All four agreeing on
  "use Linux source as the spec" is genuinely useful because Linux source
  is verifiable; all four agreeing on a piece of folklore that just
  happens to be in every model's training data is *not* evidence. The
  closer the topic to obscure-but-widely-Stack-Overflowed lore, the more
  this matters.
- **When the question is too abstract.** "How should I architect this?"
  gets four different essays; "should I put the status word at BAR+0x00
  or BAR+0x04, and why?" gets four comparable answers. The more concrete
  the question, the more useful the cross-reference.
- **When the answer requires fresh bit-level arithmetic.** As Copilot's
  MTRR example and Claude's purported correction of it demonstrated, LLMs
  are unreliable at this. The cross-reference catches the *existence* of
  a disagreement but not its *resolution* — that has to come from outside
  (see [[verify-against-silicon.md]]).

## Cost

Free-tier on each of the four models used here (ChatGPT, Grok, Copilot,
Gemini), so the marginal cost of a four-up consultation was zero. The
limiting resource was time-to-cross-reference, not money.

If a paid tier becomes the only path: the consultation is most valuable
*before* you've committed to an architectural direction, not after. Pay
for the one early consultation that catches the wrong direction, not for
the late one that ratifies what you've already built.
