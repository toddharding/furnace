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

/* See soundfont.h. An .sf2 is RIFF: a header list, a PCM blob, and an index of
 * fixed-size records over it. Nothing here is clever - it is the index, read.
 *
 * THE ONE THING WORTH KNOWING about the format is that it is two levels of
 * indirection on purpose: a PRESET (what a musician picks - "Acoustic Grand
 * Piano", bank 0 program 0) points at one or more INSTRUMENTS, and each of
 * those points at one or more SAMPLES, keyed by note range. That is how one
 * piano is eight recordings. Both levels are walked here, because taking the
 * first sample of the first instrument gives you a piano that is one recording
 * stretched over eight octaves, and it sounds like it.
 */

#include "soundfont.h"
#include "../ta-log.h"
#include <string.h>
#include <math.h>
#include <stdio.h>

/* SF2 generator opcodes, of which exactly six matter to a machine that can
   only play a sample at a rate. */
#define SFGEN_KEY_RANGE              43
#define SFGEN_INSTRUMENT             41
#define SFGEN_SAMPLE_ID              53
#define SFGEN_FINE_TUNE              52
#define SFGEN_COARSE_TUNE            51
#define SFGEN_OVERRIDING_ROOT_KEY    58

namespace {

struct Reader {
  FILE* f;
  bool ok;
  Reader(FILE* file): f(file), ok(true) {}
  unsigned int u32() {
    unsigned char b[4];
    if (fread(b,1,4,f)!=4) { ok=false; return 0; }
    return b[0]|(b[1]<<8)|(b[2]<<16)|((unsigned int)b[3]<<24);
  }
  void fourcc(char* out) {
    if (fread(out,1,4,f)!=4) { ok=false; memset(out,0,4); }
    out[4]=0;
  }
};

size_t nameLen(const char* s, size_t max) {
  size_t n=0;
  while (n<max && s[n]!=0) n++;
  return n;
}

}

