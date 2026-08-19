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

#include "xm.h"
#include "../engine.h"
#include "../ta-log.h"
#include <fmt/printf.h>
#include <math.h>

// XM note 1 is what fileOps/xm.cpp turns into Furnace note 60, so that is the
// offset this file subtracts. XM has 96 notes, hence the upper bound.
#define XM_NOTE_BASE 59
#define XM_NOTE_MIN 60
#define XM_NOTE_MAX 155
#define XM_KEY_OFF 97

// Limits of the format itself, all of them things FT2 will not read past.
#define XM_MAX_CHANNELS 32
#define XM_MAX_PATTERNS 256
#define XM_MAX_ORDERS 256
#define XM_MAX_ROWS 256
#define XM_MAX_INSTRUMENTS 128
#define XM_MAX_SAMPLES_PER_INS 16
#define XM_MAX_ENV_POINTS 12

struct XMEnvPoint {
  int frame, val;
};

// Fit a Furnace macro - one value per tick - onto XM envelope points, which
// are joined by straight lines. Only exact fits are accepted: a segment is
// extended while every tick inside it lands on the line, and a new point is
// emitted the moment one does not. So the envelope either reproduces the macro
// tick for tick or the caller refuses the export. Approximating here would
// change the sound without saying so.
static bool fitMacroToEnvelope(const int* val, int len, int loopIdx, int relIdx,
                               std::vector<XMEnvPoint>& pts) {
  pts.clear();
  if (len<1) return true;

  // Points the shape has to land on exactly, because XM addresses its loop and
  // its sustain by point index rather than by tick.
  std::vector<bool> forced;
  forced.resize(len,false);
  forced[0]=true;
  forced[len-1]=true;
  if (loopIdx>=0 && loopIdx<len) forced[loopIdx]=true;
  if (relIdx>=0 && relIdx<len) forced[relIdx]=true;

  XMEnvPoint first;
  first.frame=0;
  first.val=val[0];
  pts.push_back(first);

  int start=0;
  while (start<len-1) {
    int best=start+1;
    for (int end=start+1; end<len; end++) {
      bool ok=true;
      for (int k=start+1; k<end; k++) {
        if (forced[k]) {
          ok=false;
          break;
        }
        const int interp=val[start]+(((val[end]-val[start])*(k-start))/(end-start));
        if (interp!=val[k]) {
          ok=false;
          break;
        }
      }
      if (!ok) break;
      best=end;
    }
    XMEnvPoint p;
    p.frame=best;
    p.val=val[best];
    pts.push_back(p);
    start=best;
  }

  return (int)pts.size()<=XM_MAX_ENV_POINTS;
}

// Which point index sits on a given tick, or -1. XM loop and sustain are point
// indices, so a tick that is not a point cannot be addressed at all.
static int pointAtFrame(const std::vector<XMEnvPoint>& pts, int frame) {
  for (size_t i=0; i<pts.size(); i++) {
    if (pts[i].frame==frame) return (int)i;
  }
  return -1;
}

