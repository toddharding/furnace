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

// MCP tools: instrument domain.
//
// JSON contract: the instrument JSON produced by get_instrument and consumed by
// set_instrument / update_instrument is EXACTLY the saveJSON shape emitted by
// serializeInstrument() in src/engine/fileOps/json.cpp. get_instrument achieves
// zero drift by calling e->saveJSON with only the instruments section enabled and
// returning the entry at the index (so it agrees with song_json by construction).
// set_instrument / update_instrument use the hand-written deserializer below,
// which mirrors serializeInstrument field-for-field (same key names, same value
// encodings, including serializeMacro's macro shape and the per-operator opMacro
// block). See describe_instrument_schema for the agent-facing map.
//
// deserializer coverage: EVERY feature block serializeInstrument can emit is
// applied here (fm+operators, std macros, per-operator opMacros, gb+hwSeq, c64,
// amiga sample-map, nes dpcm map, x1_010, n163, fds, multipcm, su+hwSeq, es5506,
// snes, esfm, powernoise, waveSynth, sid2, sid3+filters). Any key the serializer
// does NOT emit is rejected as an unknown key (catches agent typos) — including
// SNES `gain`, which serializeInstrument deliberately omits, so it is not part of
// this JSON contract and cannot be round-tripped through it.

#include "mcp.h"
#include "tools_common.h"
#include "../ta-log.h"

#include <string.h>
#include <vector>

using nlohmann::json;

// ---------------------------------------------------------------------------
// instrument-type name table (hand-maintained from DivInstrumentType +
// src/gui/guiConst.cpp's insTypes[][0]; the GUI table is not linkable here).

static const char* mcpInsTypeName(int t) {
  switch ((DivInstrumentType)t) {
    case DIV_INS_STD: return "SN76489/Sega PSG";
    case DIV_INS_FM: return "FM (OPN)";
    case DIV_INS_GB: return "Game Boy";
    case DIV_INS_C64: return "C64";
    case DIV_INS_AMIGA: return "Generic Sample";
    case DIV_INS_PCE: return "PC Engine";
    case DIV_INS_AY: return "AY-3-8910/SSG";
    case DIV_INS_AY8930: return "AY8930";
    case DIV_INS_TIA: return "TIA";
    case DIV_INS_SAA1099: return "SAA1099";
    case DIV_INS_VIC: return "VIC";
    case DIV_INS_PET: return "PET";
    case DIV_INS_VRC6: return "VRC6";
    case DIV_INS_OPLL: return "FM (OPLL)";
    case DIV_INS_OPL: return "FM (OPL)";
    case DIV_INS_FDS: return "FDS";
    case DIV_INS_VBOY: return "Virtual Boy";
    case DIV_INS_N163: return "Namco 163";
    case DIV_INS_SCC: return "Konami SCC/Bubble System WSG";
    case DIV_INS_OPZ: return "FM (OPZ)";
    case DIV_INS_POKEY: return "POKEY";
    case DIV_INS_BEEPER: return "Beeper";
    case DIV_INS_SWAN: return "WonderSwan";
    case DIV_INS_MIKEY: return "Atari Lynx";
    case DIV_INS_VERA: return "VERA";
    case DIV_INS_X1_010: return "X1-010";
    case DIV_INS_VRC6_SAW: return "VRC6 (saw)";
    case DIV_INS_ES5506: return "ES5506";
    case DIV_INS_MULTIPCM: return "MultiPCM/OPL4 PCM";
    case DIV_INS_SNES: return "SNES";
    case DIV_INS_SU: return "Sound Unit";
    case DIV_INS_NAMCO: return "Namco WSG";
    case DIV_INS_OPL_DRUMS: return "OPL (drums)";
    case DIV_INS_OPM: return "FM (OPM)";
    case DIV_INS_NES: return "NES";
    case DIV_INS_MSM6258: return "MSM6258";
    case DIV_INS_MSM6295: return "MSM6295";
    case DIV_INS_ADPCMA: return "ADPCM-A";
    case DIV_INS_ADPCMB: return "ADPCM-B";
    case DIV_INS_SEGAPCM: return "SegaPCM";
    case DIV_INS_QSOUND: return "QSound";
    case DIV_INS_YMZ280B: return "YMZ280B";
    case DIV_INS_RF5C68: return "RF5C68";
    case DIV_INS_MSM5232: return "MSM5232";
    case DIV_INS_T6W28: return "T6W28";
    case DIV_INS_K007232: return "K007232";
    case DIV_INS_GA20: return "GA20";
    case DIV_INS_POKEMINI: return "Pokemon Mini/QuadTone";
    case DIV_INS_SM8521: return "SM8521";
    case DIV_INS_PV1000: return "PV-1000";
    case DIV_INS_K053260: return "K053260";
    case DIV_INS_TED: return "TED";
    case DIV_INS_C140: return "C140";
    case DIV_INS_C219: return "C219";
    case DIV_INS_ESFM: return "FM (ESFM)";
    case DIV_INS_POWERNOISE: return "PowerNoise (noise)";
    case DIV_INS_POWERNOISE_SLOPE: return "PowerNoise (slope)";
    case DIV_INS_DAVE: return "Dave";
    case DIV_INS_NDS: return "Nintendo DS";
    case DIV_INS_GBA_DMA: return "GBA DMA";
    case DIV_INS_GBA_MINMOD: return "GBA MinMod";
    case DIV_INS_BIFURCATOR: return "Bifurcator";
    case DIV_INS_SID2: return "SID2";
    case DIV_INS_SUPERVISION: return "Watara Supervision";
    case DIV_INS_UPD1771C: return "NEC uPD1771C";
    case DIV_INS_SID3: return "SID3";
    default: return "Unknown";
  }
}

// ---------------------------------------------------------------------------
// typed JSON field accessors. every value read from an instrument object goes
// through one of these; on a type mismatch they throw with the field's dotted
// path so an agent sees exactly which field it botched.

[[noreturn]] static void mcpUnknownKey(const String& path) {
  throw std::runtime_error(fmt::sprintf("unknown key: %s",path));
}

static int jI(const json& v, const String& p) {
  if (v.is_boolean()) return v.get<bool>()?1:0;
  if (!v.is_number_integer() && !v.is_number_unsigned()) {
    throw std::runtime_error(fmt::sprintf("field %s must be an integer",p));
  }
  return v.get<int>();
}

static bool jB(const json& v, const String& p) {
  if (v.is_boolean()) return v.get<bool>();
  if (v.is_number_integer() || v.is_number_unsigned()) return v.get<int>()!=0;
  throw std::runtime_error(fmt::sprintf("field %s must be a boolean",p));
}

static const json& jObj(const json& v, const String& p) {
  if (!v.is_object()) throw std::runtime_error(fmt::sprintf("field %s must be an object",p));
  return v;
}

static const json& jArr(const json& v, const String& p) {
  if (!v.is_array()) throw std::runtime_error(fmt::sprintf("field %s must be an array",p));
  return v;
}

// ---------------------------------------------------------------------------
// macro (de)serialization — mirrors serializeMacro().

// resolve one operator macro by the short name serializeInstrument uses.
static DivInstrumentMacro* mcpOpMacroByName(DivInstrumentSTD::OpMacro& om, const String& n) {
  if (n=="am") return &om.amMacro;
  if (n=="ar") return &om.arMacro;
  if (n=="dr") return &om.drMacro;
  if (n=="mult") return &om.multMacro;
  if (n=="rr") return &om.rrMacro;
  if (n=="sl") return &om.slMacro;
  if (n=="tl") return &om.tlMacro;
  if (n=="dt2") return &om.dt2Macro;
  if (n=="rs") return &om.rsMacro;
  if (n=="dt") return &om.dtMacro;
  if (n=="d2r") return &om.d2rMacro;
  if (n=="ssg") return &om.ssgMacro;
  if (n=="dam") return &om.damMacro;
  if (n=="dvb") return &om.dvbMacro;
  if (n=="egt") return &om.egtMacro;
  if (n=="ksl") return &om.kslMacro;
  if (n=="sus") return &om.susMacro;
  if (n=="vib") return &om.vibMacro;
  if (n=="ws") return &om.wsMacro;
  if (n=="ksr") return &om.ksrMacro;
  return NULL;
}

