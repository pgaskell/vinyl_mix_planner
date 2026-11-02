# Use cases and modes

Design notes. Nothing here is built yet unless it says so. They set direction for features and for
the data model; TODO.md holds the concrete work items.

## Two kinds of DJ

The app serves two different kinds of DJ. They share almost all of the core, and differ in what
the app is for at the gig.

**The vinyl DJ.** The app is for preparation. The gig is played on real turntables: the app plans
it, rehearses it, and at most guides it.

**A new kind of digital DJing.**

- **How it differs:**
  - **From DVS and CDJs:** nobody beat matches or cues live. That work is done ahead, at the
    cuehead, minutes before the room hears it, possibly by a second person.
  - **From Ableton-style sets:** the arrangement isn't fixed in advance. It's built and changed
    during the set, and the mix itself is played by hand on a real mixer, not in software.
- **The performance is the mixer:** levels, EQ, FX, and the choice of what comes next.
- **The craft moves:** away from keeping records in time, toward selection, prepared
  transitions and the sound in the room.

| Feature | Vinyl DJ | Digital style |
|---|---|---|
| Library, analysis, grids, markers | Yes | Yes |
| Timeline, aligning, auditioning transitions | Yes | Yes |
| Transitions as objects, templates, branches, sub-mixes | Yes (planning) | Yes (live) |
| Following the hardware mixer, recording takes, bounce | Yes | Yes |
| SL-1200 limits (±8%, pitch shifts key, no time-stretch) | Required | A choice (see below) |
| Labels: record, side, 33/45, needle-drop points | Yes | No |
| Performance guide, following the records by ear | Yes | No |
| Practice mode (direct out, mix on the hardware) | Rehearsal | Rehearsal and the gig itself |
| Cuehead, editing while it plays, locking near the playhead | Optional | Core |
| Two operators on one laptop | No | Yes |
| Reliability at a gig: no dropouts, no lost set | Rehearsal only | Critical |

**Consequences:**

- **One app, not two.** The split is in modes and UI, not in the engine or the set format. A set
  planned for vinyl must play in the digital style unchanged.
- **The SL-1200 limits are the open decision.** For the vinyl DJ they're the point. For the
  digital style they're a style choice: pitch that shifts key is a sound, and ±8% keeps sets
  playable on vinyl. Recommendation: keep them as the default for both, and make deck limits a
  per-set setting (a "deck profile") rather than hard-coded. Add time-stretch, or wider pitch
  ranges, only if the digital style actually needs them. Time-stretch would be a second
  rendering path, and bounce must match live.
- **The digital style makes the app a live instrument.** At a gig, anything that can fail is in
  front of an audience: a dropout, a crash, a slow file read, an edit that lands under the
  playhead. These need a higher bar than planning does:
  - crash-safe autosave of the set;
  - edit locking near the playhead;
  - the glitch reporting that exists today;
  - no modal dialogs while playing.
- **What to build first.** The shared core (library, transitions as objects, recording) serves
  both. Building it first defers choosing between the two styles.

## 1. Set planning (primary)

Plan a set from audio files, constrained to what two (to four) SL-1200s and a real mixer can do.

- **Library.** Tracks with BPM, key and other metadata; searching and filtering for planning
  (BPM range, compatible key). Each track keeps its beat grid, corrections and **markers**
  (cue points: needle-drop points, breakdowns, the first kick, phrase starts) across sets.
- **Audition transitions.** Match tempo (pitch) and drop point, listen, adjust. This works now
  inside a set.
- **Transition templates.** Named, reusable moves: bass swap, filter sweep, fade in/out,
  echo out. Applying one writes automation for the two tracks over a chosen length.
- **A performance guide.** A live view that helps the DJ perform the planned mix on real
  turntables (see "Performance guide" below).

## 2. Practice or perform without beat matching

The app plays the set, already aligned, in direct out: each deck goes to its own mixer channel.
The DJ does everything else on the real mixer (EQ, faders, FX) and doesn't have to beat match or
drop the needle on time.

- This works today: direct out, the automation turned off, the DJM followed over MIDI.
- Missing: recording what the DJ did (mixer moves as automation, the mixer's USB return as
  audio) so takes can be compared and the best one kept.

## 3. Mixing ahead of what the crowd hears