// Furnace effect to XM effect.
//
// The codes agree for most of the range because Furnace took them from XM, but
// they do NOT agree everywhere and the mismatches are silent if you get them
// wrong:
//
//   Furnace 05 is volume slide + VIBRATO, and XM 5 is tone portamento + volume
//   slide. Furnace 06 is volume slide + PORTAMENTO, and XM 6 is vibrato +
//   volume slide. The pair is swapped. Do not "correct" this.
//
//   Furnace 09 selects a groove and 0C retriggers; XM 9 is sample offset and
//   XM C sets volume. Those are different effects wearing the same number, so
//   0C is translated to E9y and 09 is refused.
static bool mapEffect(unsigned char fx, unsigned char val,
                      unsigned char& xfx, unsigned char& xval, String& why) {
  switch (fx) {
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x07:
    case 0x0a: case 0x0b:
      xfx=fx;
      xval=val;
      return true;
    case 0x05:
      xfx=0x06;
      xval=val;
      return true;
    case 0x06:
      xfx=0x05;
      xval=val;
      return true;
    case 0x0d:
      // Furnace breaks to a row counted in hex. XM reads its parameter as two
      // decimal digits, so 0D10 means row 10 and not row 16.
      if (val>99) {
        why=fmt::sprintf("0D%02X breaks to row %d, and XM cannot count past 99",val,val);
        return false;
      }
      xfx=0x0d;
      xval=(unsigned char)(((val/10)<<4)|(val%10));
      return true;
    case 0x0f:
      // XM packs speed and tempo into one effect, split at 0x20.
      if (val==0 || val>=0x20) {
        why=fmt::sprintf("0F%02X sets a speed XM reads as a tempo (it splits the two at 20)",val);
        return false;
      }
      xfx=0x0f;
      xval=val;
      return true;
    case 0x80:
      xfx=0x08;
      xval=val;
      return true;
    case 0x0c:
      if (val>15) {
        why=fmt::sprintf("0C%02X retriggers every %d ticks, and XM's E9y counts to 15 only",val,val);
        return false;
      }
      xfx=0x0e;
      xval=(unsigned char)(0x90|val);
      return true;
    case 0xec:
      if (val>15) {
        why=fmt::sprintf("EC%02X cuts after %d ticks, and XM's ECy counts to 15 only",val,val);
        return false;
      }
      xfx=0x0e;
      xval=(unsigned char)(0xc0|val);
      return true;
    case 0xed:
      if (val>15) {
        why=fmt::sprintf("ED%02X delays by %d ticks, and XM's EDy counts to 15 only",val,val);
        return false;
      }
      xfx=0x0e;
      xval=(unsigned char)(0xd0|val);
      return true;
    case 0x08:
      why="08xy sets panning as two nibbles, which XM has no room for - use 80xx";
      return false;
    case 0x09:
      why="09xx selects a groove, and XM has one speed per row";
      return false;
    case 0x90: case 0x91: case 0x92: case 0x93:
      why="sample offset is not exported yet - XM's 9xx is a whole 256-frame step";
      return false;
    default:
      why=fmt::sprintf("effect %02X has no XM equivalent",fx);
      return false;
  }
}

static void writePadded(SafeWriter* w, const String& s, int width) {
  for (int i=0; i<width; i++) {
    w->writeC((i<(int)s.size())?s[i]:0);
  }
}