// apply one serialized macro object onto a target macro (full replace of that
// macro's contents). validates every key; the "code" key is accepted (it is the
// macro identity in the serialized form) but routing is done by the caller.
static void mcpApplyMacro(DivInstrumentMacro* macro, const json& o, const String& path) {
  jObj(o,path);
  int length=0, loop=255, rel=255, dtype=0, delay=0, speed=1, jopen=0, wordSize=0;
  unsigned int mode=0;
  bool instantRel=false;
  const json* data=NULL;
  for (auto it=o.begin(); it!=o.end(); ++it) {
    const String k=it.key();
    const json& v=it.value();
    String p=path+"."+k;
    if (k=="code") { jI(v,p); }
    else if (k=="length") length=jI(v,p);
    else if (k=="loop") loop=jI(v,p);
    else if (k=="release") rel=jI(v,p);
    else if (k=="mode") mode=(unsigned int)jI(v,p);
    else if (k=="open") jopen=jI(v,p);
    else if (k=="type") dtype=jI(v,p);
    else if (k=="instantRelease") instantRel=jB(v,p);
    else if (k=="wordSize") wordSize=jI(v,p);
    else if (k=="delay") delay=jI(v,p);
    else if (k=="speed") speed=jI(v,p);
    else if (k=="data") data=&v;
    else mcpUnknownKey(p);
  }
  // reconstruct the packed "open" byte exactly the way serializeMacro decodes it:
  //   bit0 = open, bits1-2 = data type, bit3 = instantRelease, bits5-6 = wordSize
  unsigned char openByte=(unsigned char)((jopen&1)|((dtype&3)<<1)|(instantRel?(1<<3):0)|((wordSize&3)<<5));
  macro->len=(unsigned char)length;
  macro->loop=(unsigned char)loop;
  macro->rel=(unsigned char)rel;
  macro->mode=mode;
  macro->open=openByte;
  macro->delay=(unsigned char)delay;
  macro->speed=(unsigned char)speed;
  memset(macro->val,0,256*sizeof(int));
  if (data!=NULL) {
    String dp=path+".data";
    if (dtype==0) { // normal: array of values
      const json& arr=jArr(*data,dp);
      int n=(int)arr.size();
      if (n>256) n=256;
      for (int i=0; i<n; i++) macro->val[i]=jI(arr[i],dp+"[]");
    } else if (dtype==1) { // adsr
      const json& d=jObj(*data,dp);
      for (auto it=d.begin(); it!=d.end(); ++it) {
        const String k=it.key();
        String p=dp+"."+k;
        int val=jI(it.value(),p);
        if (k=="bottom") macro->val[0]=val;
        else if (k=="top") macro->val[1]=val;
        else if (k=="attack") macro->val[2]=val;
        else if (k=="hold") macro->val[3]=val;
        else if (k=="decay") macro->val[4]=val;
        else if (k=="sustain") macro->val[5]=val;
        else if (k=="susTime") macro->val[6]=val;
        else if (k=="susDecay") macro->val[7]=val;
        else if (k=="release") macro->val[8]=val;
        else mcpUnknownKey(p);
      }
    } else if (dtype==2) { // lfo
      const json& d=jObj(*data,dp);
      for (auto it=d.begin(); it!=d.end(); ++it) {
        const String k=it.key();
        String p=dp+"."+k;
        int val=jI(it.value(),p);
        if (k=="bottom") macro->val[0]=val;
        else if (k=="top") macro->val[1]=val;
        else if (k=="speed") macro->val[11]=val;
        else if (k=="shape") macro->val[12]=val;
        else if (k=="phase") macro->val[13]=val;
        else mcpUnknownKey(p);
      }
    } else {
      throw std::runtime_error(fmt::sprintf("field %s.type: unknown macro data type %d (0=normal,1=adsr,2=lfo)",path,dtype));
    }
  }
}

static void mcpClearStdMacros(DivInstrumentSTD& s) {
  for (int i=DIV_MACRO_VOL; i<=DIV_MACRO_EX10; i++) {
    DivInstrumentMacro* mm=s.macroByType((DivMacroType)i);
    if (mm!=NULL) mm->len=0;
  }
}

static void mcpClearOpMacros(DivInstrumentSTD& s) {
  for (int i=0; i<4; i++) {
    for (int j=DIV_MACRO_OP_AM; j<=DIV_MACRO_OP_KSR; j++) {
      DivInstrumentMacro* mm=s.opMacros[i].macroByType((DivMacroTypeOp)j);
      if (mm!=NULL) mm->len=0;
    }
  }
}

// ---------------------------------------------------------------------------
// feature-block appliers. each iterates the present keys of its block object,
// applies them onto `ins`, and rejects any key serializeInstrument never emits.

static void mcpApplyFMOp(DivInstrumentFM::Operator& op, const json& o, const String& path) {
  for (auto it=o.begin(); it!=o.end(); ++it) {
    const String k=it.key();
    const json& v=it.value();
    String p=path+"."+k;
    if (k=="enable") op.enable=jB(v,p);
    else if (k=="am") op.am=(unsigned char)jI(v,p);
    else if (k=="ar") op.ar=(unsigned char)jI(v,p);
    else if (k=="dr") op.dr=(unsigned char)jI(v,p);
    else if (k=="mult") op.mult=(unsigned char)jI(v,p);
    else if (k=="rr") op.rr=(unsigned char)jI(v,p);
    else if (k=="sl") op.sl=(unsigned char)jI(v,p);
    else if (k=="tl") op.tl=(unsigned char)jI(v,p);
    else if (k=="dt2") op.dt2=(unsigned char)jI(v,p);
    else if (k=="rs") op.rs=(unsigned char)jI(v,p);
    else if (k=="dt") op.dt=(unsigned char)jI(v,p);
    else if (k=="d2r") op.d2r=(unsigned char)jI(v,p);
    else if (k=="ssgEnv") op.ssgEnv=(unsigned char)jI(v,p);
    else if (k=="dam") op.dam=(unsigned char)jI(v,p);
    else if (k=="dvb") op.dvb=(unsigned char)jI(v,p);
    else if (k=="egt") op.egt=(unsigned char)jI(v,p);
    else if (k=="ksl") op.ksl=(unsigned char)jI(v,p);
    else if (k=="sus") op.sus=(unsigned char)jI(v,p);
    else if (k=="vib") op.vib=(unsigned char)jI(v,p);
    else if (k=="ws") op.ws=(unsigned char)jI(v,p);
    else if (k=="ksr") op.ksr=(unsigned char)jI(v,p);
    else if (k=="kvs") op.kvs=(unsigned char)jI(v,p);
    else mcpUnknownKey(p);
  }
}

static void mcpApplyFM(DivInstrumentFM& fm, const json& o, const String& path) {
  for (auto it=o.begin(); it!=o.end(); ++it) {
    const String k=it.key();
    const json& v=it.value();
    String p=path+"."+k;
    if (k=="alg") fm.alg=(unsigned char)jI(v,p);
    else if (k=="fb") fm.fb=(unsigned char)jI(v,p);
    else if (k=="fms") fm.fms=(unsigned char)jI(v,p);
    else if (k=="ams") fm.ams=(unsigned char)jI(v,p);
    else if (k=="fms2") fm.fms2=(unsigned char)jI(v,p);
    else if (k=="ams2") fm.ams2=(unsigned char)jI(v,p);
    else if (k=="ops") fm.ops=(unsigned char)jI(v,p);
    else if (k=="opllPreset") fm.opllPreset=(unsigned char)jI(v,p);
    else if (k=="block") fm.block=(unsigned char)jI(v,p);
    else if (k=="fixedDrums") fm.fixedDrums=jB(v,p);
    else if (k=="kickFreq") fm.kickFreq=(unsigned short)jI(v,p);
    else if (k=="snareHatFreq") fm.snareHatFreq=(unsigned short)jI(v,p);
    else if (k=="tomTopFreq") fm.tomTopFreq=(unsigned short)jI(v,p);
    else if (k=="operators") {
      const json& arr=jArr(v,p);
      int n=(int)arr.size();
      if (n>4) n=4;
      for (int i=0; i<n; i++) mcpApplyFMOp(fm.op[i],jObj(arr[i],fmt::sprintf("%s[%d]",p,i)),fmt::sprintf("%s[%d]",p,i));
    }
    else mcpUnknownKey(p);
  }
}

