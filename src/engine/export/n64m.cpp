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

/* See n64m.h for the layout and for why it is a module rather than a dump or a
 * stream. This runs synchronously - the whole of it is arithmetic over tables
 * already in memory, and a progress bar for something that takes a millisecond
 * is chrome for its own sake. */

#include "n64m.h"
#include "../engine.h"
#include "../ta-log.h"
#include <fmt/printf.h>
#include <math.h>

/* Which effects the player implements. Everything else is refused by name -
   see the header: an export that dropped an effect would make the console play
   something the composer never heard. */
static bool n64mEffectSupported(unsigned char fx) {
  switch (fx) {
    case 0x08:   /* panning */
    case 0x0b:   /* jump to order */
    case 0x0d:   /* break to next order */
    case 0x0f:   /* speed */
      return true;
    default:
      return false;
  }
}

bool DivExportN64M::go(DivEngine* e) {
  failed=false;
  output.clear();
  exportLog.clear();

  int sysIndex=-1;
  for (int i=0; i<e->song.systemLen; i++) {
    if (e->song.system[i]==DIV_SYSTEM_N64) {
      sysIndex=i;
      break;
    }
  }
  if (sysIndex<0) {
    logAppendf("ERROR: this song has no Nintendo 64 chip in it");
    failed=true;
    return false;
  }
  if (e->song.systemLen!=1) {
    logAppendf("ERROR: a .n64m is one machine's song - remove the other %d chip(s)",
               e->song.systemLen-1);
    failed=true;
    return false;
  }

  DivSubSong* sub=e->song.subsong[0];
  const int voices=e->getChannelCount(DIV_SYSTEM_N64);
  const int rows=sub->patLen;
  const int orderLen=sub->ordersLen;
  if (voices<1 || rows<1 || orderLen<1) {
    logAppendf("ERROR: this song has nothing in it");
    failed=true;
    return false;
  }

  /* THE OUTPUT RATE IS THE CHIP'S, not a choice made here. What the console
     will run its mixer at is what the composer has been listening to. */
  int rate=32000;
  {
    DivConfig flags=e->song.systemFlags[sysIndex];
    rate=flags.getInt("rate",32000);
  }
  const double hz=e->curSubSong->hz>0.0?e->curSubSong->hz:60.0;

  /* ------------------------------------------------- patterns, flattened --
     Furnace stores a pattern per channel per index and reuses them across
     orders. The file keeps that: a flat list of patterns and an order matrix
     of indices into it, which is what makes a repeated chorus one copy. */
  std::vector<std::vector<unsigned char> > pats;   /* rows*5 bytes each */
  std::vector<int> patIndex;                       /* (chan,pat) -> file index */
  patIndex.resize((size_t)voices*256,-1);
  std::vector<unsigned short> orders;
  orders.resize((size_t)orderLen*voices,0);

  bool insUsed[256];
  memset(insUsed,0,sizeof(insUsed));

  for (int o=0; o<orderLen; o++) {
    for (int c=0; c<voices; c++) {
      const int which=sub->orders.ord[c][o];
      int& slot=patIndex[(size_t)c*256+which];
      if (slot<0) {
        DivPattern* p=sub->pat[c].getPattern(which,false);
        std::vector<unsigned char> bytes((size_t)rows*5,0);
        for (int r=0; r<rows; r++) {
          const short note=p->newData[r][DIV_PAT_NOTE];
          const short ins=p->newData[r][DIV_PAT_INS];
          const short vol=p->newData[r][DIV_PAT_VOL];
          /* FURNACE'S OWN NOTE NUMBERING, carried across unconverted: 60 is
             C-0 and 108 is C-4. Translating to MIDI here and back in the
             player would be two conversions with one chance to disagree, on
             the one value that decides what pitch comes out. */
          unsigned char n=0;
          if (note==253 || note==254 || note==255) {
            n=1;                       /* off/release: the player has neither */
          } else if (note>=0 && note<=179) {
            n=(unsigned char)(note+2);
          }
          bytes[(size_t)r*5+0]=n;
          bytes[(size_t)r*5+1]=(ins>=0 && ins<255)?(unsigned char)(ins+1):0;
          if (ins>=0 && ins<256) insUsed[ins]=true;
          bytes[(size_t)r*5+2]=(vol>=0 && vol<=254)?(unsigned char)(vol+1):0;
          /* ONE EFFECT COLUMN. A player with a stack of effect slots is a
             player nobody can reason about on a machine with this little CPU
             to spare, and the export says so rather than taking the first. */
          unsigned char fx=0, fxVal=0;
          for (int fxi=0; fxi<sub->pat[c].effectCols; fxi++) {
            const short code=p->newData[r][DIV_PAT_FX(fxi)];
            const short val=p->newData[r][DIV_PAT_FXVAL(fxi)];
            if (code<0) continue;
            if (!n64mEffectSupported((unsigned char)code)) {
              logAppendf("ERROR: channel %d pattern %02X row %02X uses effect "
                         "%02X, which the N64 player does not implement "
                         "(it has 08 pan, 0B jump, 0D break, 0F speed)",
                         c+1,which,r,code);
              failed=true;
              return false;
            }
            if (fx!=0) {
              logAppendf("ERROR: channel %d pattern %02X row %02X has two "
                         "effects on one row; the N64 player runs one",
                         c+1,which,r);
              failed=true;
              return false;
            }
            fx=(unsigned char)code;
            fxVal=(val<0)?0:(unsigned char)val;
          }
          bytes[(size_t)r*5+3]=fx;
          bytes[(size_t)r*5+4]=fxVal;
        }
        slot=(int)pats.size();
        pats.push_back(bytes);
      }
      orders[(size_t)o*voices+c]=(unsigned short)slot;
    }
  }

  /* ------------------------------------------------------- the samples -- */
  std::vector<int> sampleOf;      /* file sample index -> song sample index */
  std::vector<int> sampleSlot;    /* song sample index -> file index */
  sampleSlot.resize(e->song.sample.size(),-1);

  struct InsRec { int sample; int baseNote; int vol; unsigned int step; };
  std::vector<InsRec> insOut;
  std::vector<int> insSlot;
  insSlot.resize(256,-1);

  for (int i=0; i<(int)e->song.ins.size() && i<256; i++) {
    if (!insUsed[i]) continue;
    DivInstrument* ins=e->song.ins[i];
    /* WHICH RECORDING THIS INSTRUMENT IS. A note map would let one instrument
       hold a whole multi-sampled piano; the player has no note map, so the
       export takes the sample the instrument plays at its own base note and
       says which. */
    /* C-4 IN FURNACE'S NUMBERING, which is what centerRate is the rate for. */
    const int baseNote=108;
    int smp=ins->amiga.getSample(baseNote);
    if (smp<0) smp=ins->amiga.initSample;
    if (smp<0 || smp>=(int)e->song.sample.size()) {
      logAppendf("ERROR: instrument %02X (%s) plays no sample",i,
                 ins->name.c_str());
      failed=true;
      return false;
    }
    if (sampleSlot[smp]<0) {
      sampleSlot[smp]=(int)sampleOf.size();
      sampleOf.push_back(smp);
    }
    DivSample* s=e->song.sample[smp];
    /* THE STEP, IN 16.16, AT THE BASE NOTE - exported rather than derived on
       the console. The alternative is a second implementation of Furnace's
       pitch table inside the game, and two implementations of a tuning is a
       song that is subtly out of tune on one of the two machines. */
    const double centre=(s->centerRate>0)?(double)s->centerRate:8363.0;
    const double step=65536.0*centre/(double)rate;
    InsRec rec;
    rec.sample=sampleSlot[smp];
    rec.baseNote=baseNote;
    rec.vol=255;
    rec.step=(unsigned int)(step+0.5);
    if (rec.step<1) rec.step=1;
    insSlot[i]=(int)insOut.size();
    insOut.push_back(rec);
  }
  if (insOut.empty()) {
    logAppendf("ERROR: this song plays no instruments");
    failed=true;
    return false;
  }

  /* Re-point the pattern bytes at the file's own instrument numbering. */
  for (size_t p=0; p<pats.size(); p++) {
    for (int r=0; r<rows; r++) {
      unsigned char& v=pats[p][(size_t)r*5+1];
      if (v==0) continue;
      const int songIns=v-1;
      const int slot=(songIns<256)?insSlot[songIns]:-1;
      v=(slot>=0)?(unsigned char)(slot+1):0;
    }
  }

  /* ------------------------------------------------------------- write -- */
  SafeWriter* w=new SafeWriter;
  w->init();
  w->write("UFN6",4);
  w->writeS_BE(1);                          /* version */
  w->writeS_BE(e->song.subsong[0]->ordersLen>1?1:1);  /* flags: loops */
  w->writeS_BE((short)rate);
  w->writeS_BE((short)voices);
  w->writeS_BE((short)(int)(hz+0.5));
  w->writeS_BE((short)sub->speeds.val[0]);
  w->writeS_BE((short)rows);
  w->writeS_BE((short)orderLen);
  w->writeS_BE((short)pats.size());
  w->writeS_BE((short)insOut.size());
  w->writeS_BE((short)sampleOf.size());
  w->writeS_BE(0);                          /* loop order */
  {
    char nameBuf[24];
    memset(nameBuf,0,sizeof(nameBuf));
    String n=e->song.name.empty()?String("untitled"):e->song.name;
    strncpy(nameBuf,n.c_str(),23);
    w->write(nameBuf,24);
  }

  for (size_t i=0; i<orders.size(); i++) w->writeS_BE((short)orders[i]);
  for (size_t p=0; p<pats.size(); p++) w->write(pats[p].data(),pats[p].size());
  for (size_t i=0; i<insOut.size(); i++) {
    w->writeS_BE((short)insOut[i].sample);
    w->writeC((signed char)insOut[i].baseNote);
    w->writeC((signed char)insOut[i].vol);
    w->writeI_BE((int)insOut[i].step);
  }

  size_t pcmFrames=0;
  for (size_t i=0; i<sampleOf.size(); i++) {
    DivSample* s=e->song.sample[sampleOf[i]];
    const unsigned int frames=s->samples;
    int ls=-1, le=-1;
    if (s->isLoopable() && s->loopEnd>s->loopStart) {
      ls=s->loopStart;
      le=s->loopEnd;
      if (le>(int)frames) le=frames;
    }
    w->writeI_BE((int)frames);
    w->writeI_BE(ls);
    w->writeI_BE(le);
    pcmFrames+=frames;
  }
  for (size_t i=0; i<sampleOf.size(); i++) {
    DivSample* s=e->song.sample[sampleOf[i]];
    for (unsigned int f=0; f<s->samples; f++) {
      w->writeS_BE(s->data16!=NULL?s->data16[f]:0);
    }
  }

  logAppendf("%d voices, %d orders, %d patterns, %d instruments, %d samples "
             "(%d frames), %d bytes",
             voices,orderLen,(int)pats.size(),(int)insOut.size(),
             (int)sampleOf.size(),(int)pcmFrames,(int)w->size());

  String outName=e->song.name.empty()?String("song"):e->song.name;
  output.push_back(DivROMExportOutput(outName+".n64m",w));
  return true;
}

bool DivExportN64M::isRunning() { return false; }
bool DivExportN64M::hasFailed() { return failed; }
void DivExportN64M::abort() {}
void DivExportN64M::wait() {}

DivROMExportProgress DivExportN64M::getProgress(int index) {
  DivROMExportProgress p;
  p.name=(index==0)?"Exporting...":"";
  p.amount=1.0f;
  return p;
}
