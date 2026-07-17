# Per-chip gotchas (all learned empirically — trust these over intuition)

## Volume column ranges (writing 127 to an OPL channel ≠ loud, it clamps)

| Chip | vol range |
|---|---|
| YM2612 (Genesis FM) | 0-127 |
| SN76489 (Genesis PSG) | 0-15 |
| OPL2/OPL3 | 0-63 |
| YM2151 (OPM) | 0-127 |
| SegaPCM / sample chips | 0-127 |

## YM2612 + SN76489 (Genesis)

- Channels: 0-5 FM (5 doubles as DAC when a Sample-type instrument plays on
  it), 6-8 PSG squares, 9 PSG noise.
- **SN noise channel accepts ONLY tracked notes C / C# / D** (3 preset rates;
  D = highest/crispest). Any other note silently switches to tone3-shared
  mode: square 3 gets programmed audibly (a constant whine at a pitch that
  follows your "hat" note) and the noise turns periodic/buzzy. Force preset
  white noise with effect `20 01` and/or a duty macro value 1.
- DAC drums: Sample-type instrument (type 4) + `amiga.initSample`; note C-4
  plays at natural rate.

## OPL3 (YMF262)

- 18 channels, but **4-op library patches are a trap**: check
  `instrument.fm.ops` after EVERY import. A 4-op patch consumes a hardware
  channel pair — on the wrong channel it silently kills a NEIGHBOR (observed:
  ch6 4-op killed ch7) or is itself silent. The safe placements are
  empirical, not documented (a Furnace allocation quirk): place 4-op voices
  one at a time and verify stems after each; when a channel stays dead, use a
  2-op patch or move to the high singles (15-17). Different 4-op patches can
  behave differently on the same channel.