static void mcpApplyGB(DivInstrumentGB& gb, const json& o, const String& path) {
  for (auto it=o.begin(); it!=o.end(); ++it) {
    const String k=it.key();
    const json& v=it.value();
    String p=path+"."+k;
    if (k=="envVol") gb.envVol=(unsigned char)jI(v,p);
    else if (k=="envDir") gb.envDir=(unsigned char)jI(v,p);
    else if (k=="envLen") gb.envLen=(unsigned char)jI(v,p);
    else if (k=="soundLen") gb.soundLen=(unsigned char)jI(v,p);
    else if (k=="hwSeqLen") gb.hwSeqLen=(unsigned char)jI(v,p);
    else if (k=="softEnv") gb.softEnv=jB(v,p);
    else if (k=="alwaysInit") gb.alwaysInit=jB(v,p);
    else if (k=="doubleWave") gb.doubleWave=jB(v,p);
    else if (k=="hwSeq") {
      const json& arr=jArr(v,p);
      int n=(int)arr.size();
      if (n>256) n=256;
      for (int i=0; i<n; i++) {
        const json& seq=jObj(arr[i],fmt::sprintf("%s[%d]",p,i));
        for (auto sit=seq.begin(); sit!=seq.end(); ++sit) {
          const String sk=sit.key();
          String sp=fmt::sprintf("%s[%d].%s",p,i,sk);
          if (sk=="cmd") gb.hwSeq[i].cmd=(unsigned char)jI(sit.value(),sp);
          else if (sk=="data") gb.hwSeq[i].data=(unsigned short)jI(sit.value(),sp);
          else mcpUnknownKey(sp);
        }
      }
    }
    else mcpUnknownKey(p);
  }
}

static void mcpApplyC64(DivInstrumentC64& c64, const json& o, const String& path) {
  for (auto it=o.begin(); it!=o.end(); ++it) {
    const String k=it.key();
    const json& v=it.value();
    String p=path+"."+k;
    if (k=="waveforms") {
      const json& w=jObj(v,p);
      for (auto wit=w.begin(); wit!=w.end(); ++wit) {
        const String wk=wit.key(); String wp=p+"."+wk;
        if (wk=="tri") c64.triOn=jB(wit.value(),wp);
        else if (wk=="saw") c64.sawOn=jB(wit.value(),wp);
        else if (wk=="pulse") c64.pulseOn=jB(wit.value(),wp);
        else if (wk=="noise") c64.noiseOn=jB(wit.value(),wp);
        else mcpUnknownKey(wp);
      }
    } else if (k=="envelope") {
      const json& e=jObj(v,p);
      for (auto eit=e.begin(); eit!=e.end(); ++eit) {
        const String ek=eit.key(); String ep=p+"."+ek;
        if (ek=="attack") c64.a=(unsigned char)jI(eit.value(),ep);
        else if (ek=="decay") c64.d=(unsigned char)jI(eit.value(),ep);
        else if (ek=="sustain") c64.s=(unsigned char)jI(eit.value(),ep);
        else if (ek=="release") c64.r=(unsigned char)jI(eit.value(),ep);
        else mcpUnknownKey(ep);
      }
    } else if (k=="filter") {
      const json& f=jObj(v,p);
      for (auto fit=f.begin(); fit!=f.end(); ++fit) {
        const String fk=fit.key(); String fp=p+"."+fk;
        if (fk=="to") c64.toFilter=jB(fit.value(),fp);
        else if (fk=="init") c64.initFilter=jB(fit.value(),fp);
        else if (fk=="lowPass") c64.lp=jB(fit.value(),fp);
        else if (fk=="highPass") c64.hp=jB(fit.value(),fp);
        else if (fk=="bandPass") c64.bp=jB(fit.value(),fp);
        else if (fk=="cutoff") c64.cut=(unsigned short)jI(fit.value(),fp);
        else if (fk=="resonance") c64.res=(unsigned char)jI(fit.value(),fp);
        else if (fk=="isAbsolute") c64.filterIsAbs=jB(fit.value(),fp);
        else mcpUnknownKey(fp);
      }
    }
    else if (k=="duty") c64.duty=(unsigned short)jI(v,p);
    else if (k=="ringMod") c64.ringMod=(unsigned char)jI(v,p);
    else if (k=="oscSync") c64.oscSync=(unsigned char)jI(v,p);
    else if (k=="dutyIsAbs") c64.dutyIsAbs=jB(v,p);
    else if (k=="noTest") c64.noTest=jB(v,p);
    else if (k=="resetDuty") c64.resetDuty=jB(v,p);
    else if (k=="ch3off") c64.ch3off=jB(v,p);
    else mcpUnknownKey(p);
  }
}

static void mcpApplyAmiga(DivInstrumentAmiga& am, const json& o, const String& path) {
  for (auto it=o.begin(); it!=o.end(); ++it) {
    const String k=it.key();
    const json& v=it.value();
    String p=path+"."+k;
    if (k=="initSample") am.initSample=(short)jI(v,p);
    else if (k=="useNoteMap") am.useNoteMap=jB(v,p);
    else if (k=="useSample") am.useSample=jB(v,p);
    else if (k=="useWave") am.useWave=jB(v,p);
    else if (k=="waveLen") am.waveLen=(unsigned char)jI(v,p);
    else if (k=="sampleMap") {
      const json& arr=jArr(v,p);
      int n=(int)arr.size();
      if (n>180) n=180;
      for (int i=0; i<n; i++) {
        const json& mp=jObj(arr[i],fmt::sprintf("%s[%d]",p,i));
        for (auto mit=mp.begin(); mit!=mp.end(); ++mit) {
          const String mk=mit.key(); String mpath=fmt::sprintf("%s[%d].%s",p,i,mk);
          if (mk=="freq") am.noteMap[i].freq=jI(mit.value(),mpath);
          else if (mk=="map") am.noteMap[i].map=(short)jI(mit.value(),mpath);
          else mcpUnknownKey(mpath);
        }
      }
    }
    else mcpUnknownKey(p);
  }
}

// the "nes" block carries the DPCM sample-map fields (dpcmFreq/dpcmDelta) plus a
// redundant useNoteMap; both live on ins->amiga.
static void mcpApplyNES(DivInstrumentAmiga& am, const json& o, const String& path) {
  for (auto it=o.begin(); it!=o.end(); ++it) {
    const String k=it.key();
    const json& v=it.value();
    String p=path+"."+k;
    if (k=="useNoteMap") am.useNoteMap=jB(v,p);
    else if (k=="sampleMap") {
      const json& arr=jArr(v,p);
      int n=(int)arr.size();
      if (n>180) n=180;
      for (int i=0; i<n; i++) {
        const json& mp=jObj(arr[i],fmt::sprintf("%s[%d]",p,i));
        for (auto mit=mp.begin(); mit!=mp.end(); ++mit) {
          const String mk=mit.key(); String mpath=fmt::sprintf("%s[%d].%s",p,i,mk);
          if (mk=="dpcmFreq") am.noteMap[i].dpcmFreq=(signed char)jI(mit.value(),mpath);
          else if (mk=="dpcmDelta") am.noteMap[i].dpcmDelta=(signed char)jI(mit.value(),mpath);
          else mcpUnknownKey(mpath);
        }
      }
    }
    else mcpUnknownKey(p);
  }
}

static void mcpApplyX1(DivInstrumentX1_010& x1, const json& o, const String& path) {
  for (auto it=o.begin(); it!=o.end(); ++it) {
    const String k=it.key(); String p=path+"."+k;
    if (k=="bankSlot") x1.bankSlot=jI(it.value(),p);
    else mcpUnknownKey(p);
  }
}

static void mcpApplyN163(DivInstrumentN163& n1, const json& o, const String& path) {
  for (auto it=o.begin(); it!=o.end(); ++it) {
    const String k=it.key();
    const json& v=it.value();
    String p=path+"."+k;
    if (k=="wave") n1.wave=jI(v,p);
    else if (k=="wavePos") n1.wavePos=jI(v,p);
    else if (k=="waveLen") n1.waveLen=jI(v,p);
    else if (k=="perChanPos") n1.perChanPos=jB(v,p);
    else if (k=="wavePosCh") {
      const json& arr=jArr(v,p);
      int n=(int)arr.size(); if (n>8) n=8;
      for (int i=0; i<n; i++) n1.wavePosCh[i]=jI(arr[i],p+"[]");
    }
    else if (k=="waveLenCh") {
      const json& arr=jArr(v,p);
      int n=(int)arr.size(); if (n>8) n=8;
      for (int i=0; i<n; i++) n1.waveLenCh[i]=jI(arr[i],p+"[]");
    }
    else mcpUnknownKey(p);
  }
}

