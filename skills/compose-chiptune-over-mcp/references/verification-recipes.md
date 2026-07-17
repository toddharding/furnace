# Verification recipes (python, numpy + PIL; no scipy/matplotlib needed)

All against a HEADLESS `furnace --mcp-tcp 127.0.0.1:0 --loglevel info`
(scrape `furnace-mcp ready <host:port>`, newline-delimited JSON-RPC;
`tools/call` result payload is `result.content[0].text`, JSON-parse it,
`isError` means tool failure). Never let a review instance `play` — offline
`render_wav` only, or it plays through the user's speakers.

## Stem table (liveness + balance)

`render_wav {mode:"per_channel", loops:1}` → `stem_cNN.wav` is channel NN-1.
Per stem: `peak = |s16|.max()/32768`; silence% = fraction of 50ms windows with
RMS < 0.002. Expectations: every scored channel peaks > 0.003; silence%
should roughly match how much of the arrangement the part occupies (a "hat"
at 4% silence is a drone; a lead at 100% is dead).
CAUTION: per-channel mode renders the whole song once per channel — for a
3-minute 24-channel song that exceeds render timeouts. Fall back to
master render + targeted solo renders (mute all but one channel, mode "one").

## Spectrogram as an image (your eyes are the ear substitute)

FFT frames (nfft 4096, hop = len/width), keep bins <= fmax (8k full-band /
1.2k bass zoom), 20*log10 magnitude clipped to a 70dB window, flip so low
freq is at the bottom, colorize via LUT, save PNG, then READ the image.
What to look for: an unbroken horizontal line = constant tone/whine (find the
channel, then the mechanism); harmonic comb where noise should be = periodic
noise mode; vertical stripes with gaps = healthy percussion; straight-then-
wavy melodic bands = delayed vibrato working; dense low harmonic stacks =
octave-too-low bass.

## Pitch truth

FFT the section of interest, take spectral peaks 30-2000 Hz, convert to note
names + cents (midi = 69 + 12*log2(f/440)). Compare against the intended
chords/score from your own composition data or `song_json`. Catches DMP
octave offsets and wrong-scale harmonies numerically.

## Dynamics + drone timelines

- Arc: RMS per 2s window over the master; assert min > 0.005 (dead air),
  peak < 0.95 (clipping), and that breakdown windows dip vs chorus windows.
- Drone check for channel N: solo it (mutes) → render master-mode → RMS per
  2s → longest continuous run of windows > 0.002 must not exceed the longest
  scored phrase (a sustained-forever note shows as a run spanning sections).

## Live-state probes (when a render disagrees with the score)

- `get_channel_states` (engine keyed? volume? ins?) vs
  `get_channel_oscilloscope` (does the chip emit?) vs `get_registers` (what
  was actually written to hardware) — the three disagree exactly at the layer
  containing the bug.
- Jam vs pattern: `note_on` same channel/ins/note/vol — if jamming sounds
  where playback doesn't, the bug lives in pattern/effect/order state.
- Death timeline: write probe notes on the suspect channel in every pattern,
  `play {order:0}`, poll position + channel osc every ~0.9s; the (order,row)
  where it dies names the killer (e.g. "died the row the 4-op echo entered").

## Level audition (patch selection without ears)

Scratch song, one candidate per channel, identical note/volume/gating,
per_channel render, rank stem peaks. Pick the patch whose natural level sits
mid-range (headroom both ways); the user judges character afterwards.
