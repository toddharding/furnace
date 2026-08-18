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

#include "n64.h"
#include "../engine.h"
#include <math.h>

/* The frequency register is an 8.16 counter against the output rate - the same
   shape libdragon's mixer uses, where a channel's step is its own sample rate
   over the mixer's. */
#define CHIP_FREQBASE 65536

/* WHAT ONE VOICE COSTS THE RSP, PER OUTPUT SAMPLE.
 *
 * Not measured on this machine - it is a model of the console, which is the
 * only figure worth putting in front of a composer. libdragon's mixer walks a
 * voice with a 16.16 accumulator, fetches, interpolates and accumulates into a
 * 32-bit stereo bus; on the RSP at 62.5 MHz that comes out at roughly 20 cycles
 * a voice a sample once the vector unit is doing eight lanes at a time. The
 * number below is what the panel divides by the frame to get a percentage, and
 * it is deliberately a single named constant rather than a spread of magic
 * arithmetic: if somebody measures the real thing, there is one place to put
 * it. */
#define RSP_CYCLES_PER_VOICE_SAMPLE 20.0
#define RSP_CLOCK 62500000.0

void DivPlatformN64::acquire(short** buf, size_t len) {
  int outSum[2];

  if (chan==NULL) {
    for (size_t h=0; h<len; h++) {
      buf[0][h]=0;
      buf[1][h]=0;
    }
    return;
  }

  for (int i=0; i<chans; i++) {
    oscBuf[i].begin(len);
  }

  int voicesPlaying=0;
  for (int i=0; i<chans; i++) {
    if (chan[i].active && chan[i].sample>=0) voicesPlaying++;
  }

  for (size_t h=0; h<len; h++) {
    outSum[0]=0;
    outSum[1]=0;

    for (int i=0; i<chans; i++) {
      int output=0;
      if (!chan[i].active || chan[i].sample<0 ||
          chan[i].sample>=parent->song.sampleLen) {
        oscBuf[i].putSample(h,0);
        continue;
      }
      DivSample* s=parent->getSample(chan[i].sample);
      if (s->samples==0) {
        chan[i].sample=-1;
        oscBuf[i].putSample(h,0);
        continue;
      }

      /* THE WALK, exactly as the RSP does it: a 16.16 position stepped by the
         channel's frequency, with the loop applied at the end rather than
         clamped at the fetch. */
      chan[i].audSub+=chan[i].freq;
      while (chan[i].audSub>=0x10000) {
        chan[i].audSub-=0x10000;
        chan[i].audPos+=chan[i].audDir?-1:1;
        if (chan[i].audDir) {
          if (s->isLoopable() && chan[i].audPos<s->loopStart) {
            switch (s->loopMode) {
              case DIV_SAMPLE_LOOP_PINGPONG:
                chan[i].audPos=s->loopStart+(s->loopStart-chan[i].audPos);
                chan[i].audDir=false;
                break;
              default:
                chan[i].audPos=s->loopEnd-1-(s->loopStart-chan[i].audPos);
                break;
            }
          } else if (chan[i].audPos<0) {
            chan[i].sample=-1;
          }
        } else if (chan[i].audPos>=(int)s->samples ||
                   (s->isLoopable() && chan[i].audPos>=s->loopEnd)) {
          if (s->isLoopable()) {
            switch (s->loopMode) {
              case DIV_SAMPLE_LOOP_BACKWARD:
              case DIV_SAMPLE_LOOP_PINGPONG:
                chan[i].audPos=s->loopEnd-1-(chan[i].audPos-(s->loopEnd-1));
                chan[i].audDir=true;
                break;
              default:
                chan[i].audPos=(chan[i].audPos+s->loopStart)-s->loopEnd;
                break;
            }
          } else {
            /* THE VOICE STOPS. libdragon's mixer drops a one-shot channel
               when it runs off the end rather than holding the last sample,
               and holding it would be a DC step on the bus. */
            chan[i].sample=-1;
            chan[i].active=false;
          }
        }
        if (chan[i].sample<0) break;
      }
      if (chan[i].sample<0) {
        oscBuf[i].putSample(h,0);
        continue;
      }

      const int pos=chan[i].audPos;
      const short here=(pos>=0 && pos<(int)s->samples)?s->data16[pos]:0;
      if (interp) {
        int nextPos=pos+1;
        if (nextPos>=(int)s->samples) {
          nextPos=s->isLoopable()?s->loopStart:pos;
        }
        const short next=(nextPos>=0 && nextPos<(int)s->samples)
                             ?s->data16[nextPos]:0;
        output=here+(((int)next-(int)here)*(chan[i].audSub&0xffff)>>16);
      } else {
        output=here;
      }

      if (isMuted[i]) {
        output=0;
      } else {
        output=(output*chan[i].outVol)/255;
      }
      oscBuf[i].putSample(h,output>>1);
      /* PAN IS TWO GAINS, which is what mixer_ch_set_vol_pan resolves to on
         the console - not a stereo image, just how much of the voice each
         side of the bus receives. */
      outSum[0]+=(output*chan[i].panL)>>8;
      outSum[1]+=(output*chan[i].panR)>>8;
    }

    /* THE MIXER'S OWN HEADROOM, and it clips. Sixteen voices at full volume
       summed into libdragon's 32-bit bus overflow the DAC's 16 bits, and that
       is a real N64 mixing problem: hearing it here is the point, because the
       alternative is finding it on hardware. `volScale` is the headroom a
       composer chooses; the clamp below is the DAC. */
    outSum[0]=(outSum[0]*volScale)>>8;
    outSum[1]=(outSum[1]*volScale)>>8;
    if (!outStereo) {
      const int mono=(outSum[0]+outSum[1])>>1;
      outSum[0]=mono;
      outSum[1]=mono;
    }
    if (outSum[0]<-32768) outSum[0]=-32768;
    if (outSum[0]>32767) outSum[0]=32767;
    if (outSum[1]<-32768) outSum[1]=-32768;
    if (outSum[1]>32767) outSum[1]=32767;

    buf[0][h]=outSum[0];
    buf[1][h]=outSum[1];
  }

  for (int i=0; i<chans; i++) {
    oscBuf[i].end(len);
  }

  /* What the console would be spending on what was just heard. */
  rspLoad=(float)((double)voicesPlaying*(double)rate*
                  RSP_CYCLES_PER_VOICE_SAMPLE/RSP_CLOCK);
}

