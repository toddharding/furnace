/**
 * Furnace Tracker - multi-system chiptune tracker
 * Copyright (C) 2021-2026 tildearrow and contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef _SOUNDFONT_H
#define _SOUNDFONT_H

/* SOUNDFONTS, BECAUSE A SAMPLE MACHINE NEEDS SAMPLES.
 *
 * The N64 has no oscillator: every note is a recording, so the instrument set
 * IS the timbre and a tracker aimed at it is only as good as the sounds you can
 * get into it. Furnace could load a WAV, which means a composer starts by going
 * to find 40 WAVs - and that is the step where most people stop.
 *
 * An .sf2 is 40 instruments in one file, keyed by General MIDI program, with
 * the loop points and the root key already in it. GeneralUser GS is the one
 * most people have. So this reads the format's index - which preset is which
 * program, which instrument it plays, which sample that is, where it loops and
 * what key it was recorded at - and hands one preset over as a DivSample that
 * is already in tune.
 *
 * WHAT THIS DELIBERATELY DOES NOT DO: modulators, envelopes, filters, velocity
 * layers, and the rest of the SF2 synthesis model. None of them exist on the
 * target - the RSP plays a sample at a rate with a volume and a pan, and
 * nothing else - so importing them would be importing a sound the console
 * cannot make. What comes across is the PCM and the tuning, which is all the
 * machine can use.
 */

#include "../ta-utils.h"
#include <vector>

struct DivSoundFontPreset {
  String name;
  int bank, program;
  /* How many key zones the preset has, which is a rough measure of how much of
     it a single sample is losing. A piano sampled at eight pitches imported as
     one sample is a piano stretched over eight octaves, and a composer has to
     be able to see that rather than hear it later. */
  int zones;
};

struct DivSoundFontSample {
  String name;
  std::vector<short> data;      /* 16-bit mono, as the file stores it */
  unsigned int rate;
  int loopStart, loopEnd;       /* -1 when the sample does not loop */
  int rootKey;                  /* the MIDI key it was recorded at */
  int fineTune;                 /* cents */
};

class DivSoundFont {
  public:
    /* Read the index. Returns false and fills `error` on anything malformed -
       a soundfont that half-loads is a set of instruments that are quietly
       the wrong ones. */
    bool open(const char* path);
    const std::vector<DivSoundFontPreset>& getPresets() const { return presets; }
    /* One preset's sample, at `key` (a MIDI note - which zone of a multi-
       sampled instrument to take). Returns false if the preset has no PCM at
       that key. */
    bool loadPreset(int index, int key, DivSoundFontSample& out);
    const String& getError() const { return error; }
    const String& getName() const { return fontName; }

  private:
    struct Zone { int lowKey, highKey, sampleID, rootKeyOverride, instrument,
                  fineTune, coarseTune; };
    struct Inst { String name; std::vector<Zone> zones; };
    struct Shdr { String name; unsigned int start, end, loopStart, loopEnd,
                  rate; int rootKey, correction; unsigned short type; };

    String path, error, fontName;
    std::vector<DivSoundFontPreset> presets;
    std::vector<std::vector<Zone> > presetZones;   /* per preset */
    std::vector<Inst> insts;
    std::vector<Shdr> shdrs;
    /* Where the PCM lives in the file, so a 30 MB soundfont is not held in
       memory to import one flute. */
    long smplOffset;
    unsigned int smplLen;
};

#endif