static void mcpApplyFDS(DivInstrumentFDS& fds, const json& o, const String& path) {
  for (auto it=o.begin(); it!=o.end(); ++it) {
    const String k=it.key();
    const json& v=it.value();
    String p=path+"."+k;
    if (k=="modSpeed") fds.modSpeed=jI(v,p);
    else if (k=="modDepth") fds.modDepth=jI(v,p);
    else if (k=="initModTableWithFirstWave") fds.initModTableWithFirstWave=jB(v,p);
    else if (k=="modTable") {
      const json& arr=jArr(v,p);
      int n=(int)arr.size(); if (n>32) n=32;
      for (int i=0; i<n; i++) fds.modTable[i]=(signed char)jI(arr[i],p+"[]");
    }
    else mcpUnknownKey(p);
  }
}

static void mcpApplyMultiPCM(DivInstrumentMultiPCM& mp, const json& o, const String& path) {
  for (auto it=o.begin(); it!=o.end(); ++it) {
    const String k=it.key();
    const json& v=it.value();
    String p=path+"."+k;
    if (k=="ar") mp.ar=(unsigned char)jI(v,p);
    else if (k=="d1r") mp.d1r=(unsigned char)jI(v,p);
    else if (k=="dl") mp.dl=(unsigned char)jI(v,p);
    else if (k=="d2r") mp.d2r=(unsigned char)jI(v,p);
    else if (k=="rr") mp.rr=(unsigned char)jI(v,p);
    else if (k=="rc") mp.rc=(unsigned char)jI(v,p);
    else if (k=="lfo") mp.lfo=(unsigned char)jI(v,p);
    else if (k=="vib") mp.vib=(unsigned char)jI(v,p);
    else if (k=="am") mp.am=(unsigned char)jI(v,p);
    else if (k=="damp") mp.damp=jB(v,p);
    else if (k=="pseudoReverb") mp.pseudoReverb=jB(v,p);
    else if (k=="lfoReset") mp.lfoReset=jB(v,p);
    else if (k=="levelDirect") mp.levelDirect=jB(v,p);
    else mcpUnknownKey(p);
  }
}

static void mcpApplySU(DivInstrumentSoundUnit& su, const json& o, const String& path) {
  for (auto it=o.begin(); it!=o.end(); ++it) {
    const String k=it.key();
    const json& v=it.value();
    String p=path+"."+k;
    if (k=="switchRoles") su.switchRoles=jB(v,p);
    else if (k=="hwSeqLen") su.hwSeqLen=(unsigned char)jI(v,p);
    else if (k=="hwSeq") {
      const json& arr=jArr(v,p);
      int n=(int)arr.size(); if (n>256) n=256;
      for (int i=0; i<n; i++) {
        const json& seq=jObj(arr[i],fmt::sprintf("%s[%d]",p,i));
        for (auto sit=seq.begin(); sit!=seq.end(); ++sit) {
          const String sk=sit.key(); String sp=fmt::sprintf("%s[%d].%s",p,i,sk);
          if (sk=="cmd") su.hwSeq[i].cmd=(unsigned char)jI(sit.value(),sp);
          else if (sk=="bound") su.hwSeq[i].bound=(unsigned char)jI(sit.value(),sp);
          else if (sk=="val") su.hwSeq[i].val=(unsigned char)jI(sit.value(),sp);
          else if (sk=="speed") su.hwSeq[i].speed=(unsigned short)jI(sit.value(),sp);
          else mcpUnknownKey(sp);
        }
      }
    }
    else mcpUnknownKey(p);
  }
}

static void mcpApplyES5506(DivInstrumentES5506& es, const json& o, const String& path) {
  for (auto it=o.begin(); it!=o.end(); ++it) {
    const String k=it.key();
    const json& v=it.value();
    String p=path+"."+k;
    if (k=="filter") {
      const json& f=jObj(v,p);
      for (auto fit=f.begin(); fit!=f.end(); ++fit) {
        const String fk=fit.key(); String fp=p+"."+fk;
        if (fk=="mode") es.filter.mode=(DivInstrumentES5506::Filter::FilterMode)jI(fit.value(),fp);
        else if (fk=="k1") es.filter.k1=(unsigned short)jI(fit.value(),fp);
        else if (fk=="k2") es.filter.k2=(unsigned short)jI(fit.value(),fp);
        else mcpUnknownKey(fp);
      }
    } else if (k=="envelope") {
      const json& e=jObj(v,p);
      for (auto eit=e.begin(); eit!=e.end(); ++eit) {
        const String ek=eit.key(); String ep=p+"."+ek;
        if (ek=="ecount") es.envelope.ecount=(unsigned short)jI(eit.value(),ep);
        else if (ek=="lVRamp") es.envelope.lVRamp=(signed char)jI(eit.value(),ep);
        else if (ek=="rVRamp") es.envelope.rVRamp=(signed char)jI(eit.value(),ep);
        else if (ek=="k1Ramp") es.envelope.k1Ramp=(signed char)jI(eit.value(),ep);
        else if (ek=="k2Ramp") es.envelope.k2Ramp=(signed char)jI(eit.value(),ep);
        else if (ek=="k1Slow") es.envelope.k1Slow=jB(eit.value(),ep);
        else if (ek=="k2Slow") es.envelope.k2Slow=jB(eit.value(),ep);
        else mcpUnknownKey(ep);
      }
    }
    else mcpUnknownKey(p);
  }
}

static void mcpApplySNES(DivInstrumentSNES& sn, const json& o, const String& path) {
  for (auto it=o.begin(); it!=o.end(); ++it) {
    const String k=it.key();
    const json& v=it.value();
    String p=path+"."+k;
    if (k=="useEnv") sn.useEnv=jB(v,p);
    else if (k=="sus") sn.sus=(unsigned char)jI(v,p);
    else if (k=="gainMode") sn.gainMode=(DivInstrumentSNES::GainMode)jI(v,p);
    else if (k=="a") sn.a=(unsigned char)jI(v,p);
    else if (k=="d") sn.d=(unsigned char)jI(v,p);
    else if (k=="s") sn.s=(unsigned char)jI(v,p);
    else if (k=="r") sn.r=(unsigned char)jI(v,p);
    else if (k=="d2") sn.d2=(unsigned char)jI(v,p);
    else mcpUnknownKey(p); // NOTE: `gain` is intentionally not part of the JSON contract (serializeInstrument omits it)
  }
}

static void mcpApplyESFM(DivInstrumentESFM& ef, const json& o, const String& path) {
  for (auto it=o.begin(); it!=o.end(); ++it) {
    const String k=it.key();
    const json& v=it.value();
    String p=path+"."+k;
    if (k=="noise") ef.noise=(unsigned char)jI(v,p);
    else if (k=="op") {
      const json& arr=jArr(v,p);
      int n=(int)arr.size(); if (n>4) n=4;
      for (int i=0; i<n; i++) {
        const json& op=jObj(arr[i],fmt::sprintf("%s[%d]",p,i));
        for (auto oit=op.begin(); oit!=op.end(); ++oit) {
          const String ok=oit.key(); String opth=fmt::sprintf("%s[%d].%s",p,i,ok);
          if (ok=="delay") ef.op[i].delay=(unsigned char)jI(oit.value(),opth);
          else if (ok=="outLvl") ef.op[i].outLvl=(unsigned char)jI(oit.value(),opth);
          else if (ok=="modIn") ef.op[i].modIn=(unsigned char)jI(oit.value(),opth);
          else if (ok=="left") ef.op[i].left=(unsigned char)jI(oit.value(),opth);
          else if (ok=="right") ef.op[i].right=(unsigned char)jI(oit.value(),opth);
          else if (ok=="fixed") ef.op[i].fixed=(unsigned char)jI(oit.value(),opth);
          else if (ok=="ct") ef.op[i].ct=(signed char)jI(oit.value(),opth);
          else if (ok=="dt") ef.op[i].dt=(signed char)jI(oit.value(),opth);
          else mcpUnknownKey(opth);
        }
      }
    }
    else mcpUnknownKey(p);
  }
}