void DivPlatformN64::tick(bool sysTick) {
  (void)sysTick;
  size_t bytes=0;
  for (int i=0; i<parent->song.sampleLen; i++) {
    DivSample* s=parent->song.sample[i];
    if (s!=NULL) bytes+=(size_t)s->samples*2;
  }
  sampleBytes=bytes;

  for (int i=0; i<chans; i++) {
    chan[i].std.next();
    if (chan[i].std.vol.had) {
      chan[i].outVol=(chan[i].vol*MIN(chan[i].macroVolMul,chan[i].std.vol.val))/
                     chan[i].macroVolMul;
    }
    if (NEW_ARP_STRAT) {
      chan[i].handleArp();
    } else if (chan[i].std.arp.had && !chan[i].rawFreq) {
      if (!chan[i].inPorta) {
        chan[i].baseFreq=chan[i].calcBaseFreq(
            parent->calcArp(chan[i].note,chan[i].std.arp.val));
      }
      chan[i].freqChanged=true;
    }
    if (chan[i].std.pitch.had) {
      if (chan[i].std.pitch.mode) {
        chan[i].pitch2+=chan[i].std.pitch.val;
        CLAMP_VAR(chan[i].pitch2,-32768,32767);
      } else {
        chan[i].pitch2=chan[i].std.pitch.val;
      }
      chan[i].freqChanged=true;
    }
    if (chan[i].std.panL.had) {
      chan[i].panL=(255*(chan[i].std.panL.val&chan[i].macroPanMul))/
                   chan[i].macroPanMul;
    }
    if (chan[i].std.panR.had) {
      chan[i].panR=(255*(chan[i].std.panR.val&chan[i].macroPanMul))/
                   chan[i].macroPanMul;
    }
    if (chan[i].std.phaseReset.had) {
      if (chan[i].std.phaseReset.val==1) {
        chan[i].audDir=false;
        chan[i].audPos=0;
        chan[i].audSub=0;
      }
    }
    if (chan[i].freqChanged || chan[i].keyOn || chan[i].keyOff) {
      chan[i].freq=chan[i].calcFreq();
      if (chan[i].freq>0xffffff) chan[i].freq=0xffffff;
      if (chan[i].freq<0) chan[i].freq=0;
      if (chan[i].keyOn) {
        if (!chan[i].std.vol.had) chan[i].outVol=chan[i].vol;
        chan[i].keyOn=false;
      }
      if (chan[i].keyOff) chan[i].keyOff=false;
      chan[i].freqChanged=false;
    }
  }
}