bool DivExportXM::go(DivEngine* e) {
  failed=false;
  output.clear();
  exportLog.clear();

  if (e->song.systemLen!=1) {
    logAppendf("ERROR: an XM is one machine's song - remove the other %d chip(s)",
               e->song.systemLen-1);
    failed=true;
    return false;
  }

  DivSubSong* sub=e->song.subsong[0];
  if (e->song.subsong.size()>1) {
    logAppendf("NOTE: this song has %d subsongs and XM holds one - exporting the first",
               (int)e->song.subsong.size());
  }

  const int chans=e->getChannelCount(e->song.system[0]);
  const int rows=sub->patLen;
  const int orderLen=sub->ordersLen;

  if (chans<1 || rows<1 || orderLen<1) {
    logAppendf("ERROR: this song has nothing in it");
    failed=true;
    return false;
  }
  if (chans>XM_MAX_CHANNELS) {
    logAppendf("ERROR: %d channels, and XM holds %d",chans,XM_MAX_CHANNELS);
    failed=true;
    return false;
  }
  if (rows>XM_MAX_ROWS) {
    logAppendf("ERROR: %d rows a pattern, and XM holds %d",rows,XM_MAX_ROWS);
    failed=true;
    return false;
  }
  if (orderLen>XM_MAX_ORDERS) {
    logAppendf("ERROR: %d orders, and XM holds %d",orderLen,XM_MAX_ORDERS);
    failed=true;
    return false;
  }
  if (sub->speeds.len!=1) {
    logAppendf("ERROR: this song runs a groove of %d speeds, and XM has one speed at a time",
               sub->speeds.len);
    failed=true;
    return false;
  }

  // XM counts tempo in BPM at 2.5 ticks a beat, which is the relation
  // fileOps/xm.cpp reads in the other direction.
  const double hz=(sub->hz>0.0)?sub->hz:60.0;
  const double bpmExact=hz*2.5;
  int bpm=(int)(bpmExact+0.5);
  if (bpm<32) bpm=32;
  if (bpm>255) bpm=255;
  if (fabs(bpmExact-(double)bpm)>0.01) {
    logAppendf("NOTE: %g Hz is %g BPM and XM counts BPM in whole numbers - exported as %d",
               hz,bpmExact,bpm);
  }

  int speed=sub->speeds.val[0];
  if (speed<1) speed=1;
  if (speed>31) {
    logAppendf("ERROR: speed %d, and XM reads anything from 20 up as a tempo",speed);
    failed=true;
    return false;
  }

  // The chip's volume range against XM's. Every sample chip here counts volume
  // to its own maximum and XM's column stops at 64, so say what the rescale
  // costs rather than letting it happen quietly.
  int volMax=64;
  {
    DivDispatch* d=e->getDispatch(0);
    if (d!=NULL) {
      const int q=d->dispatch(DivCommand(DIV_CMD_GET_VOLMAX,0));
      if (q>0) volMax=q;
    }
  }
  if (volMax!=64) {
    logAppendf("NOTE: this chip counts volume to %d and XM's column stops at 64 - "
               "volumes are rescaled",volMax);
  }

  /* ------------------------------------------------------------ patterns --
     Furnace gives every channel its own order list. XM has one order list and
     a pattern is all the channels at once. So each Furnace order becomes a
     column combination, and identical combinations share one XM pattern -
     which is what keeps a repeated chorus a single copy. */
  std::vector<std::vector<unsigned short> > combos;
  std::vector<unsigned char> orderOut;
  orderOut.resize(orderLen,0);

  for (int o=0; o<orderLen; o++) {
    std::vector<unsigned short> key;
    key.resize(chans,0);
    for (int c=0; c<chans; c++) {
      key[c]=sub->orders.ord[c][o];
    }
    int found=-1;
    for (size_t i=0; i<combos.size(); i++) {
      if (combos[i]==key) {
        found=(int)i;
        break;
      }
    }
    if (found<0) {
      if ((int)combos.size()>=XM_MAX_PATTERNS) {
        logAppendf("ERROR: this song needs more than %d XM patterns",XM_MAX_PATTERNS);
        failed=true;
        return false;
      }
      found=(int)combos.size();
      combos.push_back(key);
    }
    orderOut[o]=(unsigned char)found;
  }

  bool insUsed[256];
  memset(insUsed,0,sizeof(insUsed));
  bool warnedNoteOff=false;

  std::vector<std::vector<unsigned char> > packed;

  for (size_t p=0; p<combos.size(); p++) {
    std::vector<unsigned char> bytes;
    for (int r=0; r<rows; r++) {
      for (int c=0; c<chans; c++) {
        DivPattern* pat=sub->pat[c].getPattern(combos[p][c],false);
        const short note=pat->newData[r][DIV_PAT_NOTE];
        const short ins=pat->newData[r][DIV_PAT_INS];
        const short vol=pat->newData[r][DIV_PAT_VOL];

        unsigned char xNote=0;
        if (note==DIV_NOTE_OFF || note==DIV_NOTE_REL) {
          // XM has one way to end a note and Furnace has two. Both become key
          // off, which releases the envelope.
          xNote=XM_KEY_OFF;
          if (note==DIV_NOTE_OFF && !warnedNoteOff) {
            logAppendf("NOTE: note off becomes XM key off, which releases the "
                       "envelope instead of cutting");
            warnedNoteOff=true;
          }
        } else if (note>=XM_NOTE_MIN && note<=XM_NOTE_MAX) {
          xNote=(unsigned char)(note-XM_NOTE_BASE);
        } else if (note>0 && note<DIV_NOTE_RAW) {
          logAppendf("ERROR: channel %d pattern %02X row %02X plays a note outside "
                     "XM's eight octaves",c+1,(int)p,r);
          failed=true;
          return false;
        }

        unsigned char xIns=0;
        if (ins>=0 && ins<XM_MAX_INSTRUMENTS) {
          insUsed[ins]=true;
          xIns=(unsigned char)(ins+1);
        } else if (ins>=XM_MAX_INSTRUMENTS) {
          logAppendf("ERROR: instrument %02X is past XM's %d",ins,XM_MAX_INSTRUMENTS);
          failed=true;
          return false;
        }

        unsigned char xVol=0;
        if (vol>=0) {
          int v=(volMax==64)?vol:((vol*64+volMax/2)/volMax);
          if (v>64) v=64;
          if (v<0) v=0;
          xVol=(unsigned char)(0x10+v);
        }

        unsigned char xFx=0, xFxVal=0;
        bool hasFx=false;
        for (int fxi=0; fxi<sub->pat[c].effectCols; fxi++) {
          const short code=pat->newData[r][DIV_PAT_FX(fxi)];
          const short val=pat->newData[r][DIV_PAT_FXVAL(fxi)];
          if (code<0) continue;
          if (hasFx) {
            logAppendf("ERROR: channel %d pattern %02X row %02X has two effects, "
                       "and an XM row holds one",c+1,(int)p,r);
            failed=true;
            return false;
          }
          String why;
          if (!mapEffect((unsigned char)code,(val<0)?0:(unsigned char)val,xFx,xFxVal,why)) {
            logAppendf("ERROR: channel %d pattern %02X row %02X: %s",
                       c+1,(int)p,r,why.c_str());
            failed=true;
            return false;
          }
          hasFx=true;
        }

        unsigned char mask=0x80;
        if (xNote) mask|=1;
        if (xIns) mask|=2;
        if (xVol) mask|=4;
        if (hasFx) mask|=8|16;
        bytes.push_back(mask);
        if (xNote) bytes.push_back(xNote);
        if (xIns) bytes.push_back(xIns);
        if (xVol) bytes.push_back(xVol);
        if (hasFx) {
          bytes.push_back(xFx);
          bytes.push_back(xFxVal);
        }
      }
    }
    if (bytes.size()>65535) {
      logAppendf("ERROR: pattern %d packs to %d bytes, and XM counts to 65535",
                 (int)p,(int)bytes.size());
      failed=true;
      return false;
    }
    packed.push_back(bytes);
  }

  /* --------------------------------------------------------- instruments --
     XM's instrument is the thing our own format did not have: a sample for
     each key, plus a volume and a panning envelope. So the macros and the note
     map that a .n64m dropped are carried here instead. */
  int insCount=0;
  for (int i=0; i<XM_MAX_INSTRUMENTS; i++) {
    if (insUsed[i]) insCount=i+1;
  }
  if (insCount<1) {
    logAppendf("ERROR: this song plays no instruments");
    failed=true;
    return false;
  }

  struct InsOut {
    std::vector<int> samples;              // song sample index, in XM order
    unsigned char noteMap[96];
    std::vector<XMEnvPoint> volEnv, panEnv;
    int volLoopS, volLoopE, volSus;
    int panLoopS, panLoopE, panSus;
    bool volLoop, volSustain, panLoop, panSustain;
    String name;
    InsOut():
      volLoopS(0), volLoopE(0), volSus(0),
      panLoopS(0), panLoopE(0), panSus(0),
      volLoop(false), volSustain(false), panLoop(false), panSustain(false) {
      memset(noteMap,0,sizeof(noteMap));
    }
  };
  std::vector<InsOut> insOut;
  insOut.resize(insCount);

  for (int i=0; i<insCount; i++) {
    InsOut& io=insOut[i];
    if (!insUsed[i] || i>=(int)e->song.ins.size()) continue;
    DivInstrument* ins=e->song.ins[i];
    io.name=ins->name;

    // One sample per key, which is what XM stores and what our own format
    // could not. Furnace numbers notes from 60 where XM numbers from 1.
    for (int n=0; n<96; n++) {
      const int furnaceNote=n+XM_NOTE_MIN;
      short smp=ins->amiga.getSample(furnaceNote);
      if (smp<0) smp=ins->amiga.initSample;
      if (smp<0 || smp>=(int)e->song.sample.size()) {
        io.noteMap[n]=0;
        continue;
      }
      int slot=-1;
      for (size_t k=0; k<io.samples.size(); k++) {
        if (io.samples[k]==smp) {
          slot=(int)k;
          break;
        }
      }
      if (slot<0) {
        if ((int)io.samples.size()>=XM_MAX_SAMPLES_PER_INS) {
          logAppendf("ERROR: instrument %02X (%s) uses more than %d samples, "
                     "which is XM's limit for one instrument",
                     i,ins->name.c_str(),XM_MAX_SAMPLES_PER_INS);
          failed=true;
          return false;
        }
        slot=(int)io.samples.size();
        io.samples.push_back(smp);
      }
      io.noteMap[n]=(unsigned char)slot;
    }

    if (io.samples.empty()) {
      logAppendf("ERROR: instrument %02X (%s) plays no sample",i,ins->name.c_str());
      failed=true;
      return false;
    }

    // Volume macro to volume envelope.
    if (ins->std.volMacro.len>0) {
      const int loopIdx=(ins->std.volMacro.loop<ins->std.volMacro.len)?ins->std.volMacro.loop:-1;
      const int relIdx=(ins->std.volMacro.rel<ins->std.volMacro.len)?ins->std.volMacro.rel:-1;
      int vals[256];
      for (int k=0; k<ins->std.volMacro.len && k<256; k++) {
        int v=ins->std.volMacro.val[k];
        if (v<0) v=0;
        if (v>64) v=64;
        vals[k]=v;
      }
      if (!fitMacroToEnvelope(vals,ins->std.volMacro.len,loopIdx,relIdx,io.volEnv)) {
        logAppendf("ERROR: instrument %02X (%s) has a volume macro that needs more "
                   "than %d envelope points - XM joins its points with straight lines",
                   i,ins->name.c_str(),XM_MAX_ENV_POINTS);
        failed=true;
        return false;
      }
      if (loopIdx>=0) {
        const int lp=pointAtFrame(io.volEnv,loopIdx);
        if (lp>=0) {
          io.volLoop=true;
          io.volLoopS=lp;
          io.volLoopE=(int)io.volEnv.size()-1;
        }
      }
      if (relIdx>=0) {
        const int sp=pointAtFrame(io.volEnv,relIdx);
        if (sp>=0) {
          io.volSustain=true;
          io.volSus=sp;
        }
      }
    }

    // Panning macros to panning envelope. Furnace keeps a left and a right
    // level; XM keeps one position, 32 being centre.
    if (ins->std.panLMacro.len>0 || ins->std.panRMacro.len>0) {
      const int len=MAX(ins->std.panLMacro.len,ins->std.panRMacro.len);
      int vals[256];
      for (int k=0; k<len && k<256; k++) {
        const int l=(k<ins->std.panLMacro.len)?ins->std.panLMacro.val[k]:127;
        const int r=(k<ins->std.panRMacro.len)?ins->std.panRMacro.val[k]:127;
        const int top=MAX(MAX(l,r),1);
        int pos=32+((r-l)*32)/top;
        if (pos<0) pos=0;
        if (pos>64) pos=64;
        vals[k]=pos;
      }
      const int loopIdx=(ins->std.panLMacro.loop<len)?ins->std.panLMacro.loop:-1;
      const int relIdx=(ins->std.panLMacro.rel<len)?ins->std.panLMacro.rel:-1;
      if (!fitMacroToEnvelope(vals,len,loopIdx,relIdx,io.panEnv)) {
        logAppendf("ERROR: instrument %02X (%s) has a panning macro that needs more "
                   "than %d envelope points",i,ins->name.c_str(),XM_MAX_ENV_POINTS);
        failed=true;
        return false;
      }
      if (loopIdx>=0) {
        const int lp=pointAtFrame(io.panEnv,loopIdx);
        if (lp>=0) {
          io.panLoop=true;
          io.panLoopS=lp;
          io.panLoopE=(int)io.panEnv.size()-1;
        }
      }
      if (relIdx>=0) {
        const int sp=pointAtFrame(io.panEnv,relIdx);
        if (sp>=0) {
          io.panSustain=true;
          io.panSus=sp;
        }
      }
    }
  }

  /* --------------------------------------------------------------- write -- */
  SafeWriter* w=new SafeWriter;
  w->init();

  w->write("Extended Module: ",17);
  writePadded(w,e->song.name,20);
  w->writeC(0x1a);
  writePadded(w,"Furnace Tracker",20);
  w->writeS(0x0104);
  w->writeI(276);                                    // header size, itself included
  w->writeS((short)orderLen);
  w->writeS(0);                                      // restart position
  w->writeS((short)chans);
  w->writeS((short)combos.size());
  w->writeS((short)insCount);
  w->writeS((e->song.compatFlags.linearPitch!=0)?1:0);
  w->writeS((short)speed);
  w->writeS((short)bpm);
  for (int i=0; i<XM_MAX_ORDERS; i++) {
    w->writeC((i<orderLen)?orderOut[i]:0);
  }

  for (size_t p=0; p<packed.size(); p++) {
    w->writeI(9);                                    // pattern header length
    w->writeC(0);                                    // packing type
    w->writeS((short)rows);
    w->writeS((short)packed[p].size());
    if (!packed[p].empty()) {
      w->write(packed[p].data(),packed[p].size());
    }
  }

  for (int i=0; i<insCount; i++) {
    InsOut& io=insOut[i];
    const int numSamples=(int)io.samples.size();

    w->writeI(263);
    writePadded(w,io.name,22);
    w->writeC(0);                                    // instrument type
    w->writeS((short)numSamples);

    w->writeI(40);                                   // sample header size
    w->write(io.noteMap,96);
    for (int k=0; k<XM_MAX_ENV_POINTS; k++) {
      if (k<(int)io.volEnv.size()) {
        w->writeS((short)io.volEnv[k].frame);
        w->writeS((short)io.volEnv[k].val);
      } else {
        w->writeS(0);
        w->writeS(0);
      }
    }
    for (int k=0; k<XM_MAX_ENV_POINTS; k++) {
      if (k<(int)io.panEnv.size()) {
        w->writeS((short)io.panEnv[k].frame);
        w->writeS((short)io.panEnv[k].val);
      } else {
        w->writeS(0);
        w->writeS(0);
      }
    }
    w->writeC((signed char)io.volEnv.size());
    w->writeC((signed char)io.panEnv.size());
    w->writeC((signed char)io.volSus);
    w->writeC((signed char)io.volLoopS);
    w->writeC((signed char)io.volLoopE);
    w->writeC((signed char)io.panSus);
    w->writeC((signed char)io.panLoopS);
    w->writeC((signed char)io.panLoopE);
    w->writeC((signed char)(io.volEnv.empty()?0:(1|(io.volSustain?2:0)|(io.volLoop?4:0))));
    w->writeC((signed char)(io.panEnv.empty()?0:(1|(io.panSustain?2:0)|(io.panLoop?4:0))));
    w->writeC(0);                                    // vibrato type
    w->writeC(0);                                    // vibrato sweep
    w->writeC(0);                                    // vibrato depth
    w->writeC(0);                                    // vibrato rate
    w->writeS(0);                                    // volume fadeout
    for (int k=0; k<22; k++) w->writeC(0);           // reserved

    for (int k=0; k<numSamples; k++) {
      DivSample* s=e->song.sample[io.samples[k]];
      if (s->depth!=DIV_SAMPLE_DEPTH_8BIT && s->depth!=DIV_SAMPLE_DEPTH_16BIT) {
        logAppendf("ERROR: sample %d (%s) is not 8- or 16-bit PCM, which is all XM stores",
                   io.samples[k],s->name.c_str());
        failed=true;
        w->finish();
        delete w;
        return false;
      }
      const bool wide=(s->depth==DIV_SAMPLE_DEPTH_16BIT);
      const int step=wide?2:1;
      const unsigned int frames=s->samples;

      int loopStart=s->loopStart, loopEnd=s->loopEnd;
      if (loopStart<0) loopStart=0;
      if (loopEnd>(int)frames || loopEnd<0) loopEnd=(int)frames;
      const int loopLen=(loopEnd>loopStart)?(loopEnd-loopStart):0;

      unsigned char type=0;
      if (s->loop && loopLen>0) {
        type=(s->loopMode==DIV_SAMPLE_LOOP_PINGPONG)?2:1;
      }
      if (wide) type|=0x10;

      // XM tunes a sample by how far its recording sits from 8363 Hz, which is
      // the inverse of what fileOps/xm.cpp computes when it reads one.
      const double centre=(s->centerRate>0)?(double)s->centerRate:8363.0;
      const double semis=12.0*log2(centre/8363.0);
      int relNote=(int)floor(semis+0.5);
      int fine=(int)floor((semis-(double)relNote)*128.0+0.5);
      if (fine>127) {
        fine-=128;
        relNote++;
      }
      if (fine<-128) {
        fine+=128;
        relNote--;
      }
      if (relNote>127) relNote=127;
      if (relNote<-128) relNote=-128;

      w->writeI((int)frames*step);
      w->writeI(loopStart*step);
      w->writeI(loopLen*step);
      w->writeC(64);                                 // sample volume
      w->writeC((signed char)fine);
      w->writeC((signed char)type);
      w->writeC((signed char)128);                   // sample panning, centre
      w->writeC((signed char)relNote);
      w->writeC(0);                                  // reserved
      writePadded(w,s->name,22);
    }

    // XM stores every sample's data after every sample's header, as deltas.
    for (int k=0; k<numSamples; k++) {
      DivSample* s=e->song.sample[io.samples[k]];
      if (s->depth==DIV_SAMPLE_DEPTH_16BIT) {
        short old=0;
        for (unsigned int j=0; j<s->samples; j++) {
          const short cur=s->data16[j];
          w->writeS((short)(cur-old));
          old=cur;
        }
      } else {
        signed char old=0;
        for (unsigned int j=0; j<s->samples; j++) {
          const signed char cur=s->data8[j];
          w->writeC((signed char)(cur-old));
          old=cur;
        }
      }
    }
  }

  // The writer is handed over open. Whoever takes the result reads the buffer
  // and then finishes it, and finish() is what frees the buffer - so calling it
  // here would hand over nothing at all.
  const String outName=e->song.name.empty()?String("song"):e->song.name;
  output.push_back(DivROMExportOutput(outName+".xm",w));
  logAppendf("wrote %d channels, %d patterns, %d instruments",
             chans,(int)combos.size(),insCount);
  return true;
}

bool DivExportXM::hasFailed() { return failed; }
bool DivExportXM::isRunning() { return false; }
void DivExportXM::abort() {}
void DivExportXM::wait() {}

DivROMExportProgress DivExportXM::getProgress(int index) {
  DivROMExportProgress p;
  p.name="";
  p.amount=(index==0)?1.0f:0.0f;
  return p;
}