static void mcpApplyPowerNoise(DivInstrumentPowerNoise& pn, const json& o, const String& path) {
  for (auto it=o.begin(); it!=o.end(); ++it) {
    const String k=it.key(); String p=path+"."+k;
    if (k=="octave") pn.octave=(unsigned char)jI(it.value(),p);
    else mcpUnknownKey(p);
  }
}

static void mcpApplyWaveSynth(DivInstrumentWaveSynth& ws, const json& o, const String& path) {
  for (auto it=o.begin(); it!=o.end(); ++it) {
    const String k=it.key();
    const json& v=it.value();
    String p=path+"."+k;
    if (k=="wave1") ws.wave1=jI(v,p);
    else if (k=="wave2") ws.wave2=jI(v,p);
    else if (k=="rateDivider") ws.rateDivider=(unsigned char)jI(v,p);
    else if (k=="effect") ws.effect=(unsigned char)jI(v,p);
    else if (k=="oneShot") ws.oneShot=jB(v,p);
    else if (k=="enabled") ws.enabled=jB(v,p);
    else if (k=="global") ws.global=jB(v,p);
    else if (k=="speed") ws.speed=(unsigned char)jI(v,p);
    else if (k=="param1") ws.param1=(unsigned char)jI(v,p);
    else if (k=="param2") ws.param2=(unsigned char)jI(v,p);
    else if (k=="param3") ws.param3=(unsigned char)jI(v,p);
    else if (k=="param4") ws.param4=(unsigned char)jI(v,p);
    else mcpUnknownKey(p);
  }
}

static void mcpApplySID2(DivInstrumentSID2& s2, const json& o, const String& path) {
  for (auto it=o.begin(); it!=o.end(); ++it) {
    const String k=it.key();
    const json& v=it.value();
    String p=path+"."+k;
    if (k=="volume") s2.volume=(unsigned char)jI(v,p);
    else if (k=="mixMode") s2.mixMode=(unsigned char)jI(v,p);
    else if (k=="noiseMode") s2.noiseMode=(unsigned char)jI(v,p);
    else mcpUnknownKey(p);
  }
}

static void mcpApplySID3Filter(DivInstrumentSID3::Filter& f, const json& o, const String& path) {
  for (auto it=o.begin(); it!=o.end(); ++it) {
    const String k=it.key();
    const json& v=it.value();
    String p=path+"."+k;
    if (k=="enable") f.enabled=jB(v,p);
    else if (k=="init") f.init=jB(v,p);
    else if (k=="absoluteCutoff") f.absoluteCutoff=jB(v,p);
    else if (k=="bindCutoffOnNote") f.bindCutoffOnNote=jB(v,p);
    else if (k=="bindCutoffToNote") f.bindCutoffToNote=jB(v,p);
    else if (k=="bindCutoffToNoteDir") f.bindCutoffToNoteDir=jB(v,p);
    else if (k=="bindResonanceOnNote") f.bindResonanceOnNote=jB(v,p);
    else if (k=="bindResonanceToNote") f.bindResonanceToNote=jB(v,p);
    else if (k=="bindResonanceToNoteDir") f.bindResonanceToNoteDir=jB(v,p);
    else if (k=="cutoff") f.cutoff=(unsigned short)jI(v,p);
    else if (k=="resonance") f.resonance=(unsigned char)jI(v,p);
    else if (k=="output_volume") f.output_volume=(unsigned char)jI(v,p);
    else if (k=="distortion_level") f.distortion_level=(unsigned char)jI(v,p);
    else if (k=="mode") f.mode=(unsigned char)jI(v,p);
    else if (k=="filter_matrix") f.filter_matrix=(unsigned char)jI(v,p);
    else if (k=="bindCutoffToNoteStrength") f.bindCutoffToNoteStrength=(unsigned char)jI(v,p);
    else if (k=="bindCutoffToNoteCenter") f.bindCutoffToNoteCenter=(unsigned char)jI(v,p);
    else if (k=="bindResonanceToNoteStrength") f.bindResonanceToNoteStrength=(unsigned char)jI(v,p);
    else if (k=="bindResonanceToNoteCenter") f.bindResonanceToNoteCenter=(unsigned char)jI(v,p);
    else mcpUnknownKey(p);
  }
}

static void mcpApplySID3(DivInstrumentSID3& s3, const json& o, const String& path) {
  for (auto it=o.begin(); it!=o.end(); ++it) {
    const String k=it.key();
    const json& v=it.value();
    String p=path+"."+k;
    if (k=="waveEnable") {
      const json& w=jObj(v,p);
      for (auto wit=w.begin(); wit!=w.end(); ++wit) {
        const String wk=wit.key(); const json& wv=wit.value(); String wp=p+"."+wk;
        if (wk=="noise") s3.noiseOn=jB(wv,wp);
        else if (wk=="pulse") s3.pulseOn=jB(wv,wp);
        else if (wk=="saw") s3.sawOn=jB(wv,wp);
        else if (wk=="tri") s3.triOn=jB(wv,wp);
        else if (wk=="special") {
          // serialized as `false` (special off) or a number (special_wave, on)
          if (wv.is_boolean()) { s3.specialWaveOn=wv.get<bool>(); }
          else { s3.specialWaveOn=true; s3.special_wave=(unsigned char)jI(wv,wp); }
        }
        else mcpUnknownKey(wp);
      }
    } else if (k=="envelope") {
      const json& e=jObj(v,p);
      for (auto eit=e.begin(); eit!=e.end(); ++eit) {
        const String ek=eit.key(); String ep=p+"."+ek;
        if (ek=="attack") s3.a=(unsigned char)jI(eit.value(),ep);
        else if (ek=="decay") s3.d=(unsigned char)jI(eit.value(),ep);
        else if (ek=="sustain") s3.s=(unsigned char)jI(eit.value(),ep);
        else if (ek=="susRate") s3.sr=(unsigned char)jI(eit.value(),ep);
        else if (ek=="release") s3.r=(unsigned char)jI(eit.value(),ep);
        else mcpUnknownKey(ep);
      }
    } else if (k=="phaseMod") {
      const json& d=jObj(v,p);
      for (auto dit=d.begin(); dit!=d.end(); ++dit) {
        const String dk=dit.key(); String dp=p+"."+dk;
        if (dk=="enable") s3.phase_mod=jB(dit.value(),dp);
        else if (dk=="source") s3.phase_mod_source=(unsigned char)jI(dit.value(),dp);
        else mcpUnknownKey(dp);
      }
    } else if (k=="ringMod") {
      const json& d=jObj(v,p);
      for (auto dit=d.begin(); dit!=d.end(); ++dit) {
        const String dk=dit.key(); String dp=p+"."+dk;
        if (dk=="enable") s3.ringMod=(unsigned char)jI(dit.value(),dp);
        else if (dk=="source") s3.ring_mod_source=(unsigned char)jI(dit.value(),dp);
        else mcpUnknownKey(dp);
      }
    } else if (k=="oscSync") {
      const json& d=jObj(v,p);
      for (auto dit=d.begin(); dit!=d.end(); ++dit) {
        const String dk=dit.key(); String dp=p+"."+dk;
        if (dk=="enable") s3.oscSync=(unsigned char)jI(dit.value(),dp);
        else if (dk=="source") s3.sync_source=(unsigned char)jI(dit.value(),dp);
        else mcpUnknownKey(dp);
      }
    }
    else if (k=="mixMode") s3.mixMode=(unsigned char)jI(v,p);
    else if (k=="duty") s3.duty=(unsigned short)jI(v,p);
    else if (k=="oneBitNoise") s3.oneBitNoise=jB(v,p);
    else if (k=="separateNoisePitch") s3.separateNoisePitch=jB(v,p);
    else if (k=="doWavetable") s3.doWavetable=jB(v,p);
    else if (k=="resetDuty") s3.resetDuty=jB(v,p);
    else if (k=="dutyIsAbs") s3.dutyIsAbs=jB(v,p);
    else if (k=="phaseInv") s3.phaseInv=(unsigned char)jI(v,p);
    else if (k=="feedback") s3.feedback=(unsigned char)jI(v,p);
    else if (k=="filters") {
      const json& arr=jArr(v,p);
      int n=(int)arr.size(); if (n>4) n=4;
      for (int i=0; i<n; i++) mcpApplySID3Filter(s3.filt[i],jObj(arr[i],fmt::sprintf("%s[%d]",p,i)),fmt::sprintf("%s[%d]",p,i));
    }
    else mcpUnknownKey(p);
  }
}