int DivPlatformN64::dispatch(DivCommand c) {
  if (c.chan>=chans) return 0;
  switch (c.cmd) {
    case DIV_CMD_NOTE_ON: {
      /* THE INSTRUMENT IS A SAMPLE AND ONLY A SAMPLE. There is no wavetable
         branch here, and its absence is the machine: the N64 has no oscillator
         of any kind, so every note is a recording and the instrument set is
         the whole of the timbre. */
      DivInstrument* ins=parent->getIns(chan[c.chan].ins,DIV_INS_AMIGA);
      chan[c.chan].macroVolMul=ins->type==DIV_INS_AMIGA?64:255;
      chan[c.chan].macroPanMul=ins->type==DIV_INS_AMIGA?127:255;
      if (c.value!=DIV_NOTE_NULL) {
        chan[c.chan].sample=ins->amiga.getSample(c.value);
        chan[c.chan].pitchTable=samplePitchTable.get(chan[c.chan].sample);
        chan[c.chan].sampleNote=c.value;
        c.value=ins->amiga.getFreq(c.value);
        chan[c.chan].sampleNoteDelta=c.value-chan[c.chan].sampleNote;
      } else if (chan[c.chan].sampleNote!=DIV_NOTE_NULL) {
        chan[c.chan].sample=ins->amiga.getSample(chan[c.chan].sampleNote);
        chan[c.chan].pitchTable=samplePitchTable.get(chan[c.chan].sample);
        c.value=ins->amiga.getFreq(chan[c.chan].sampleNote);
      }
      if (c.value!=DIV_NOTE_NULL) {
        chan[c.chan].baseFreq=chan[c.chan].calcBaseFreq(c.value);
      }
      if (chan[c.chan].sample<0 ||
          chan[c.chan].sample>=parent->song.sampleLen) {
        chan[c.chan].sample=-1;
        chan[c.chan].sampleNote=DIV_NOTE_NULL;
        chan[c.chan].sampleNoteDelta=0;
      }
      if (chan[c.chan].setPos) {
        chan[c.chan].setPos=false;
      } else {
        chan[c.chan].audDir=false;
        chan[c.chan].audPos=0;
      }
      chan[c.chan].audSub=0;
      if (c.value!=DIV_NOTE_NULL) {
        chan[c.chan].freqChanged=true;
        chan[c.chan].note=c.value;
      }
      chan[c.chan].active=true;
      chan[c.chan].keyOn=true;
      chan[c.chan].macroInit(ins);
      if (!parent->song.compatFlags.brokenOutVol && !chan[c.chan].std.vol.will) {
        chan[c.chan].outVol=chan[c.chan].vol;
      }
      chan[c.chan].insChanged=false;
      break;
    }
    case DIV_CMD_NOTE_OFF:
      chan[c.chan].sample=-1;
      chan[c.chan].active=false;
      chan[c.chan].keyOff=true;
      chan[c.chan].macroInit(NULL);
      break;
    case DIV_CMD_NOTE_OFF_ENV:
    case DIV_CMD_ENV_RELEASE:
      chan[c.chan].std.release();
      break;
    case DIV_CMD_INSTRUMENT:
      if (chan[c.chan].ins!=c.value || c.value2==1) {
        chan[c.chan].ins=c.value;
        chan[c.chan].insChanged=true;
      }
      break;
    case DIV_CMD_VOLUME:
      if (chan[c.chan].vol!=c.value) {
        chan[c.chan].vol=c.value;
        if (!chan[c.chan].std.vol.has) chan[c.chan].outVol=c.value;
      }
      break;
    case DIV_CMD_GET_VOLUME:
      return chan[c.chan].vol;
    case DIV_CMD_PANNING:
      chan[c.chan].panL=c.value;
      chan[c.chan].panR=c.value2;
      break;
    case DIV_CMD_PITCH:
      chan[c.chan].pitch=c.value;
      chan[c.chan].freqChanged=true;
      break;
    case DIV_CMD_NOTE_PORTA: {
      int destFreq=chan[c.chan].calcBaseFreq(
          c.value2+chan[c.chan].sampleNoteDelta);
      bool return2=false;
      if (destFreq>chan[c.chan].baseFreq) {
        chan[c.chan].baseFreq+=c.value;
        if (chan[c.chan].baseFreq>=destFreq) {
          chan[c.chan].baseFreq=destFreq;
          return2=true;
        }
      } else {
        chan[c.chan].baseFreq-=c.value;
        if (chan[c.chan].baseFreq<=destFreq) {
          chan[c.chan].baseFreq=destFreq;
          return2=true;
        }
      }
      chan[c.chan].freqChanged=true;
      if (return2) {
        chan[c.chan].inPorta=false;
        return 2;
      }
      break;
    }
    case DIV_CMD_LEGATO:
      chan[c.chan].baseFreq=chan[c.chan].calcBaseFreq(
          c.value+chan[c.chan].sampleNoteDelta+
          ((HACKY_LEGATO_MESS)?(chan[c.chan].std.arp.val):(0)));
      chan[c.chan].freqChanged=true;
      chan[c.chan].note=c.value;
      break;
    case DIV_CMD_PRE_PORTA:
      if (chan[c.chan].active && c.value2) {
        if (parent->song.compatFlags.resetMacroOnPorta) {
          chan[c.chan].macroInit(parent->getIns(chan[c.chan].ins,DIV_INS_AMIGA));
        }
      }
      chan[c.chan].inPorta=c.value;
      break;
    case DIV_CMD_SAMPLE_POS:
      chan[c.chan].audPos=c.value;
      chan[c.chan].setPos=true;
      break;
    case DIV_CMD_GET_VOLMAX:
      return 255;
    case DIV_CMD_MACRO_OFF:
      chan[c.chan].std.mask(c.value,true);
      break;
    case DIV_CMD_MACRO_ON:
      chan[c.chan].std.mask(c.value,false);
      break;
    case DIV_CMD_MACRO_RESTART:
      chan[c.chan].std.restart(c.value);
      break;
    default:
      break;
  }
  return 1;
}