bool DivSoundFont::open(const char* p) {
  path=p;
  error="";
  presets.clear();
  presetZones.clear();
  insts.clear();
  shdrs.clear();
  smplOffset=0;
  smplLen=0;

  FILE* f=fopen(p,"rb");
  if (f==NULL) {
    error=fmt::sprintf("could not open %s",p);
    return false;
  }
  Reader r(f);
  char tag[5];
  r.fourcc(tag);
  if (strcmp(tag,"RIFF")!=0) {
    error="not a RIFF file";
    fclose(f);
    return false;
  }
  const unsigned int riffLen=r.u32();
  r.fourcc(tag);
  if (strcmp(tag,"sfbk")!=0) {
    error="RIFF, but not a SoundFont (no sfbk)";
    fclose(f);
    return false;
  }

  /* The three top-level LISTs. Each chunk inside is walked by name; anything
     unrecognised is skipped by its own length, which is what makes this
     survive the vendor extensions real soundfonts carry. */
  const long end=(long)riffLen+8;
  std::vector<unsigned char> phdr,pbag,pgen,inst,ibag,igen,shdr;
  while (r.ok && ftell(f)<end) {
    char listTag[5], listType[5];
    r.fourcc(listTag);
    const unsigned int listLen=r.u32();
    if (!r.ok) break;
    const long listEnd=ftell(f)+(long)listLen;
    if (strcmp(listTag,"LIST")!=0) {
      fseek(f,listEnd,SEEK_SET);
      continue;
    }
    r.fourcc(listType);
    while (r.ok && ftell(f)<listEnd) {
      char sub[5];
      r.fourcc(sub);
      const unsigned int subLen=r.u32();
      if (!r.ok) break;
      const long next=ftell(f)+(long)subLen+(long)(subLen&1);
      if (strcmp(listType,"INFO")==0 && strcmp(sub,"INAM")==0) {
        std::vector<char> b(subLen+1,0);
        if (subLen>0 && fread(b.data(),1,subLen,f)!=subLen) r.ok=false;
        fontName=String(b.data());
      } else if (strcmp(listType,"sdta")==0 && strcmp(sub,"smpl")==0) {
        /* NOT READ HERE. A General MIDI soundfont is tens of megabytes of PCM
           and an import wants one flute out of it, so the offset is kept and
           the frames are read when a preset is asked for. */
        smplOffset=ftell(f);
        smplLen=subLen/2;
      } else if (strcmp(listType,"pdta")==0) {
        std::vector<unsigned char>* into=NULL;
        if (strcmp(sub,"phdr")==0) into=&phdr;
        else if (strcmp(sub,"pbag")==0) into=&pbag;
        else if (strcmp(sub,"pgen")==0) into=&pgen;
        else if (strcmp(sub,"inst")==0) into=&inst;
        else if (strcmp(sub,"ibag")==0) into=&ibag;
        else if (strcmp(sub,"igen")==0) into=&igen;
        else if (strcmp(sub,"shdr")==0) into=&shdr;
        if (into!=NULL) {
          into->resize(subLen);
          if (subLen>0 && fread(into->data(),1,subLen,f)!=subLen) r.ok=false;
        }
      }
      fseek(f,next,SEEK_SET);
    }
    fseek(f,listEnd,SEEK_SET);
  }
  fclose(f);

  if (phdr.empty() || pbag.empty() || pgen.empty() || inst.empty() ||
      ibag.empty() || igen.empty() || shdr.empty() || smplLen==0) {
    error="this soundfont has no preset index in it (pdta/sdta incomplete)";
    return false;
  }

  /* -------------------------------------------------------- the samples -- */
  const size_t nShdr=shdr.size()/46;
  for (size_t i=0; i+1<nShdr; i++) {   /* the last record is the EOS terminal */
    const unsigned char* b=&shdr[i*46];
    Shdr s;
    s.name=String((const char*)b,nameLen((const char*)b,20));
    unsigned int w[5];
    for (int k=0; k<5; k++) {
      const unsigned char* q=b+20+k*4;
      w[k]=q[0]|(q[1]<<8)|(q[2]<<16)|((unsigned int)q[3]<<24);
    }
    s.start=w[0]; s.end=w[1]; s.loopStart=w[2]; s.loopEnd=w[3]; s.rate=w[4];
    s.rootKey=b[40];
    s.correction=(signed char)b[41];
    s.type=b[44]|(b[45]<<8);
    shdrs.push_back(s);
  }

  /* ---------------------------------------------------- the instruments -- */
  const size_t nInst=inst.size()/22;
  const size_t nIbag=ibag.size()/4;
  const size_t nIgen=igen.size()/4;
  for (size_t i=0; i+1<nInst; i++) {
    const unsigned char* b=&inst[i*22];
    Inst it;
    it.name=String((const char*)b,nameLen((const char*)b,20));
    const unsigned int bagStart=b[20]|(b[21]<<8);
    const unsigned char* nb=&inst[(i+1)*22];
    const unsigned int bagEnd=nb[20]|(nb[21]<<8);
    for (unsigned int bg=bagStart; bg+1<bagEnd+1 && bg+1<nIbag && bg<bagEnd; bg++) {
      const unsigned int genStart=ibag[bg*4]|(ibag[bg*4+1]<<8);
      const unsigned int genEnd=ibag[(bg+1)*4]|(ibag[(bg+1)*4+1]<<8);
      Zone z={0,127,-1,-1,-1,0,0};
      for (unsigned int g=genStart; g<genEnd && g<nIgen; g++) {
        const unsigned short op=igen[g*4]|(igen[g*4+1]<<8);
        const unsigned short amt=igen[g*4+2]|(igen[g*4+3]<<8);
        switch (op) {
          case SFGEN_KEY_RANGE: z.lowKey=amt&0xff; z.highKey=(amt>>8)&0xff; break;
          case SFGEN_SAMPLE_ID: z.sampleID=amt; break;
          case SFGEN_OVERRIDING_ROOT_KEY: z.rootKeyOverride=(short)amt; break;
          case SFGEN_FINE_TUNE: z.fineTune=(short)amt; break;
          case SFGEN_COARSE_TUNE: z.coarseTune=(short)amt; break;
          default: break;
        }
      }
      if (z.sampleID>=0) it.zones.push_back(z);
    }
    insts.push_back(it);
  }

  /* -------------------------------------------------------- the presets -- */
  const size_t nPhdr=phdr.size()/38;
  const size_t nPbag=pbag.size()/4;
  const size_t nPgen=pgen.size()/4;
  for (size_t i=0; i+1<nPhdr; i++) {
    const unsigned char* b=&phdr[i*38];
    DivSoundFontPreset pr;
    pr.name=String((const char*)b,nameLen((const char*)b,20));
    pr.program=b[20]|(b[21]<<8);
    pr.bank=b[22]|(b[23]<<8);
    const unsigned int bagStart=b[24]|(b[25]<<8);
    const unsigned char* nb=&phdr[(i+1)*38];
    const unsigned int bagEnd=nb[24]|(nb[25]<<8);
    std::vector<Zone> zones;
    for (unsigned int bg=bagStart; bg<bagEnd && bg+1<nPbag; bg++) {
      const unsigned int genStart=pbag[bg*4]|(pbag[bg*4+1]<<8);
      const unsigned int genEnd=pbag[(bg+1)*4]|(pbag[(bg+1)*4+1]<<8);
      Zone z={0,127,-1,-1,-1,0,0};
      for (unsigned int g=genStart; g<genEnd && g<nPgen; g++) {
        const unsigned short op=pgen[g*4]|(pgen[g*4+1]<<8);
        const unsigned short amt=pgen[g*4+2]|(pgen[g*4+3]<<8);
        switch (op) {
          case SFGEN_KEY_RANGE: z.lowKey=amt&0xff; z.highKey=(amt>>8)&0xff; break;
          case SFGEN_INSTRUMENT: z.instrument=amt; break;
          default: break;
        }
      }
      if (z.instrument>=0) zones.push_back(z);
    }
    int zoneCount=0;
    for (size_t z=0; z<zones.size(); z++) {
      if (zones[z].instrument>=0 && zones[z].instrument<(int)insts.size()) {
        zoneCount+=(int)insts[zones[z].instrument].zones.size();
      }
    }
    pr.zones=zoneCount;
    presets.push_back(pr);
    presetZones.push_back(zones);
  }

  if (presets.empty()) {
    error="this soundfont has no presets in it";
    return false;
  }
  logI("soundfont %s: %d presets, %d instruments, %d samples",
       fontName.c_str(),(int)presets.size(),(int)insts.size(),
       (int)shdrs.size());
  return true;
}