// ---------------------------------------------------------------------------
// top-level deserializer: apply a saveJSON-shape instrument object onto `ins`.
// only keys present are applied (partial merge). set_instrument feeds a default
// instrument first so absent fields fall back to defaults (full overwrite);
// update_instrument feeds a copy of the live instrument (partial merge). arrays
// (macros / opMacro / operators / sampleMap / hwSeq / filters) replace wholesale.
static void mcpApplyInstrumentJSON(DivInstrument& ins, const json& data) {
  jObj(data,"data");
  for (auto it=data.begin(); it!=data.end(); ++it) {
    const String k=it.key();
    const json& v=it.value();
    String p=k;
    if (k=="name") {
      if (!v.is_string()) throw std::runtime_error("field name must be a string");
      ins.name=v.get<String>();
    } else if (k=="type") {
      ins.type=(DivInstrumentType)jI(v,p);
    } else if (k=="macros") {
      const json& arr=jArr(v,p);
      mcpClearStdMacros(ins.std);
      for (size_t i=0; i<arr.size(); i++) {
        const json& mo=jObj(arr[i],fmt::sprintf("macros[%d]",(int)i));
        if (!mo.contains("code")) throw std::runtime_error(fmt::sprintf("macros[%d] is missing 'code' (the macro identity)",(int)i));
        int code=jI(mo["code"],fmt::sprintf("macros[%d].code",(int)i));
        if (code<DIV_MACRO_VOL || code>DIV_MACRO_EX10) throw std::runtime_error(fmt::sprintf("macros[%d].code out of range: %d (0..21)",(int)i,code));
        DivInstrumentMacro* target=ins.std.macroByType((DivMacroType)code);
        if (target==NULL) throw std::runtime_error(fmt::sprintf("macros[%d].code %d has no macro",(int)i,code));
        mcpApplyMacro(target,mo,fmt::sprintf("macros[%d]",(int)i));
      }
    } else if (k=="opMacro") {
      const json& arr=jArr(v,p);
      mcpClearOpMacros(ins.std);
      int n=(int)arr.size(); if (n>4) n=4;
      for (int i=0; i<n; i++) {
        if (arr[i].is_null()) continue; // serializer leaves gaps for ops with no macros
        const json& om=jObj(arr[i],fmt::sprintf("opMacro[%d]",i));
        for (auto oit=om.begin(); oit!=om.end(); ++oit) {
          const String mn=oit.key();
          String mp=fmt::sprintf("opMacro[%d].%s",i,mn);
          DivInstrumentMacro* target=mcpOpMacroByName(ins.std.opMacros[i],mn);
          if (target==NULL) mcpUnknownKey(mp);
          mcpApplyMacro(target,jObj(oit.value(),mp),mp);
        }
      }
    }
    else if (k=="fm") mcpApplyFM(ins.fm,jObj(v,p),p);
    else if (k=="gb") mcpApplyGB(ins.gb,jObj(v,p),p);
    else if (k=="64") mcpApplyC64(ins.c64,jObj(v,p),p);
    else if (k=="amiga") mcpApplyAmiga(ins.amiga,jObj(v,p),p);
    else if (k=="nes") mcpApplyNES(ins.amiga,jObj(v,p),p);
    else if (k=="x1_010") mcpApplyX1(ins.x1_010,jObj(v,p),p);
    else if (k=="n163") mcpApplyN163(ins.n163,jObj(v,p),p);
    else if (k=="fds") mcpApplyFDS(ins.fds,jObj(v,p),p);
    else if (k=="multipcm") mcpApplyMultiPCM(ins.multipcm,jObj(v,p),p);
    else if (k=="su") mcpApplySU(ins.su,jObj(v,p),p);
    else if (k=="es5506") mcpApplyES5506(ins.es5506,jObj(v,p),p);
    else if (k=="snes") mcpApplySNES(ins.snes,jObj(v,p),p);
    else if (k=="esfm") mcpApplyESFM(ins.esfm,jObj(v,p),p);
    else if (k=="powernoise") mcpApplyPowerNoise(ins.powernoise,jObj(v,p),p);
    else if (k=="waveSynth") mcpApplyWaveSynth(ins.ws,jObj(v,p),p);
    else if (k=="sid2") mcpApplySID2(ins.sid2,jObj(v,p),p);
    else if (k=="sid3") mcpApplySID3(ins.sid3,jObj(v,p),p);
    else mcpUnknownKey(p);
  }
}

// ---------------------------------------------------------------------------
// helpers shared by the tools

// require an instrument index in range.
static int mcpReqInsIndex(FurnaceMCP& m, const json& args) {
  int idx=mcpArgInt(args,"index");
  int n=m.engine()->song.insLen;
  if (idx<0 || idx>=n) throw std::runtime_error(fmt::sprintf("instrument index out of range: %d (have %d)",idx,n));
  return idx;
}

// get one instrument as its saveJSON-shape object by asking the engine to
// serialize only the instruments section, then picking the entry at `index`.
// this guarantees agreement with song_json by construction (zero drift).
static json mcpInstrumentJSON(FurnaceMCP& m, int index) {
  DivEngine* e=m.engine();
  DivJSONExportOptions opts;
  opts.jsonPretty=false;
  opts.exportMetadata=false;
  opts.exportChips=false;
  opts.exportOrders=false;
  opts.exportPatterns=false;
  opts.exportInstruments=true;
  opts.exportWaves=false;
  opts.exportSamples=false;
  opts.exportCompatFlags=false;
  String out=mcpWriterToString(e->saveJSON(&opts));
  json parsed=json::parse(out);
  if (!parsed.contains("instruments") || !parsed["instruments"].is_array()) {
    throw std::runtime_error("engine did not serialize instruments");
  }
  const json& arr=parsed["instruments"];
  if (index<0 || (size_t)index>=arr.size()) {
    throw std::runtime_error(fmt::sprintf("instrument index out of range: %d (have %d)",index,(int)arr.size()));
  }
  return arr[index];
}

// ---------------------------------------------------------------------------