While the set plays to the crowd, the upcoming part is prepared at the **cuehead**: a second
position on the timeline, independent of the playhead and usually ahead of it. It has its own
transport and plays out of a separate output, typically the laptop's built-in output into a
second pair of headphones.

- **Solo.** The DJ mixes at the playhead on the hardware mixer. Between transitions, they line up
  and audition the next ones at the cuehead.
- **Two DJs, one laptop.** One DJ works at the cuehead: picks tracks, beat matches, sets drop
  points, refines transitions. The other performs at the playhead and concentrates on levels,
  effects and what the room hears. This is most useful with 4 decks.
- **Sub-mixes.** Short runs of a few songs, with their transitions tested and refined. They can
  be reused, shared and strung together into a longer set. The set can still change if the
  crowd or the vibe calls for it.
- **Branches.** Tested transitions out of one song into several others. Pick the next song from
  the ones you've already worked out.

## Modes

| Mode | Playhead output | Cuehead output | Who mixes for the crowd | Stored automation at the playhead |
|---|---|---|---|---|
| Plan | Internal mix or direct out | Optional | Nobody (no crowd) | Plays |
| Practice | Direct out | Not used | DJ on the hardware | Off; the hardware is shown |
| Ahead (solo or two DJs) | Direct out, mixer to the PA | Laptop out, internal mix | DJ on the hardware | Off, or per control |
| Vinyl performance | Not used, or a reference | Optional | DJ on turntables and mixer | None (the guide shows it) |

The audience path is always the hardware mixer. The cuehead never touches it, so nothing done
at the cuehead can reach the room.

## Constraints and consequences

**The cuehead is a second engine, not a second deck set.**

- It renders the same `Schedule` at its own position, in internal-mix mode, to its own audio
  device. That keeps the rule that one code path does live, cue and bounce.
- Two devices means two clocks. That's fine here because nobody hears them together, so there's
  nothing to keep in sync.
- The prefetcher has to keep audio loaded around two positions. The pool gets split, or grows.
  Running out of audio at the cuehead must never starve the playhead: the playhead comes first.
- The cuehead's mixer is the app's own (on-screen controls and automation), not the hardware.
  So it only approximates the room's sound until the NXS2 emulation is done. That's acceptable
  for lining up and auditioning.

**Edits near the playhead.**

- The cuehead person edits the set while it plays. An edit close to the playhead changes audio
  that is already loaded, or about to be heard, and that the performer is reacting to.
- Lock a region just ahead of the playhead (say 10–20 s). Edits there are refused, or queued
  until it has passed.
- Edits made while the playhead is inside a transition shouldn't move anything the performer
  is already mixing.

**What the cuehead person changes reaches the performer.** In direct out, the performer gets
the alignment and the pitch. Whether they also get the automation is per control:

- Mixer automation off: the performer does the mix and the plan is only a suggestion. This is
  the default.
- Optionally, the app plays some moves itself (a planned filter sweep, say), while the performer
  keeps the faders.
- On the DJM, the app can't move the hardware. Planned moves for controls the performer owns
  belong in the performance guide, not the audio.

**A transition is a relationship between two tracks, not a timeline position.**

- To reuse one in another set (sub-mixes, branches), store it relative to the tracks:
  - the drop point and overlap in each track's source frames;
  - the incoming track's speed relative to the outgoing one (not absolute pitch);
  - its automation, anchored to source frames.
- If both tracks' speeds scale by the same factor, the alignment still holds. So one transition
  works at any absolute pitch, as long as it's within ±8% on both decks.
- The key moves with the pitch. A transition that was in key stays in key relative to the other
  track, but not in absolute terms.

**Automation is its own object, separate from clips.**

- **Clips keep their baseline:** the pitch setting and the mixer state at the needle drop.
- **Transitions own the moves:** fades, EQ swaps and pitch rides between two tracks. Each move
  is still anchored to the source frames of the clip it acts on, so it follows the music when
  the clip is trimmed or moved.
- **Effects:**
  - Deleting a transition removes its moves and leaves both clips intact.
  - A clip can take part in two transitions (in from the previous track, out to the next) with
    no ambiguity about which move belongs where.
  - A transition can be copied to another set, or saved as a template.