- Waveforms 4-7 need OPL3 mode (fine in Furnace's OPL3, absent on OPL2).
- Build dev binaries `-DCONSOLE_SUBSYSTEM=ON` or there is no stdout at all.

## YM2151 (OPM) + SegaPCM

- OPM: 8 channels, ALL native 4-op — no pairing traps. SegaPCM: 16 sample
  channels after the OPM block (channel 8+); Sample-type instruments work.
- **OPM library .dmp patches vary >10x in baked-in loudness** ("trashy
  guitar" inaudible at 78, harsh at 127; "Organ 2 (Percussive)" healthy at
  106). Level-audition before committing (render candidates at equal volume,
  compare stem peaks).
- PCM drums overpower OPM FM by ~10:1 at equal note volumes — balance with
  chip mixer volumes (e.g. FM 1.0 / PCM 0.35), then master ~1.5.

## SNES (S-SMP/DSP)

- 8 sample channels; instruments are type 29 with `amiga.useSample` +
  `amiga.initSample` and the `snes` ADSR block (`a` 0-15, `d` 0-7, `s` 0-7,
  `r` = sustain rate, 0 holds forever until key-off).
- **The DSP voice sum clips internally** (16-bit accumulator, hardware-
  authentic saturation) and the mixer master volume is applied AFTER that
  clip — turning master down just scales the distortion. Keep voice volumes
  around HALF the range (peaks ~55/127, pads ~30) so the 8-voice sum stays
  inside headroom; verify with a flat-top count on the rendered wave (samples
  pinned at ±peak), not just the peak value.
- Synthesized single-cycle loops work great (BRR-friendly: loop length a
  multiple of 16, exact integer periods loop clean without crossfade; e.g.
  64-frame cycle at 16 kHz, `centerRate` 16744 puts C-4 at 261.6 Hz).
- **`snes.r` (sustain rate) 0 = hold forever = "drony"** — looped-sample
  voices sustain at level `s` until the next note. Set `r>0` for a natural
  fade after sustain (measured: r=12 on a pad/choir ≈ −12 dB ~2 s after the
  note; r=16 on a decaying bell ≈ −38 dB at 1 s). Long held notes under a
  written crescendo then fade mid-swell — re-key the note halfway (reads as
  a singer re-breathing). Verify decay with a solo render + 100 ms RMS
  windows, not by eyeballing the envelope numbers.
- Echo is the character of the chip: `18 01` enable buffer, `19 xx` delay
  (xx*16 ms of RAM!), `1C` feedback, `1A/1B` L/R echo volume, `30-37` an
  8-tap FIR (a decaying tap series = dark lowpassed cavern repeats), and
  `12 01` per channel to opt in. Echo RAM comes out of the 64 KB sample
  budget — size samples accordingly.
- Hardware noise per channel: `11 01` toggle + `1D xx` frequency (0-1F).

## C64 (SID 6581/8580)

- 3 channels; instruments are type 3 and the feature block is keyed `"64"`
  (not `"c64"`): `waveforms` {tri,saw,pulse,noise}, `duty` 0-4095,
  `envelope` {attack,decay,sustain,release} all 0-15, `filter`
  {init,lowPass/bandPass/highPass,cutoff 0-2047,resonance,to,isAbsolute}.
- **The volume column IS per-channel in Furnace** (measured: vol 8 ≈ 0.49x
  the RMS of vol 15) — the real SID's global-volume-register limitation does
  not constrain authoring here. Range 0-15.
- **Triangle is ~40% the amplitude of pulse** at equal settings; noise is
  quieter still. Drum bodies want pulse (wave macro e.g. `[8,4,4,4]` =
  noise crack then square thump), not triangle, or they vanish under a
  pulse bass. Drums = wave macro (code 3, waveform bitmask: 1 tri, 2 saw,
  4 pulse, 8 noise) + relative arp macro for the pitch drop
  (`[24,-2,-6,...]` works; measured kick 233→89 Hz) + ADSR decaying to
  sustain 0 (self-terminating: droning impossible by construction).
- **One filter, whole chip.** An instrument with `filter.init:true` + `to`
  owns/re-inits it on every key-on, so only ONE part at a time should claim
  the filter (bass in the groove, pad in a bridge). Sweep it during a held
  note with `24xx`/`25xx` cutoff slides (init only fires on key-on); cancel
  with `2400` before the next section. Absolute cutoff is `4xxx` (0-7FF,
  code byte 0x40|hi, value lo); pulse width is `3xxx` likewise.
- Arps: `00xy` + `E0xx` arp speed (1 = 60 Hz shimmer). Live ADSR rewrites:
  `20xy` attack/decay, `21xy` sustain/release.
- Envelope lifecycle: sustain holds forever while gated — SID has no
  hardware fade-after-sustain, so phrase parts need note-OFF hygiene plus a
  release value that matches the tail you want (r=3 bass ≈ tight, r=5 lead
  ≈ short sing-off, r=8 pad wash). Percussion: decay to sustain 0.

## Instrument JSON contract (all chips)

- Keys must match `saveJSON` exactly: `fm.operators` (array; `fm.ops` is the
  scalar count), `gb.envVol/envDir/envLen/soundLen`, macro code 2 = duty/
  noise-mode, 3 = wave. Unknown keys are rejected with dotted paths — good;
  read the error.
- **Macro objects require `"length"`** or they silently never apply (decay
  macros absent = every PSG note sustains forever = "hiss"/wash).
- `get_instrument` returns `{index, instrument}` — unwrap.
- Imported `.dmp` can sound an octave off written pitch (verify stem
  fundamentals vs score; transpose the written notes, not the patch).

## Sequencer semantics

- `order_ops add` inserts AFTER the current order, not at the end
  (`deep_clone_end` appends). After structural surgery, `read_orders` and
  verify the matrix; safest is `write_orders` with an explicit full matrix.
- `write_pattern` writes ONLY the rows given — replacing a phrase means
  clearing the old rows explicitly first (leftover notes bleed through).
- Effects persist until changed (vibrato, panning, porta): reset with
  `04 00` / `01 00` etc. when a phrase ends.
- `0Bxx` at the last row jumps to order xx (loop point).