bool DivSoundFont::loadPreset(int index, int key, DivSoundFontSample& out) {
  if (index<0 || index>=(int)presets.size()) {
    error="no such preset";
    return false;
  }
  if (key<0) key=0;
  if (key>127) key=127;

  /* WHICH RECORDING THIS KEY IS. Both levels of the format are keyed by note
     range, so a piano's low A and its high C are different files inside the
     same preset - and taking the first one gives an instrument that is in tune
     in one octave. */
  const std::vector<Zone>& pz=presetZones[index];
  const Zone* pick=NULL;
  const Zone* fallback=NULL;
  for (size_t i=0; i<pz.size(); i++) {
    if (pz[i].instrument<0 || pz[i].instrument>=(int)insts.size()) continue;
    const Inst& it=insts[pz[i].instrument];
    for (size_t z=0; z<it.zones.size(); z++) {
      const Zone& iz=it.zones[z];
      if (fallback==NULL) fallback=&iz;
      if (key>=iz.lowKey && key<=iz.highKey &&
          key>=pz[i].lowKey && key<=pz[i].highKey) {
        pick=&iz;
        break;
      }
    }
    if (pick!=NULL) break;
  }
  if (pick==NULL) pick=fallback;
  if (pick==NULL || pick->sampleID<0 || pick->sampleID>=(int)shdrs.size()) {
    error="that preset has no sample at that key";
    return false;
  }

  const Shdr& s=shdrs[pick->sampleID];
  if (s.end<=s.start || s.end>smplLen) {
    error=fmt::sprintf("sample %s is outside the soundfont PCM",
                       s.name.c_str());
    return false;
  }

  FILE* f=fopen(path.c_str(),"rb");
  if (f==NULL) {
    error="the soundfont went away";
    return false;
  }
  const unsigned int frames=s.end-s.start;
  out.data.resize(frames);
  fseek(f,smplOffset+(long)s.start*2,SEEK_SET);
  if (fread(out.data.data(),2,frames,f)!=frames) {
    fclose(f);
    error="short read on the soundfont PCM";
    return false;
  }
  fclose(f);

  out.name=s.name;
  out.rate=s.rate;
  out.rootKey=(pick->rootKeyOverride>=0)?pick->rootKeyOverride:s.rootKey;
  out.rootKey-=pick->coarseTune;
  out.fineTune=s.correction+pick->fineTune;
  if (s.loopEnd>s.loopStart && s.loopEnd<=s.end && s.loopStart>=s.start) {
    out.loopStart=(int)(s.loopStart-s.start);
    out.loopEnd=(int)(s.loopEnd-s.start);
  } else {
    out.loopStart=-1;
    out.loopEnd=-1;
  }
  return true;
}