- **Rules needed:**
  - Two transitions can't drive the same control on the same clip over overlapping time. Refuse
    it, or let the later one win, but never blend them silently.
  - Moves outside any transition (a mid-track EQ ride) need an owner too. Either a clip-level
    lane (what exists today) or a "solo" transition with one clip.
  - The current per-clip automation migrates into transitions. That's a set-format change and a
    backwards-compatibility shim for TODO.md.

**Templates are written in beats and bars of the outgoing track.**

- There's no global grid, so a template like "bass swap over 8 bars at the phrase" resolves
  through the outgoing track's beat grid (and the bar resets in TODO.md) into source frames.
- Templates should stay performable: no more simultaneous moves than two hands can make.
  Otherwise the plan can't be done live.

**Sharing sub-mixes means sharing plans, not audio.**

- Other people have different files and paths, and the audio is copyrighted. Tracks need an
  identity that survives that: an audio fingerprint, or metadata plus duration, with relinking
  when a file isn't found.
- The same matching fixes moved files in your own library.
- Another person's copy of a record may be a different pressing or rip, with a slightly
  different offset. Matching can't be exact, so offsets should be re-checked against the grid.

## Performance guide (the dynamic cue sheet)

A live view for performing the planned set on real turntables. It tells the DJ what is coming
and when: which record and side go next, and on which deck, with countdowns to the actions:

- set the pitch slider to this value;
- start beat matching now;
- drop the needle at this marker;
- let go on bar 33, beat 1;
- start the bass swap, which lasts 8 bars;
- the fader comes out.

**Where the real records are is the hard part, because there's no timecode.**

1. **Manual sync.** The DJ taps a key at an audible landmark ("first kick") on each record and
   the guide runs from there. Simple, but it drifts with the DJ's real pitch.
2. **Listening to the records.** On the 900SRT, the DJM Setup utility can send each channel's
   raw phono input over USB, before the fader (the "Control Tone PHONO" option seen on USB
   5–8). The app can match that audio against the decoded track to find where the record is and
   how fast it is running. Then the guide can say "deck 2 is 0.3% fast" or "you're half a beat
   early", and the countdowns follow the real record.
   - This needs the inputs (microphone permission is declared) and a robust matcher. A vinyl
     rip and the actual pressing differ in level, EQ, noise and wow.
   - Worth a prototype before committing to it.
   - The NXS2 likely has a similar option; check its setup utility.

Option 2 also gives practice mode a way to score a real-vinyl run against the plan.

## Concepts for the data model

- **Track (library):** file identity, analysis, the DJ's corrections, markers and start points.
  Shared by every set.
- **Clip:** a track placed on a lane, with its baseline pitch and mixer state.
- **Transition:** outgoing clip → incoming clip: alignment, relative speed and its automation.
  Reusable outside the set as a track-pair object.
- **Segment (sub-mix):** an ordered run of tracks joined by transitions.
- **Set:** clips on lanes, plus transitions. It could be built from segments, and still edited
  freely afterwards.
- **Transition graph:** tracks are nodes and tested transitions are edges. Branching and "what
  can I play next" are queries on it.
- **Take:** a recorded performance (mixer automation and/or USB return audio) against a set,
  for comparing and choosing.
- **Performance guide:** generated from a set and its transitions; never edited directly.

## Suggested order

1. Library with per-track memory (grid, key, markers, start points). Already first in TODO.md;
   everything else depends on it.
2. Automation as transition objects, separate from clips. Do this before more automation
   features are built on the per-clip model.
3. Record mixer moves as takes (phase 3). This completes use case 2.
4. The cuehead: a second engine and output, locking edits near the playhead, prefetching two
   positions. This enables use case 3.
5. Transition templates written in bars; reusing transitions across sets.
6. The performance guide with manual sync, then a prototype of following the records by ear.
7. Segments, sharing and fingerprint matching.

## Open questions

- **Automation at the cuehead:** while lining up the next transition, does the cuehead person
  want to hear the planned moves (internal mix with automation), or just the raw overlap?
  Probably a toggle.
- **The cuehead's position:** should it move when the playhead reaches it? It could follow the
  playhead or keep its own position, and this matters during long sets.
- **Automation the app plays live:** which planned moves should it play in front of a crowd, if
  any (FX on a control the performer doesn't own)? Or is the hardware always fully manual?
- **Performance guide:** what should it look like? Bar counts, time, or the waveform with
  markers? On the laptop screen, or a second display?