void DivPlatformN64::muteChannel(int ch, bool mute) {
  if (ch>=chans) return;
  isMuted[ch]=mute;
}

void DivPlatformN64::forceIns() {
  for (int i=0; i<chans; i++) {
    chan[i].insChanged=true;
    chan[i].freqChanged=true;
    chan[i].audDir=false;
    chan[i].audPos=0;
    chan[i].sample=-1;
  }
}

SharedChannel* DivPlatformN64::getChanState(int ch) {
  if (ch>=chans) return NULL;
  return &chan[ch];
}

DivDispatchOscBuffer* DivPlatformN64::getOscBuffer(int ch) {
  if (ch>=chans) return NULL;
  return &oscBuf[ch];
}

void DivPlatformN64::reset() {
  for (int i=0; i<chans; i++) {
    chan[i]=DivPlatformN64::Channel(parent->song.compatFlags.linearPitch);
    chan[i].pitchTable=samplePitchTable.get(-1);
    chan[i].std.setEngine(parent);
  }
  rspLoad=0.0f;
}

int DivPlatformN64::getOutputCount() {
  return 2;
}

bool DivPlatformN64::hasSoftPan(int ch) {
  (void)ch;
  return outStereo;
}

DivMacroInt* DivPlatformN64::getChanMacroInt(int ch) {
  if (ch>=chans) return NULL;
  return &chan[ch].std;
}