void registerInstrumentTools(FurnaceMCP& m) {
  // -------------------------------------------------------------------------
  // list_instruments
  m.addTool(FurnaceMCPTool(
    "list_instruments",
    "List every instrument: index, name, type (integer DivInstrumentType) and typeName (human string).",
    json{{"type","object"},{"properties",json::object()}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      json list=json::array();
      e->lockEngine([&]() {
        for (int i=0; i<e->song.insLen; i++) {
          DivInstrument* ins=e->song.ins[i];
          list.push_back(json{
            {"index",i},
            {"name",ins->name},
            {"type",(int)ins->type},
            {"typeName",mcpInsTypeName((int)ins->type)}
          });
        }
      });
      return json{{"count",(int)list.size()},{"instruments",list}};
    }
  ));

  // -------------------------------------------------------------------------
  // get_instrument
  m.addTool(FurnaceMCPTool(
    "get_instrument",
    "Get one instrument as JSON in the exact saveJSON shape (agrees with song_json's instruments[index] by construction). Only the feature blocks that apply to the instrument's type are present, and only macros with length>0 are emitted. See describe_instrument_schema for the field map.",
    json{{"type","object"},{"properties",{
      {"index",{{"type","integer"},{"description","instrument index"}}}
    }},{"required",json::array({"index"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      int idx=mcpReqInsIndex(m,args);
      json ins=mcpInstrumentJSON(m,idx);
      return json{{"index",idx},{"instrument",ins}};
    }
  ));

  // -------------------------------------------------------------------------
  // set_instrument (full overwrite)
  m.addTool(FurnaceMCPTool(
    "set_instrument",
    "Full overwrite of an instrument from JSON in the saveJSON shape (the same shape get_instrument returns, under 'data'). The instrument is first reset to defaults, then every provided key is applied, so any field you omit falls back to its default. Unknown keys are a tool error naming the offending dotted path (catches typos). Nothing changes if any key is rejected. Returns the instrument re-read.",
    json{{"type","object"},{"properties",{
      {"index",{{"type","integer"},{"description","instrument index to overwrite"}}},
      {"data",{{"type","object"},{"description","full instrument in saveJSON shape"}}}
    }},{"required",json::array({"index","data"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int idx=mcpReqInsIndex(m,args);
      if (!args.contains("data") || !args["data"].is_object()) throw std::runtime_error("missing or non-object argument: data");
      // build the new instrument off-lock; a throw here leaves the live one untouched.
      DivInstrument temp;
      mcpApplyInstrumentJSON(temp,args["data"]);
      e->lockEngine([&]() {
        *e->song.ins[idx]=temp;
      });
      return json{{"index",idx},{"instrument",mcpInstrumentJSON(m,idx)}};
    }
  ));

  // -------------------------------------------------------------------------
  // update_instrument (partial merge)
  m.addTool(FurnaceMCPTool(
    "update_instrument",
    "Partial merge into an instrument: only the keys present in 'data' are applied, recursing into nested objects; arrays (macros, opMacro, operators, sampleMap, hwSeq, filters) replace wholesale. Everything not mentioned keeps its current value. Same saveJSON shape as get_instrument. Unknown keys are a tool error naming the dotted path; nothing changes if any key is rejected. Returns the instrument re-read.",
    json{{"type","object"},{"properties",{
      {"index",{{"type","integer"},{"description","instrument index to update"}}},
      {"data",{{"type","object"},{"description","partial instrument in saveJSON shape"}}}
    }},{"required",json::array({"index","data"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int idx=mcpReqInsIndex(m,args);
      if (!args.contains("data") || !args["data"].is_object()) throw std::runtime_error("missing or non-object argument: data");
      // snapshot the live instrument, merge onto the copy, then commit atomically.
      DivInstrument temp;
      e->lockEngine([&]() {
        temp=*e->song.ins[idx];
      });
      mcpApplyInstrumentJSON(temp,args["data"]); // may throw -> live instrument untouched
      e->lockEngine([&]() {
        *e->song.ins[idx]=temp;
      });
      return json{{"index",idx},{"instrument",mcpInstrumentJSON(m,idx)}};
    }
  ));

  // -------------------------------------------------------------------------
  // add_instrument
  m.addTool(FurnaceMCPTool(
    "add_instrument",
    "Create a new instrument and return its index. 'type' (integer DivInstrumentType, optional) sets the instrument type; if omitted the engine picks a default for the current chip. 'name' (optional) sets the name.",
    json{{"type","object"},{"properties",{
      {"type",{{"type","integer"},{"description","DivInstrumentType (optional)"}}},
      {"name",{{"type","string"},{"description","instrument name (optional)"}}}
    }}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      bool hasType=args.contains("type");
      int type=DIV_INS_STD;
      if (hasType) {
        type=mcpArgInt(args,"type");
        if (type<0 || type>=DIV_INS_MAX) throw std::runtime_error(fmt::sprintf("type out of range: %d (0..%d)",type,DIV_INS_MAX-1));
      }
      int idx;
      if (hasType) {
        idx=e->addInstrument(-1,(DivInstrumentType)type); // refChan<0 -> use fallbackType
      } else {
        idx=e->addInstrument();
      }
      if (idx<0) throw std::runtime_error("could not add instrument (limit reached, max 256)");
      if (args.contains("name")) {
        String nm=mcpArgStr(args,"name");
        e->lockEngine([&]() { e->song.ins[idx]->name=nm; });
      }
      return json{{"index",idx},{"type",(int)e->song.ins[idx]->type},{"typeName",mcpInsTypeName((int)e->song.ins[idx]->type)},{"name",e->song.ins[idx]->name}};
    }
  ));

  // -------------------------------------------------------------------------
  // del_instrument
  m.addTool(FurnaceMCPTool(
    "del_instrument",
    "Delete the instrument at 'index'. References to it in patterns shift down (engine handles the reindex).",
    json{{"type","object"},{"properties",{
      {"index",{{"type","integer"},{"description","instrument index to delete"}}}
    }},{"required",json::array({"index"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int idx=mcpReqInsIndex(m,args);
      e->delInstrument(idx); // self-locked
      return json{{"deleted",idx},{"count",e->song.insLen}};
    }
  ));

  // -------------------------------------------------------------------------
  // duplicate_instrument
  m.addTool(FurnaceMCPTool(
    "duplicate_instrument",
    "Deep-copy the instrument at 'index' and append the copy. Returns the new index.",
    json{{"type","object"},{"properties",{
      {"index",{{"type","integer"},{"description","instrument index to duplicate"}}}
    }},{"required",json::array({"index"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int idx=mcpReqInsIndex(m,args);
      DivInstrument* dup=new DivInstrument;
      e->lockEngine([&]() { *dup=*e->song.ins[idx]; });
      int r=e->addInstrumentPtr(dup); // self-locked; takes ownership; returns new insLen
      if (r<0) throw std::runtime_error("could not duplicate instrument (limit reached, max 256)");
      int newIdx=r-1; // addInstrumentPtr returns the new count
      return json{{"source",idx},{"index",newIdx},{"count",e->song.insLen}};
    }
  ));

  // -------------------------------------------------------------------------
  // move_instrument
  m.addTool(FurnaceMCPTool(
    "move_instrument",
    "Move the instrument at 'from' to position 'to', sliding the ones in between (the same reorder the instrument-list up/down buttons do; pattern references follow). Returns the new index.",
    json{{"type","object"},{"properties",{
      {"from",{{"type","integer"},{"description","current index"}}},
      {"to",{{"type","integer"},{"description","destination index"}}}
    }},{"required",json::array({"from","to"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int n=e->song.insLen;
      int from=mcpArgInt(args,"from");
      int to=mcpArgInt(args,"to");
      if (from<0 || from>=n) throw std::runtime_error(fmt::sprintf("from out of range: %d (have %d)",from,n));
      if (to<0 || to>=n) throw std::runtime_error(fmt::sprintf("to out of range: %d (have %d)",to,n));
      // moveInsUp/moveInsDown are self-locked; step one slot at a time.
      int cur=from;
      while (cur<to) { if (!e->moveInsDown(cur)) break; cur++; }
      while (cur>to) { if (!e->moveInsUp(cur)) break; cur--; }
      return json{{"from",from},{"to",cur}};
    }
  ));

  // -------------------------------------------------------------------------
  // export_instrument
  m.addTool(FurnaceMCPTool(
    "export_instrument",
    "Save the instrument at 'index' to a Furnace instrument file (.fui) at 'path' on disk. Wavetables/samples the instrument references are embedded.",
    json{{"type","object"},{"properties",{
      {"index",{{"type","integer"},{"description","instrument index"}}},
      {"path",{{"type","string"},{"description","destination .fui path"}}}
    }},{"required",json::array({"index","path"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int idx=mcpReqInsIndex(m,args);
      String path=mcpArgStr(args,"path");
      bool ok=false;
      e->lockEngine([&]() { ok=e->song.ins[idx]->save(path.c_str(),&e->song,true); });
      if (!ok) throw std::runtime_error(fmt::sprintf("could not save instrument to %s",path));
      return json{{"ok",true},{"index",idx},{"path",path}};
    }
  ));

  // -------------------------------------------------------------------------
  // import_instrument
  m.addTool(FurnaceMCPTool(
    "import_instrument",
    "Load instrument(s) from a file on disk (.fui and the many supported instrument formats: .dmp/.tfi/.vgi/.opli/.opni/.bnk/.gyb/.opm/.wopl/.wopn/etc). Each loaded instrument is appended. Returns the new indices.",
    json{{"type","object"},{"properties",{
      {"path",{{"type","string"},{"description","instrument file path"}}}
    }},{"required",json::array({"path"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      String path=mcpArgStr(args,"path");
      int sampleLenBefore=e->song.sampleLen;
      std::vector<DivInstrument*> loaded=e->instrumentFromFile(path.c_str(),true,true);
      if (loaded.empty()) throw std::runtime_error(fmt::sprintf("could not import instrument from %s: %s",path,e->getLastError()));
      json indices=json::array();
      for (DivInstrument* ins: loaded) {
        int r=e->addInstrumentPtr(ins); // self-locked; takes ownership; returns new count
        if (r<0) { throw std::runtime_error("could not add imported instrument (limit reached, max 256)"); }
        indices.push_back(r-1);
      }
      if (e->song.sampleLen!=sampleLenBefore) {
        e->renderSamplesP();
      }
      String warn=e->getWarnings();
      json res{{"path",path},{"count",(int)indices.size()},{"indices",indices}};
      if (!warn.empty()) res["warning"]=warn;
      return res;
    }
  ));

  // -------------------------------------------------------------------------
  // describe_instrument_schema
  m.addTool(FurnaceMCPTool(
    "describe_instrument_schema",
    "Machine-readable map of the instrument JSON (saveJSON shape) that set_instrument/update_instrument accept: which feature blocks apply to each DivInstrumentType, the std macro catalog (code + meaning), the per-operator opMacro list, and the macro object shape. Pass 'type' (integer DivInstrumentType) to get the block list for one type; omit it for the full table.",
    json{{"type","object"},{"properties",{
      {"type",{{"type","integer"},{"description","DivInstrumentType to describe (optional)"}}}
    }}},
    [](FurnaceMCP& m, const json& args) -> json {
      // block applicability per type (mirrors serializeInstrument's switch).
      // a block name maps to the top-level JSON key it produces.
      auto blocksFor=[](int t)->json {
        json b=json::array();
        auto add=[&](const char* s){ b.push_back(s); };
        bool ws=false; // whether waveSynth ("waveSynth") can apply when ins->ws.enabled
        switch ((DivInstrumentType)t) {
          case DIV_INS_FM: case DIV_INS_OPLL: case DIV_INS_OPL: case DIV_INS_OPZ:
          case DIV_INS_OPL_DRUMS: case DIV_INS_OPM:
            add("fm"); break;
          case DIV_INS_ESFM: add("fm"); add("esfm"); break;
          case DIV_INS_GB: add("gb"); ws=true; break;
          case DIV_INS_C64: add("64"); break;
          case DIV_INS_SID2: add("64"); add("sid2"); break;
          case DIV_INS_SID3: add("sid3"); add("amiga"); ws=true; break;
          case DIV_INS_AMIGA: case DIV_INS_AY: case DIV_INS_AY8930: case DIV_INS_VRC6:
          case DIV_INS_MIKEY: case DIV_INS_MSM6258: case DIV_INS_MSM6295:
          case DIV_INS_ADPCMA: case DIV_INS_ADPCMB: case DIV_INS_SEGAPCM:
          case DIV_INS_QSOUND: case DIV_INS_YMZ280B: case DIV_INS_RF5C68:
          case DIV_INS_K007232: case DIV_INS_GA20: case DIV_INS_K053260:
          case DIV_INS_C140: case DIV_INS_C219: case DIV_INS_NDS:
          case DIV_INS_GBA_DMA: case DIV_INS_GBA_MINMOD: case DIV_INS_SUPERVISION:
            add("amiga"); break;
          case DIV_INS_PCE: case DIV_INS_SWAN:
            add("amiga"); ws=true; break;
          case DIV_INS_NES: add("amiga"); add("nes"); break;
          case DIV_INS_FDS: add("fds"); ws=true; break;
          case DIV_INS_VBOY: add("fds"); ws=true; break;
          case DIV_INS_N163: add("n163"); ws=true; break;
          case DIV_INS_SCC: case DIV_INS_NAMCO: case DIV_INS_SM8521: ws=true; break;
          case DIV_INS_MULTIPCM: add("amiga"); add("multipcm"); break;
          case DIV_INS_SNES: add("amiga"); add("snes"); ws=true; break;
          case DIV_INS_SU: add("amiga"); add("su"); break;
          case DIV_INS_ES5506: add("amiga"); add("es5506"); break;
          case DIV_INS_X1_010: add("x1_010"); add("amiga"); ws=true; break;
          case DIV_INS_POWERNOISE: case DIV_INS_POWERNOISE_SLOPE: add("powernoise"); break;
          default: break; // STD/TIA/SAA/VIC/PET/etc: macros only
        }
        if (ws) add("waveSynth");
        return b;
      };

      // std macro catalog: code (DivMacroType) + short meaning.
      json macros=json::array();
      struct MDef { int code; const char* name; const char* meaning; };
      static const MDef mdefs[]={
        {0,"vol","volume / TL level"},
        {1,"arp","arpeggio (relative unless fixed)"},
        {2,"duty","duty / noise mode"},
        {3,"wave","waveform select"},
        {4,"pitch","pitch bend"},
        {5,"ex1","extra 1 (chip-specific, e.g. C64 filter mode / FM AM depth)"},
        {6,"ex2","extra 2 (chip-specific, e.g. C64 resonance / FM feedback in some)"},
        {7,"ex3","extra 3 (chip-specific)"},
        {8,"alg","FM algorithm"},
        {9,"fb","FM feedback"},
        {10,"fms","FM LFO frequency (PMS/FMS)"},
        {11,"ams","FM LFO amplitude (AMS)"},
        {12,"panLeft","panning left (or pan)"},
        {13,"panRight","panning right"},
        {14,"phaseReset","phase reset trigger"},
        {15,"ex4","extra 4 (chip-specific)"},
        {16,"ex5","extra 5 (chip-specific)"},
        {17,"ex6","extra 6 (chip-specific)"},
        {18,"ex7","extra 7 (chip-specific)"},
        {19,"ex8","extra 8 (chip-specific)"},
        {20,"ex9","extra 9 (chip-specific)"},
        {21,"ex10","extra 10 (chip-specific)"},
      };
      for (const MDef& d: mdefs) macros.push_back(json{{"code",d.code},{"name",d.name},{"meaning",d.meaning}});

      json opMacroList=json::array({
        "am","ar","dr","mult","rr","sl","tl","dt2","rs","dt","d2r","ssg",
        "dam","dvb","egt","ksl","sus","vib","ws","ksr"
      });

      json macroShape=json{
        {"code","DivMacroType identity (see macros[].code); required for entries in the top-level 'macros' array"},
        {"length","number of steps (0 = macro absent; such macros are omitted by get_instrument)"},
        {"loop","loop step, 255 = no loop"},
        {"release","release step, 255 = no release"},
        {"mode","macro mode bits (chip-specific; e.g. arp fixed/relative, ADSR/LFO flags)"},
        {"open","1 if the macro editor is open/enabled"},
        {"type","macro data encoding: 0 = normal (data is an array), 1 = ADSR (data is an object), 2 = LFO (data is an object)"},
        {"instantRelease","true = release is instant"},
        {"wordSize","0..3 storage word-size hint"},
        {"delay","initial delay in ticks"},
        {"speed","ticks per step"},
        {"data","for type 0: array of ints; type 1 (ADSR): {bottom,top,attack,hold,decay,sustain,susTime,susDecay,release}; type 2 (LFO): {bottom,top,speed,shape,phase}"}
      };

      json topLevel=json{
        {"name","instrument name (string; omitted by get_instrument when empty)"},
        {"type","DivInstrumentType (integer)"},
        {"macros","array of std macro objects; each routed by its 'code'. Replaces the whole macro set on update."},
        {"opMacro","array indexed by operator 0..3; each entry an object keyed by opMacro name (see opMacros). null entries mark operators with no macros."},
        {"featureBlocks","one nested object per applicable block for the type (see blocksByType)"}
      };

      if (args.contains("type")) {
        int t=mcpArgInt(args,"type");
        if (t<0 || t>=DIV_INS_MAX) throw std::runtime_error(fmt::sprintf("type out of range: %d (0..%d)",t,DIV_INS_MAX-1));
        return json{
          {"type",t},
          {"typeName",mcpInsTypeName(t)},
          {"blocks",blocksFor(t)},
          {"macros",macros},
          {"opMacros",opMacroList},
          {"macroShape",macroShape},
          {"topLevelKeys",topLevel}
        };
      }

      json byType=json::array();
      for (int t=0; t<DIV_INS_MAX; t++) {
        json blocks=blocksFor(t);
        byType.push_back(json{
          {"type",t},
          {"typeName",mcpInsTypeName(t)},
          {"blocks",blocks}
        });
      }
      return json{
        {"blocksByType",byType},
        {"macros",macros},
        {"opMacros",opMacroList},
        {"macroShape",macroShape},
        {"topLevelKeys",topLevel},
        {"note","macros with length 0 are omitted by get_instrument. Only blocks listed for a type are serialized; setting a block that does not apply to the type is accepted but has no effect once re-serialized. SNES `gain` is intentionally not part of this contract."}
      };
    }
  ));
}
