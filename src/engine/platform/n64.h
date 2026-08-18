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

#ifndef _N64_H
#define _N64_H

/* THE NINTENDO 64 HAS NO SOUND CHIP, and that is the whole design of this file.
 *
 * Every other system here emulates hardware: a chip with registers, a fixed
 * number of voices and a timbre of its own. The N64 has a DMA engine and a DAC
 * (the AI), and everything between a sample and the speaker is SOFTWARE running
 * on the RSP. So what a composer is actually writing for is a mixer - and which
 * mixer decides what the machine can do.
 *
 * This models libdragon's, because that is what the games built with this
 * toolchain run: N voices, each playing 8- or 16-bit PCM at its own rate, with
 * a volume and a pan, summed into a stereo buffer at a fixed output rate. Those
 * are exactly the knobs `mixer_ch_play`, `mixer_ch_set_freq` and
 * `mixer_ch_set_vol_pan` give, and nothing else in the machine colours the
 * sound. A tracker that offered more would be lying about the target.
 *
 * WHAT THE CONSTRAINTS REALLY ARE, and they are the reason to compose here
 * rather than on a generic sample chip:
 *
 *   - VOICES COST CPU. Every voice is resampled and summed by the RSP, in a
 *     frame that is also drawing. 16 is libdragon's usual mixer size and is
 *     what this defaults to; the panel reports the modelled RSP load so that a
 *     32-voice arrangement is visibly a different decision.
 *   - SAMPLES COST ROM AND RDRAM. There is no wavetable and no FM: every note
 *     is a recording. The memory figure is the whole instrument set.
 *   - THE OUTPUT RATE IS A BUDGET. 32 kHz is what most N64 titles ran at;
 *     44.1 kHz costs a third more RSP time for the same music.
 */

#include "../dispatch.h"

class DivPlatformN64: public DivDispatch {
  struct Channel: public SharedChannel {
    /* 16.16 through the sample, exactly as the RSP walks it. */
    int audPos;
    int audSub;
    bool audDir;
    int sample;
    /* 0..255 each, the two halves libdragon's vol/pan resolves to. */
    int panL, panR;
    bool setPos;
    int macroVolMul;
    int macroPanMul;
    Channel(bool linear=true):
      SharedChannel(255,linear),
      audPos(0),
      audSub(0),
      audDir(false),
      sample(-1),
      panL(255),
      panR(255),
      setPos(false),
      macroVolMul(64),
      macroPanMul(127) {}
  };
  Channel* chan;
  bool* isMuted;
  int chans;
  DivDispatchOscBuffer* oscBuf;
  DivPitchTableManager samplePitchTable;

  /* 0 none, 1 linear. The RSP's resampler interpolates; "none" is here so a
     composer can hear what the cheap path costs them. */
  int interp;
  /* What the mixer's own headroom is. libdragon sums into 32 bits and clamps,
     and a 16-voice arrangement at full volume clips - which is a real N64
     mixing problem and has to be audible here or it will be found on hardware. */
  int volScale;
  bool outStereo;

  /* THE RSP COST, MODELLED. Not a measurement of this machine - it is what the
     console would spend to produce what you are hearing, so that "this is too
     many voices" is a number on a panel rather than a discovery on hardware. */
  float rspLoad;
  size_t sampleBytes;

  friend void putDispatchChip(void*,int);
  friend void putDispatchChan(void*,int,int);

  public:
    void acquire(short** buf, size_t len);
    int dispatch(DivCommand c);
    SharedChannel* getChanState(int chan);
    DivDispatchOscBuffer* getOscBuffer(int chan);
    void reset();
    void forceIns();
    void tick(bool sysTick=true);
    void muteChannel(int ch, bool mute);
    int getOutputCount();
    bool hasSoftPan(int ch);
    DivMacroInt* getChanMacroInt(int ch);
    unsigned short getPan(int chan);
    DivSamplePos getSamplePos(int ch);
    void setFlags(const DivConfig& flags);
    void notifyInsChange(int ins);
    void notifyInsDeletion(void* ins);
    void notifyPitchTable(int sample=-1);
    unsigned int getMaxFreq(int ch);
    int getPortaFloor(int ch);
    float getPostAmp();
    int init(DivEngine* parent, int channels, int sugRate, const DivConfig& flags);
    void quit();
    DivPlatformN64():
      chan(NULL), isMuted(NULL), chans(0), oscBuf(NULL) {}
};

#endif