unsigned short DivPlatformN64::getPan(int ch) {
  if (ch>=chans) return 0;
  return (chan[ch].panL<<8)|chan[ch].panR;
}

DivSamplePos DivPlatformN64::getSamplePos(int ch) {
  if (ch>=chans) return DivSamplePos();
  return DivSamplePos(chan[ch].sample,chan[ch].audPos,chan[ch].freq);
}

void DivPlatformN64::notifyInsChange(int ins) {
  for (int i=0; i<chans; i++) {
    if (chan[i].ins==ins) chan[i].insChanged=true;
  }
}

void DivPlatformN64::notifyInsDeletion(void* ins) {
  for (int i=0; i<chans; i++) {
    chan[i].std.notifyInsDeletion((DivInstrument*)ins);
  }
}

void DivPlatformN64::notifyPitchTable(int sample) {
  samplePitchTable.update<Channel>(chan,chans,parent->song.tuning,chipClock,
                                   CHIP_FREQBASE,0xffffff,false,
                                   parent->song.compatFlags.linearPitch,sample);
}

unsigned int DivPlatformN64::getMaxFreq(int ch) {
  (void)ch;
  return 0xffffff;
}

int DivPlatformN64::getPortaFloor(int ch) {
  (void)ch;
  return 0;
}

float DivPlatformN64::getPostAmp() {
  return 1.0f;
}

void DivPlatformN64::setFlags(const DivConfig& flags) {
  /* 32000 IS THE DEFAULT AND IT IS A CHOICE, not a hardware number: the AI
     will clock anything, and 32 kHz is what most N64 titles ran their mixer
     at because 44.1 costs a third more RSP time for the same music. */
  rate=flags.getInt("rate",32000);
  if (rate<8000) rate=8000;
  if (rate>48000) rate=48000;
  chipClock=rate;
  outStereo=flags.getBool("stereo",true);
  interp=flags.getInt("interpolation",1);
  volScale=flags.getInt("volScale",256);
  if (volScale<1) volScale=1;
  for (int i=0; i<chans; i++) {
    oscBuf[i].setRate(rate);
  }
  notifyPitchTable();
}

int DivPlatformN64::init(DivEngine* p, int channels, int sugRate,
                         const DivConfig& flags) {
  (void)sugRate;
  parent=p;
  samplePitchTable.init(parent);
  dumpWrites=false;
  skipRegisterWrites=false;
  oscBuf=new DivDispatchOscBuffer[channels];
  chan=new Channel[channels];
  isMuted=new bool[channels];
  chans=channels;
  for (int i=0; i<channels; i++) {
    isMuted[i]=false;
  }
  sampleBytes=0;
  setFlags(flags);
  reset();
  return chans;
}

void DivPlatformN64::quit() {
  samplePitchTable.destroy<Channel>(chan,chans);
  delete[] chan;
  delete[] isMuted;
  delete[] oscBuf;
  chan=NULL;
  isMuted=NULL;
  oscBuf=NULL;
  chans=0;
}
