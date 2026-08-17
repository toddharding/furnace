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

// MCP tools: asset domain (wavetables + samples, including DSP ops).
//
// PCM encoding contract (get_sample / set_sample_data):
//   sample audio crosses the MCP boundary as base64 of raw signed 16-bit
//   little-endian mono PCM (s16le). The base64 codec here is a standard,
//   binary-safe RFC 4648 implementation local to this file: the engine's
//   taDecodeBase64 silently DROPS 0x00 bytes (baseutils.cpp), which would
//   corrupt any PCM that crosses zero, so it must not be used for audio.
//   Reads transcode 8-bit samples up to s16le (v<<8); set_sample_data always
//   (re)creates the sample as 16-bit depth. DSP ops operate on the sample's
//   stored depth (must be 8- or 16-bit, matching the GUI's sample editor).
//
// The wavetable generator and every sample DSP op mirror the math in
// src/gui/waveEdit.cpp and src/gui/sampleEdit.cpp; simplifications (only the
// FM generator) are documented at their tool.

#include "mcp.h"
#include "tools_common.h"
#include "../ta-log.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using nlohmann::json;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---------------------------------------------------------------------------
// base64 (RFC 4648, binary-safe; local to keep 0x00 bytes intact)

static const char* mcpB64Table="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static String mcpB64Encode(const unsigned char* data, size_t len) {
  String out;
  out.reserve(((len+2)/3)*4);
  for (size_t i=0; i<len; i+=3) {
    unsigned int n=((unsigned int)data[i])<<16;
    if (i+1<len) n|=((unsigned int)data[i+1])<<8;
    if (i+2<len) n|=((unsigned int)data[i+2]);
    out.push_back(mcpB64Table[(n>>18)&63]);
    out.push_back(mcpB64Table[(n>>12)&63]);
    out.push_back((i+1<len)?mcpB64Table[(n>>6)&63]:'=');
    out.push_back((i+2<len)?mcpB64Table[n&63]:'=');
  }
  return out;
}

static String mcpB64Decode(const String& in) {
  signed char rev[256];
  memset(rev,-1,sizeof(rev));
  for (int i=0; i<64; i++) rev[(unsigned char)mcpB64Table[i]]=(signed char)i;
  String out;
  out.reserve((in.size()/4)*3);
  unsigned int val=0;
  int bits=0;
  for (char c: in) {
    if (c=='=') break;
    if (c=='\n' || c=='\r' || c=='\t' || c==' ') continue;
    signed char d=rev[(unsigned char)c];
    if (d<0) throw std::runtime_error("invalid base64 input");
    val=(val<<6)|(unsigned int)d;
    bits+=6;
    if (bits>=8) {
      bits-=8;
      out.push_back((char)((val>>bits)&0xff));
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// resolvers / small helpers

static int mcpReqWaveIndex(FurnaceMCP& m, const json& args) {
  int idx=mcpArgInt(args,"index");
  int have=(int)m.engine()->song.wave.size();
  if (idx<0 || idx>=have) throw std::runtime_error(fmt::sprintf("wavetable index out of range: %d (have %d)",idx,have));
  return idx;
}

static int mcpReqSampleIndex(FurnaceMCP& m, const json& args) {
  int idx=mcpArgInt(args,"index");
  int have=(int)m.engine()->song.sample.size();
  if (idx<0 || idx>=have) throw std::runtime_error(fmt::sprintf("sample index out of range: %d (have %d)",idx,have));
  return idx;
}

static const char* mcpDepthName(DivSampleDepth d) {
  switch (d) {
    case DIV_SAMPLE_DEPTH_1BIT: return "1-bit";
    case DIV_SAMPLE_DEPTH_1BIT_DPCM: return "1-bit DPCM";
    case DIV_SAMPLE_DEPTH_YMZ_ADPCM: return "YMZ ADPCM";
    case DIV_SAMPLE_DEPTH_QSOUND_ADPCM: return "QSound ADPCM";
    case DIV_SAMPLE_DEPTH_ADPCM_A: return "ADPCM-A";
    case DIV_SAMPLE_DEPTH_ADPCM_B: return "ADPCM-B";
    case DIV_SAMPLE_DEPTH_ADPCM_K: return "K053260 ADPCM";
    case DIV_SAMPLE_DEPTH_8BIT: return "8-bit";
    case DIV_SAMPLE_DEPTH_BRR: return "BRR";
    case DIV_SAMPLE_DEPTH_VOX: return "VOX";
    case DIV_SAMPLE_DEPTH_MULAW: return "8-bit mu-law";
    case DIV_SAMPLE_DEPTH_C219: return "C219";
    case DIV_SAMPLE_DEPTH_IMA_ADPCM: return "IMA ADPCM";
    case DIV_SAMPLE_DEPTH_12BIT: return "12-bit";
    case DIV_SAMPLE_DEPTH_4BIT: return "4-bit";
    case DIV_SAMPLE_DEPTH_16BIT: return "16-bit";
    default: return "unknown";
  }
}

// build s16le mono PCM bytes for a range [from,to) of a sample. requires 8- or
// 16-bit depth; 8-bit is scaled up (v<<8) so the wire format is always s16le.
static String mcpSampleToS16LE(DivSample* s, unsigned int from, unsigned int to) {
  if (s->depth!=DIV_SAMPLE_DEPTH_8BIT && s->depth!=DIV_SAMPLE_DEPTH_16BIT) {
    throw std::runtime_error(fmt::sprintf("PCM read requires an 8- or 16-bit sample (this one is %s); convert it first",mcpDepthName(s->depth)));
  }
  String out;
  out.resize((size_t)(to-from)*2);
  size_t o=0;
  if (s->depth==DIV_SAMPLE_DEPTH_16BIT) {
    for (unsigned int i=from; i<to; i++) {
      short v=s->data16[i];
      out[o++]=(char)(v&0xff);
      out[o++]=(char)((v>>8)&0xff);
    }
  } else {
    for (unsigned int i=from; i<to; i++) {
      short v=(short)(((int)s->data8[i])<<8);
      out[o++]=(char)(v&0xff);
      out[o++]=(char)((v>>8)&0xff);
    }
  }
  return out;
}

// meta object shared by get_sample / list_samples.
static json mcpSampleMeta(DivSample* s, int index) {
  return json{
    {"index",index},
    {"name",s->name},
    {"rate",s->centerRate},
    {"centerRate",s->centerRate},
    {"legacyRate",s->legacyRate},
    {"depth",(int)s->depth},
    {"depthName",mcpDepthName(s->depth)},
    {"length",(int)s->samples},
    {"loopStart",s->loopStart},
    {"loopEnd",s->loopEnd},
    {"loop",s->loop},
    {"loopMode",(int)s->loopMode}
  };
}

// resolve/validate a [from,to) range against a sample length.
static void mcpRange(const json& args, unsigned int len, unsigned int& from, unsigned int& to) {
  int f=mcpOptInt(args,"from",0);
  int t=mcpOptInt(args,"to",(int)len);
  if (f<0 || (unsigned int)f>len) throw std::runtime_error(fmt::sprintf("'from' out of range: %d (0..%u)",f,len));
  if (t<0 || (unsigned int)t>len) throw std::runtime_error(fmt::sprintf("'to' out of range: %d (0..%u)",t,len));
  if (f>t) throw std::runtime_error("'from' must be <= 'to'");
  from=(unsigned int)f;
  to=(unsigned int)t;
}

static void mcpRequirePcmDepth(DivSample* s) {
  if (s->depth!=DIV_SAMPLE_DEPTH_8BIT && s->depth!=DIV_SAMPLE_DEPTH_16BIT) {
    throw std::runtime_error(fmt::sprintf("this DSP op requires an 8- or 16-bit sample (this one is %s)",mcpDepthName(s->depth)));
  }
}

// ---------------------------------------------------------------------------

void registerAssetTools(FurnaceMCP& m) {
  // =========================================================================
  // WAVETABLES
  // =========================================================================

  // -------------------------------------------------------------------------
  // list_wavetables
  m.addTool(FurnaceMCPTool(
    "list_wavetables",
    "List all wavetables in the song. Returns [{index, len, max}], where len is the point count and max is the peak sample value (heights run 0..max).",
    json{{"type","object"},{"properties",json::object()}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      json list=json::array();
      e->lockEngine([&]() {
        for (size_t i=0; i<e->song.wave.size(); i++) {
          DivWavetable* w=e->song.wave[i];
          list.push_back(json{{"index",(int)i},{"len",w->len},{"max",w->max}});
        }
      });
      return json{{"count",(int)list.size()},{"wavetables",list}};
    }
  ));

  // -------------------------------------------------------------------------
  // get_wavetable
  m.addTool(FurnaceMCPTool(
    "get_wavetable",
    "Read a wavetable's contents. Returns {index, len, min, max, data:[...]} where data has len entries, each 0..max.",
    json{{"type","object"},{"properties",{
      {"index",{{"type","integer"},{"description","wavetable index"}}}
    }},{"required",json::array({"index"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int idx=mcpReqWaveIndex(m,args);
      json data=json::array();
      int len=0,mn=0,mx=0;
      e->lockEngine([&]() {
        DivWavetable* w=e->song.wave[idx];
        len=w->len; mn=w->min; mx=w->max;
        for (int i=0; i<w->len; i++) data.push_back(w->data[i]);
      });
      return json{{"index",idx},{"len",len},{"min",mn},{"max",mx},{"data",data}};
    }
  ));

  // -------------------------------------------------------------------------
  // set_wavetable
  m.addTool(FurnaceMCPTool(
    "set_wavetable",
    "Replace a wavetable's point data. 'data' is an array of 1..256 integers; the new length becomes data.size(). 'max' (optional, 1..255) sets the peak value; every data point must be 0..max. If 'max' is omitted the wavetable's current max is kept.",
    json{{"type","object"},{"properties",{
      {"index",{{"type","integer"}}},
      {"data",{{"type","array"},{"description","1..256 integers, each 0..max"},{"items",{{"type","integer"}}}}},
      {"max",{{"type","integer"},{"description","peak value 1..255 (default: keep current)"}}}
    }},{"required",json::array({"index","data"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int idx=mcpReqWaveIndex(m,args);
      if (!args.contains("data") || !args["data"].is_array()) throw std::runtime_error("missing or non-array argument: data");
      const json& data=args["data"];
      int len=(int)data.size();
      if (len<1 || len>256) throw std::runtime_error(fmt::sprintf("data must have 1..256 entries (got %d)",len));
      int curMax=e->song.wave[idx]->max;
      int mx=mcpOptInt(args,"max",curMax);
      if (mx<1 || mx>255) throw std::runtime_error(fmt::sprintf("max out of range: %d (1..255)",mx));
      std::vector<int> vals;
      vals.reserve(len);
      for (const json& v: data) {
        if (!v.is_number_integer()) throw std::runtime_error("each data entry must be an integer");
        int iv=v.get<int>();
        if (iv<0 || iv>mx) throw std::runtime_error(fmt::sprintf("data value out of range: %d (0..%d)",iv,mx));
        vals.push_back(iv);
      }
      e->lockEngine([&]() {
        DivWavetable* w=e->song.wave[idx];
        w->len=len;
        w->max=mx;
        for (int i=0; i<len; i++) w->data[i]=vals[i];
      });
      e->notifyWaveChange(idx);
      return json{{"index",idx},{"len",len},{"max",mx}};
    }
  ));

  // -------------------------------------------------------------------------
  // add_wavetable
  m.addTool(FurnaceMCPTool(
    "add_wavetable",
    "Append a new wavetable and return its index. Defaults to a 32-point ramp (max 31); pass 'len' (1..256) and/or 'max' (1..255) to start it as a flat/blank wavetable of that size.",
    json{{"type","object"},{"properties",{
      {"len",{{"type","integer"},{"description","initial point count 1..256"}}},
      {"max",{{"type","integer"},{"description","initial peak value 1..255"}}}
    }}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int idx=e->addWave();
      if (idx<0) throw std::runtime_error("could not add wavetable (too many wavetables)");
      bool hasLen=args.contains("len");
      bool hasMax=args.contains("max");
      if (hasLen || hasMax) {
        int len=mcpOptInt(args,"len",e->song.wave[idx]->len);
        int mx=mcpOptInt(args,"max",e->song.wave[idx]->max);
        if (len<1 || len>256) throw std::runtime_error(fmt::sprintf("len out of range: %d (1..256)",len));
        if (mx<1 || mx>255) throw std::runtime_error(fmt::sprintf("max out of range: %d (1..255)",mx));
        e->lockEngine([&]() {
          DivWavetable* w=e->song.wave[idx];
          w->len=len;
          w->max=mx;
          for (int i=0; i<256; i++) w->data[i]=0;
        });
        e->notifyWaveChange(idx);
      }
      return json{{"index",idx},{"count",(int)e->song.wave.size()}};
    }
  ));

  // -------------------------------------------------------------------------
  // del_wavetable
  m.addTool(FurnaceMCPTool(
    "del_wavetable",
    "Delete a wavetable by index. Subsequent wavetables shift down by one.",
    json{{"type","object"},{"properties",{
      {"index",{{"type","integer"}}}
    }},{"required",json::array({"index"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int idx=mcpReqWaveIndex(m,args);
      e->delWave(idx);
      return json{{"deleted",idx},{"count",(int)e->song.wave.size()}};
    }
  ));

  // -------------------------------------------------------------------------
  // duplicate_wavetable
  m.addTool(FurnaceMCPTool(
    "duplicate_wavetable",
    "Append a copy of an existing wavetable and return the new index.",
    json{{"type","object"},{"properties",{
      {"index",{{"type","integer"}}}
    }},{"required",json::array({"index"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int src=mcpReqWaveIndex(m,args);
      int idx=e->addWave();
      if (idx<0) throw std::runtime_error("could not add wavetable (too many wavetables)");
      e->lockEngine([&]() {
        *e->song.wave[idx]=*e->song.wave[src];
      });
      e->notifyWaveChange(idx);
      return json{{"index",idx},{"source",src},{"count",(int)e->song.wave.size()}};
    }
  ));

  // -------------------------------------------------------------------------
  // export_wavetable
  m.addTool(FurnaceMCPTool(
    "export_wavetable",
    "Save a wavetable to disk as a Furnace .fuw file (DivWavetable::save; header '-Furnace waveta-').",
    json{{"type","object"},{"properties",{
      {"index",{{"type","integer"}}},
      {"path",{{"type","string"},{"description","destination .fuw path"}}}
    }},{"required",json::array({"index","path"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int idx=mcpReqWaveIndex(m,args);
      String path=mcpArgStr(args,"path");
      bool ok=false;
      e->lockEngine([&]() {
        ok=e->song.wave[idx]->save(path.c_str());
      });
      if (!ok) throw std::runtime_error(fmt::sprintf("could not save wavetable to %s",path));
      return json{{"ok",true},{"index",idx},{"path",path}};
    }
  ));

  // -------------------------------------------------------------------------
  // import_wavetable
  m.addTool(FurnaceMCPTool(
    "import_wavetable",
    "Load a wavetable from disk (.fuw / .dmw, or a raw dump) via the engine's waveFromFile, append it, and return the new index.",
    json{{"type","object"},{"properties",{
      {"path",{{"type","string"},{"description","source wavetable file path"}}}
    }},{"required",json::array({"path"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      String path=mcpArgStr(args,"path");
      DivWavetable* w=e->waveFromFile(path.c_str());
      if (w==NULL) throw std::runtime_error(fmt::sprintf("could not load wavetable from %s: %s",path,e->getLastError()));
      // NOTE: addWavePtr returns the new wave COUNT (waveLen), not the index,
      // and deletes 'w' itself on failure (returns -1). The new index is ret-1.
      int ret=e->addWavePtr(w);
      if (ret<0) throw std::runtime_error("could not add wavetable (too many wavetables)");
      int idx=ret-1;
      return json{{"index",idx},{"path",path},{"count",(int)e->song.wave.size()}};
    }
  ));

  // -------------------------------------------------------------------------
  // generate_wavetable
  m.addTool(FurnaceMCPTool(
    "generate_wavetable",
    "Procedurally fill a wavetable, mirroring the Wavetable Editor's generator (src/gui/waveEdit.cpp).\n"
    "Base-shape mode (default): shape is 'sine'|'triangle'|'saw'|'pulse'. 'duty' (0..1, default 0.5) sets the pulse duty. "
    "'harmonics' is an additive partial list [{amp, phase}, ...] (up to 16; partial j uses harmonic number j+1, phase in wave cycles); "
    "if omitted a single unit fundamental is used, giving the pure shape. The summed waveform is mapped to 0..max exactly as the GUI does "
    "((1+x)/2 then round to max).\n"
    "FM mode: pass 'fm' = {ops:[{ratio, level, feedback?}, ...]} (1..8 sine operators). SIMPLIFIED vs the GUI: this is a self-contained "
    "linear modulation chain of pure sine operators — ops[0] is the carrier, each later op phase-modulates the previous one; 'level' is the "
    "carrier amplitude / modulator index and 'feedback' feeds an op's own last output back into its phase. It does NOT reproduce the GUI's "
    "4-operator selectable-waveform connection matrix.\n"
    "Post-transforms (applied in this order, mirroring the editor buttons): invert (bool, phase-inverts the whole wave before mapping), "
    "reverse, half (squash to first half), double (2x repeat), randomize (replaces data with noise), normalize (stretch min..max to 0..max).",
    json{{"type","object"},{"properties",{
      {"index",{{"type","integer"}}},
      {"shape",{{"type","string"},{"enum",json::array({"sine","triangle","saw","pulse"})}}},
      {"duty",{{"type","number"},{"description","pulse duty 0..1 (default 0.5)"}}},
      {"harmonics",{{"type","array"},{"description","additive partials [{amp,phase}...], up to 16"},{"items",{{"type","object"}}}}},
      {"fm",{{"type","object"},{"description","{ops:[{ratio,level,feedback?}...]} simplified sine FM"}}},
      {"invert",{{"type","boolean"}}},
      {"reverse",{{"type","boolean"}}},
      {"half",{{"type","boolean"}}},
      {"double",{{"type","boolean"}}},
      {"randomize",{{"type","boolean"}}},
      {"normalize",{{"type","boolean"}}}
    }},{"required",json::array({"index"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int idx=mcpReqWaveIndex(m,args);

      String shape=mcpOptStr(args,"shape","sine");
      int shapeId=-1;
      if (shape=="sine") shapeId=0;
      else if (shape=="triangle") shapeId=1;
      else if (shape=="saw") shapeId=2;
      else if (shape=="pulse") shapeId=3;
      else throw std::runtime_error(fmt::sprintf("unknown shape: %s (sine|triangle|saw|pulse)",shape));

      double duty=mcpOptFloat(args,"duty",0.5);
      if (duty<0.0 || duty>1.0) throw std::runtime_error("duty must be 0..1");

      // additive partials (base-shape mode)
      double amp[16], phase[16];
      for (int j=0; j<16; j++) { amp[j]=0.0; phase[j]=0.0; }
      bool hasHarmonics=args.contains("harmonics");
      if (hasHarmonics) {
        if (!args["harmonics"].is_array()) throw std::runtime_error("harmonics must be an array");
        const json& hs=args["harmonics"];
        if (hs.size()>16) throw std::runtime_error("at most 16 harmonics are supported");
        for (size_t j=0; j<hs.size(); j++) {
          const json& h=hs[j];
          if (!h.is_object()) throw std::runtime_error("each harmonic must be an object {amp, phase}");
          amp[j]=h.contains("amp")?h["amp"].get<double>():0.0;
          phase[j]=h.contains("phase")?h["phase"].get<double>():0.0;
        }
      } else {
        amp[0]=1.0; // pure fundamental
      }

      // FM operators (simplified)
      bool useFM=args.contains("fm");
      std::vector<double> fmRatio, fmLevel, fmFeedback;
      if (useFM) {
        const json& fm=args["fm"];
        if (!fm.is_object() || !fm.contains("ops") || !fm["ops"].is_array()) throw std::runtime_error("fm must be {ops:[{ratio,level,feedback?}...]}");
        const json& ops=fm["ops"];
        if (ops.empty() || ops.size()>8) throw std::runtime_error("fm.ops must have 1..8 operators");
        for (const json& op: ops) {
          if (!op.is_object()) throw std::runtime_error("each fm op must be an object");
          fmRatio.push_back(op.contains("ratio")?op["ratio"].get<double>():1.0);
          fmLevel.push_back(op.contains("level")?op["level"].get<double>():1.0);
          fmFeedback.push_back(op.contains("feedback")?op["feedback"].get<double>():0.0);
        }
      }

      bool doInvert=mcpOptBool(args,"invert",false);
      bool doReverse=mcpOptBool(args,"reverse",false);
      bool doHalf=mcpOptBool(args,"half",false);
      bool doDouble=mcpOptBool(args,"double",false);
      bool doRandomize=mcpOptBool(args,"randomize",false);
      bool doNormalize=mcpOptBool(args,"normalize",false);

      int len=0,mx=0;
      e->lockEngine([&]() {
        DivWavetable* w=e->song.wave[idx];
        len=w->len; mx=w->max;
        if (len<2) return;

        double finalResult[256];
        memset(finalResult,0,sizeof(finalResult));

        if (useFM) {
          int n=(int)fmRatio.size();
          double fbMem[8]={0,0,0,0,0,0,0,0};
          for (int i=0; i<len; i++) {
            double pos=(double)i/(double)len;
            double mod=0.0;
            for (int k=n-1; k>=0; k--) {
              double ph=2.0*M_PI*fmRatio[k]*pos+mod;
              if (fmFeedback[k]!=0.0) ph+=fmFeedback[k]*fbMem[k];
              double s=sin(ph);
              fbMem[k]=s;
              if (k==0) {
                finalResult[i]=s*fmLevel[k];
              } else {
                mod=s*fmLevel[k]*2.0*M_PI;
              }
            }
          }
        } else {
          for (int i=0; i<len; i++) {
            for (int j=0; j<16; j++) {
              if (amp[j]==0.0) continue;
              double pos=fmod((phase[j]*len)+(i*(j+1)),(double)len);
              double partial=0.0;
              switch (shapeId) {
                case 0: partial=sin((0.5+pos)*2.0*M_PI/(double)len); break;
                case 1: partial=4.0*(0.5-fabs(0.5-(pos/(double)(len-1))))-1.0; break;
                case 2: partial=((2*pos)/(double)(len-1))-1.0; break;
                case 3: partial=(pos>=(duty*len))?1.0:-1.0; break;
              }
              partial*=amp[j];
              finalResult[i]+=partial;
            }
          }
        }

        // phase inversion (GUI invert-point at 0 => whole wave)
        if (doInvert) {
          for (int i=0; i<len; i++) finalResult[i]=-finalResult[i];
        }

        // map to 0..max exactly as waveEdit.cpp
        for (int i=0; i<len; i++) {
          double v=(1.0+finalResult[i])*0.5;
          if (v<0.0) v=0.0;
          if (v>1.0) v=1.0;
          w->data[i]=(int)round(v*mx);
        }

        // post-transforms (mirror the editor buttons)
        if (doReverse) {
          int orig[256];
          memcpy(orig,w->data,len*sizeof(int));
          for (int i=0; i<len; i++) w->data[i]=orig[len-1-i];
        }
        if (doHalf) {
          int orig[256];
          memcpy(orig,w->data,len*sizeof(int));
          for (int i=0; i<len; i++) w->data[i]=orig[i>>1];
        }
        if (doDouble) {
          int orig[256];
          memcpy(orig,w->data,len*sizeof(int));
          for (int i=0; i<len; i++) w->data[i]=orig[(i*2)%len];
        }
        if (doRandomize && mx>0) {
          for (int i=0; i<len; i++) w->data[i]=rand()%(mx+1);
        }
        if (doNormalize) {
          int lowest=mx, highest=0;
          for (int i=0; i<len; i++) {
            if (w->data[i]<lowest) lowest=w->data[i];
            if (w->data[i]>highest) highest=w->data[i];
          }
          if (lowest!=highest && !(lowest==mx && highest==0)) {
            for (int i=0; i<len; i++) w->data[i]-=lowest;
            highest-=lowest;
            for (int i=0; i<len; i++) w->data[i]=(w->data[i]*mx)/highest;
          }
        }
      });
      if (len<2) throw std::runtime_error("wavetable length must be at least 2 to generate");
      e->notifyWaveChange(idx);

      // read back
      json data=json::array();
      e->lockEngine([&]() {
        DivWavetable* w=e->song.wave[idx];
        for (int i=0; i<w->len; i++) data.push_back(w->data[i]);
      });
      return json{{"index",idx},{"len",len},{"max",mx},{"mode",useFM?"fm":"shape"},{"data",data}};
    }
  ));

  // =========================================================================
  // SAMPLES
  // =========================================================================

  // -------------------------------------------------------------------------
  // list_samples
  m.addTool(FurnaceMCPTool(
    "list_samples",
    "List all samples. Returns [{index, name, rate, centerRate, legacyRate, depth, depthName, length, loopStart, loopEnd, loop, loopMode}]. 'rate' aliases centerRate (the sample's playback/center rate); length is in sample frames.",
    json{{"type","object"},{"properties",json::object()}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      json list=json::array();
      e->lockEngine([&]() {
        for (size_t i=0; i<e->song.sample.size(); i++) {
          list.push_back(mcpSampleMeta(e->song.sample[i],(int)i));
        }
      });
      return json{{"count",(int)list.size()},{"samples",list}};
    }
  ));

  // -------------------------------------------------------------------------
  // get_sample
  m.addTool(FurnaceMCPTool(
    "get_sample",
    "Get a sample's metadata, and optionally its PCM. With includeData=true the response adds pcm_base64 (base64 of signed 16-bit little-endian mono PCM; 8-bit samples are scaled up to 16-bit) and sampleCount. PCM readout requires an 8- or 16-bit sample.",
    json{{"type","object"},{"properties",{
      {"index",{{"type","integer"}}},
      {"includeData",{{"type","boolean"},{"description","include pcm_base64 (default false)"}}}
    }},{"required",json::array({"index"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int idx=mcpReqSampleIndex(m,args);
      bool includeData=mcpOptBool(args,"includeData",false);
      json out;
      String pcm;
      unsigned int count=0;
      e->lockEngine([&]() {
        DivSample* s=e->song.sample[idx];
        out=mcpSampleMeta(s,idx);
        if (includeData) {
          count=s->samples;
          String raw=mcpSampleToS16LE(s,0,s->samples);
          pcm=mcpB64Encode((const unsigned char*)raw.data(),raw.size());
        }
      });
      if (includeData) {
        out["sampleCount"]=(int)count;
        out["pcm_base64"]=pcm;
        out["pcmFormat"]="s16le-mono";
      }
      return out;
    }
  ));

  // -------------------------------------------------------------------------
  // set_sample_data
  m.addTool(FurnaceMCPTool(
    "set_sample_data",
    "Replace a sample's audio with raw PCM. pcm_base64 is base64 of signed 16-bit little-endian mono PCM (byte length must be even). The sample is (re)created as 16-bit depth with sampleCount = bytes/2. 'rate' (optional) sets centerRate. Loop points beyond the new length are clamped.",
    json{{"type","object"},{"properties",{
      {"index",{{"type","integer"}}},
      {"pcm_base64",{{"type","string"},{"description","base64 of s16le mono PCM"}}},
      {"rate",{{"type","integer"},{"description","new centerRate in Hz (optional)"}}}
    }},{"required",json::array({"index","pcm_base64"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int idx=mcpReqSampleIndex(m,args);
      String b64=mcpArgStr(args,"pcm_base64");
      String bytes=mcpB64Decode(b64);
      if (bytes.size()%2!=0) throw std::runtime_error("pcm byte length must be even (s16le)");
      unsigned int count=(unsigned int)(bytes.size()/2);
      bool hasRate=args.contains("rate");
      int rate=mcpOptInt(args,"rate",0);
      if (hasRate && (rate<1 || rate>384000)) throw std::runtime_error(fmt::sprintf("rate out of range: %d (1..384000)",rate));

      bool ok=true;
      e->lockEngine([&]() {
        DivSample* s=e->song.sample[idx];
        s->depth=DIV_SAMPLE_DEPTH_16BIT;
        if (!s->init(count)) { ok=false; return; }
        const unsigned char* b=(const unsigned char*)bytes.data();
        for (unsigned int i=0; i<count; i++) {
          s->data16[i]=(short)((unsigned short)b[i*2]|((unsigned short)b[i*2+1]<<8));
        }
        if (hasRate) s->centerRate=rate;
        if (s->loopStart>(int)count) s->loopStart=count;
        if (s->loopEnd>(int)count) s->loopEnd=count;
        e->renderSamples(idx);
      });
      if (!ok) throw std::runtime_error("could not initialize sample data");
      e->notifySampleChange(idx);
      e->notifyPitchTable();
      return json{{"index",idx},{"sampleCount",(int)count},{"depth",16}};
    }
  ));

  // -------------------------------------------------------------------------
  // set_sample_props
  m.addTool(FurnaceMCPTool(
    "set_sample_props",
    "Set sample properties (only provided fields change). name (string); rate/centerRate (1..384000, aliases — the sample's center rate); loopStart, loopEnd (-1 to clear, else 0..length); loop (bool, enables looping); loopMode (0=forward,1=backward,2=pingpong); depth (8 or 16 — converts the audio via DivSample::convert). Returns the updated metadata.",
    json{{"type","object"},{"properties",{
      {"index",{{"type","integer"}}},
      {"name",{{"type","string"}}},
      {"rate",{{"type","integer"},{"description","center rate 1..384000 (alias of centerRate)"}}},
      {"centerRate",{{"type","integer"},{"description","center rate 1..384000"}}},
      {"loopStart",{{"type","integer"}}},
      {"loopEnd",{{"type","integer"}}},
      {"loop",{{"type","boolean"}}},
      {"loopMode",{{"type","integer"},{"description","0 forward, 1 backward, 2 pingpong"}}},
      {"depth",{{"type","integer"},{"description","8 or 16"}}}
    }},{"required",json::array({"index"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int idx=mcpReqSampleIndex(m,args);
      DivSample* s=e->song.sample[idx];
      int len=(int)s->samples;

      // validate everything up front
      bool hasName=args.contains("name");
      String name=hasName?mcpArgStr(args,"name"):"";

      bool hasRate=args.contains("rate")||args.contains("centerRate");
      int rate=s->centerRate;
      if (args.contains("centerRate")) rate=mcpArgInt(args,"centerRate");
      else if (args.contains("rate")) rate=mcpArgInt(args,"rate");
      if (hasRate && (rate<1 || rate>384000)) throw std::runtime_error(fmt::sprintf("rate out of range: %d (1..384000)",rate));

      bool hasLS=args.contains("loopStart");
      int ls=hasLS?mcpArgInt(args,"loopStart"):s->loopStart;
      if (hasLS && (ls<-1 || ls>len)) throw std::runtime_error(fmt::sprintf("loopStart out of range: %d (-1..%d)",ls,len));
      bool hasLE=args.contains("loopEnd");
      int le=hasLE?mcpArgInt(args,"loopEnd"):s->loopEnd;
      if (hasLE && (le<-1 || le>len)) throw std::runtime_error(fmt::sprintf("loopEnd out of range: %d (-1..%d)",le,len));

      bool hasLoop=args.contains("loop");
      bool loop=mcpOptBool(args,"loop",s->loop);

      bool hasLM=args.contains("loopMode");
      int lm=hasLM?mcpArgInt(args,"loopMode"):(int)s->loopMode;
      if (hasLM && (lm<0 || lm>2)) throw std::runtime_error(fmt::sprintf("loopMode out of range: %d (0..2)",lm));

      bool hasDepth=args.contains("depth");
      int depth=hasDepth?mcpArgInt(args,"depth"):(int)s->depth;
      if (hasDepth && depth!=8 && depth!=16) throw std::runtime_error("depth must be 8 or 16");

      e->lockEngine([&]() {
        if (hasName) s->name=name;
        if (hasRate) s->centerRate=rate;
        if (hasLS) s->loopStart=ls;
        if (hasLE) s->loopEnd=le;
        if (hasLoop) s->loop=loop;
        if (hasLM) s->loopMode=(DivSampleLoopMode)lm;
        if (hasDepth && depth!=(int)s->depth) {
          s->convert(depth==8?DIV_SAMPLE_DEPTH_8BIT:DIV_SAMPLE_DEPTH_16BIT);
        }
        e->renderSamples(idx);
      });
      e->notifySampleChange(idx);
      e->notifyPitchTable();

      json out;
      e->lockEngine([&]() { out=mcpSampleMeta(s,idx); });
      return out;
    }
  ));

  // -------------------------------------------------------------------------
  // add_sample
  m.addTool(FurnaceMCPTool(
    "add_sample",
    "Append a new empty 16-bit sample and return its index. Fill it with set_sample_data.",
    json{{"type","object"},{"properties",json::object()}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int idx=e->addSample();
      if (idx<0) throw std::runtime_error("could not add sample (too many samples)");
      e->notifyPitchTable();
      return json{{"index",idx},{"count",(int)e->song.sample.size()}};
    }
  ));

  // -------------------------------------------------------------------------
  // del_sample
  m.addTool(FurnaceMCPTool(
    "del_sample",
    "Delete a sample by index. Subsequent samples shift down by one.",
    json{{"type","object"},{"properties",{
      {"index",{{"type","integer"}}}
    }},{"required",json::array({"index"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int idx=mcpReqSampleIndex(m,args);
      e->delSample(idx);
      e->notifyPitchTable();
      return json{{"deleted",idx},{"count",(int)e->song.sample.size()}};
    }
  ));

  // -------------------------------------------------------------------------
  // duplicate_sample
  m.addTool(FurnaceMCPTool(
    "duplicate_sample",
    "Append a copy of an existing sample (data + properties) and return the new index.",
    json{{"type","object"},{"properties",{
      {"index",{{"type","integer"}}}
    }},{"required",json::array({"index"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int src=mcpReqSampleIndex(m,args);
      DivSample* prev=e->song.sample[src];
      int idx=e->addSample();
      if (idx<0) throw std::runtime_error("could not add sample (too many samples)");
      bool ok=true;
      e->lockEngine([&]() {
        DivSample* s=e->song.sample[idx];
        s->centerRate=prev->centerRate;
        s->name=prev->name;
        s->loopStart=prev->loopStart;
        s->loopEnd=prev->loopEnd;
        s->loop=prev->loop;
        s->loopMode=prev->loopMode;
        s->brrEmphasis=prev->brrEmphasis;
        s->brrNoFilter=prev->brrNoFilter;
        s->dither=prev->dither;
        s->depth=prev->depth;
        if (s->init(prev->samples)) {
          if (prev->getCurBuf()!=NULL && s->getCurBuf()!=NULL) {
            memcpy(s->getCurBuf(),prev->getCurBuf(),prev->getCurBufLen());
          }
        } else {
          ok=false;
        }
        e->renderSamples(idx);
      });
      if (!ok) throw std::runtime_error("could not initialize duplicate sample");
      e->notifySampleChange(idx);
      e->notifyPitchTable();
      return json{{"index",idx},{"source",src},{"count",(int)e->song.sample.size()}};
    }
  ));

  // -------------------------------------------------------------------------
  // import_sample
  m.addTool(FurnaceMCPTool(
    "import_sample",
    "Load a sample from an audio file on disk and append it, returning the new index. Normally decodes via the engine's sampleFromFile (WAV/AIFF/etc.); if the file yields multiple samples, only the first is imported. Pass 'raw' = {depth, channels, rate, signed?, bigEndian?, swapNibbles?} to interpret the file as headerless raw data via sampleFromFileRaw.",
    json{{"type","object"},{"properties",{
      {"path",{{"type","string"}}},
      {"raw",{{"type","object"},{"description","{depth, channels, rate, signed?, bigEndian?, swapNibbles?}"}}}
    }},{"required",json::array({"path"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      String path=mcpArgStr(args,"path");
      int idx=-1;
      if (args.contains("raw")) {
        const json& r=args["raw"];
        if (!r.is_object()) throw std::runtime_error("raw must be an object");
        int depth=r.contains("depth")?r["depth"].get<int>():16;
        int channels=r.contains("channels")?r["channels"].get<int>():1;
        int rate=r.contains("rate")?r["rate"].get<int>():44100;
        bool isSigned=r.contains("signed")?r["signed"].get<bool>():true;
        bool bigEndian=r.contains("bigEndian")?r["bigEndian"].get<bool>():false;
        bool swapNibbles=r.contains("swapNibbles")?r["swapNibbles"].get<bool>():false;
        if (depth<0 || depth>=(int)DIV_SAMPLE_DEPTH_MAX) throw std::runtime_error("raw.depth is not a valid DivSampleDepth");
        // sampleFromFileRaw(path, depth, channels, bigEndian, unsign, swapNibbles, rate)
        DivSample* s=e->sampleFromFileRaw(path.c_str(),(DivSampleDepth)depth,channels,bigEndian,!isSigned,swapNibbles,rate);
        if (s==NULL) throw std::runtime_error(fmt::sprintf("could not load raw sample from %s: %s",path,e->getLastError()));
        // addSamplePtr returns the new index, and deletes 's' itself on failure.
        idx=e->addSamplePtr(s);
        if (idx<0) throw std::runtime_error("could not add sample (too many samples)");
      } else {
        std::vector<DivSample*> samples=e->sampleFromFile(path.c_str());
        if (samples.empty()) throw std::runtime_error(fmt::sprintf("could not load sample from %s: %s",path,e->getLastError()));
        // free any extra samples we did not adopt
        for (size_t i=1; i<samples.size(); i++) delete samples[i];
        // addSamplePtr returns the new index, and deletes samples[0] on failure.
        idx=e->addSamplePtr(samples[0]);
        if (idx<0) throw std::runtime_error("could not add sample (too many samples)");
      }
      e->notifyPitchTable();
      return json{{"index",idx},{"path",path},{"count",(int)e->song.sample.size()}};
    }
  ));

  // -------------------------------------------------------------------------
  // export_sample
  m.addTool(FurnaceMCPTool(
    "export_sample",
    "Save a sample to disk as a WAV file (DivSample::save via libsndfile; 8-bit -> PCM_U8, otherwise PCM_16). Requires the sample to have 16-bit data rendered (true for 8/16-bit samples).",
    json{{"type","object"},{"properties",{
      {"index",{{"type","integer"}}},
      {"path",{{"type","string"},{"description","destination .wav path"}}}
    }},{"required",json::array({"index","path"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int idx=mcpReqSampleIndex(m,args);
      String path=mcpArgStr(args,"path");
      bool ok=false;
      e->lockEngine([&]() {
        ok=e->song.sample[idx]->save(path.c_str());
      });
      if (!ok) throw std::runtime_error(fmt::sprintf("could not save sample to %s (is Furnace built with libsndfile, and is the sample 8/16-bit?)",path));
      return json{{"ok",true},{"index",idx},{"path",path}};
    }
  ));

  // -------------------------------------------------------------------------
  // sample_dsp
  m.addTool(FurnaceMCPTool(
    "sample_dsp",
    "Apply an in-place DSP operation to a sample, mirroring the Sample Editor (src/gui/sampleEdit.cpp). All ops require an 8- or 16-bit sample. Most ops act on a frame range [from,to) (defaults: whole sample). Ops and their params:\n"
    "  amplify: volume (percent, default 100), offset (DC offset percent -100..100, default 0)\n"
    "  normalize: (range) scale peak to full scale\n"
    "  fade_in / fade_out: (range) linear fade\n"
    "  silence: (range) zero the range\n"
    "  insert_silence: at (frame, default end), length (frames) — inserts silence, lengthening the sample\n"
    "  trim: (range) keep only [from,to), discard the rest\n"
    "  resample: to_rate (target Hz, required), filter (0=none,1=linear,2=cubic,3=blep,4=sinc,5=best; default 1) — resamples audio from the current centerRate; centerRate is left unchanged (mirrors the GUI)\n"
    "  reverse / invert / sign_flip: (range) reverse order / negate / flip sign bit\n"
    "  filter: (range) state-variable filter. cutoff (Hz, default centerRate/4), cutoffEnd (Hz, for sweep), sweep (bool), resonance (0..0.99, default 0), power (1..3, default 1), low/band/high (mix 0..1; default low=1)\n"
    "  crossfade_loop: length (frames), law (0=linear..100=equal-power, default 0) — blends pre-loop audio into the loop tail; requires loopStart/loopEnd set\n"
    "  trim_side_noise: (range) threshold (dBFS -144..0, default -60) — cuts leading/trailing noise floor, keeping a decaying tail (128-frame window, quarter must exceed threshold); reports trimmedStart/trimmedEnd\n"
    "  tune_loop: target (0=Amiga 1=SNES 2=Namco C219 3=NDS 16-bit 4=NDS 8-bit 5=NDS IMA 6=GBA DMA, default 0), filter (resample filter, default 1) — resamples so the loop length hits the chip's block alignment, then snaps both loop points onto it; this is the fix for a looped sample that clicks on hardware. Changes centerRate; reports the new centerRate/loopStart/loopEnd",
    json{{"type","object"},{"properties",{
      {"index",{{"type","integer"}}},
      {"op",{{"type","string"},{"enum",json::array({"amplify","normalize","fade_in","fade_out","silence","insert_silence","trim","resample","reverse","invert","sign_flip","filter","crossfade_loop","trim_side_noise","tune_loop"})}}},
      {"from",{{"type","integer"},{"description","range start frame (default 0)"}}},
      {"to",{{"type","integer"},{"description","range end frame exclusive (default length)"}}},
      {"volume",{{"type","number"}}},
      {"offset",{{"type","number"}}},
      {"at",{{"type","integer"}}},
      {"length",{{"type","integer"}}},
      {"to_rate",{{"type","number"}}},
      {"filter",{{"type","integer"}}},
      {"cutoff",{{"type","number"}}},
      {"cutoffEnd",{{"type","number"}}},
      {"sweep",{{"type","boolean"}}},
      {"resonance",{{"type","number"}}},
      {"power",{{"type","integer"}}},
      {"low",{{"type","number"}}},
      {"band",{{"type","number"}}},
      {"high",{{"type","number"}}},
      {"law",{{"type","integer"}}},
      {"threshold",{{"type","number"},{"description","trim_side_noise: noise floor in dBFS (-144..0)"}}},
      {"target",{{"type","integer"},{"description","tune_loop: chip alignment target 0..6"}}}
    }},{"required",json::array({"index","op"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int idx=mcpReqSampleIndex(m,args);
      String op=mcpArgStr(args,"op");
      DivSample* s=e->song.sample[idx];
      mcpRequirePcmDepth(s);

      bool is16=(s->depth==DIV_SAMPLE_DEPTH_16BIT);
      json result{{"index",idx},{"op",op}};
      s->prepareUndo(true);

      if (op=="amplify") {
        unsigned int from,to; mcpRange(args,s->samples,from,to);
        double vol=mcpOptFloat(args,"volume",100.0)/100.0;
        double offPct=mcpOptFloat(args,"offset",0.0);
        e->lockEngine([&]() {
          if (is16) {
            double off=32767.0*(offPct/100.0);
            for (unsigned int i=from; i<to; i++) {
              double v=off+s->data16[i]*vol;
              if (v<-32768) v=-32768; if (v>32767) v=32767;
              s->data16[i]=(short)v;
            }
          } else {
            double off=127.0*(offPct/100.0);
            for (unsigned int i=from; i<to; i++) {
              double v=off+s->data8[i]*vol;
              if (v<-128) v=-128; if (v>127) v=127;
              s->data8[i]=(signed char)v;
            }
          }
          e->renderSamples(idx);
        });
      } else if (op=="normalize") {
        unsigned int from,to; mcpRange(args,s->samples,from,to);
        e->lockEngine([&]() {
          double maxVal=0.0;
          if (is16) {
            for (unsigned int i=from; i<to; i++) { double v=fabs((double)s->data16[i]/32767.0); if (v>maxVal) maxVal=v; }
            if (maxVal>1.0) maxVal=1.0;
            if (maxVal>0.0) { double vv=1.0/maxVal; for (unsigned int i=from; i<to; i++) { double v=s->data16[i]*vv; if (v<-32768) v=-32768; if (v>32767) v=32767; s->data16[i]=(short)v; } }
          } else {
            for (unsigned int i=from; i<to; i++) { double v=fabs((double)s->data8[i]/127.0); if (v>maxVal) maxVal=v; }
            if (maxVal>1.0) maxVal=1.0;
            if (maxVal>0.0) { double vv=1.0/maxVal; for (unsigned int i=from; i<to; i++) { double v=s->data8[i]*vv; if (v<-128) v=-128; if (v>127) v=127; s->data8[i]=(signed char)v; } }
          }
          e->renderSamples(idx);
        });
      } else if (op=="fade_in" || op=="fade_out") {
        unsigned int from,to; mcpRange(args,s->samples,from,to);
        if (to<=from) throw std::runtime_error("fade needs a non-empty range");
        bool in=(op=="fade_in");
        e->lockEngine([&]() {
          if (is16) {
            for (unsigned int i=from; i<to; i++) { double f=in?((double)(i-from)/(double)(to-from)):((double)(to-i)/(double)(to-from)); double v=s->data16[i]*f; if (v<-32768) v=-32768; if (v>32767) v=32767; s->data16[i]=(short)v; }
          } else {
            for (unsigned int i=from; i<to; i++) { double f=in?((double)(i-from)/(double)(to-from)):((double)(to-i)/(double)(to-from)); double v=s->data8[i]*f; if (v<-128) v=-128; if (v>127) v=127; s->data8[i]=(signed char)v; }
          }
          e->renderSamples(idx);
        });
      } else if (op=="silence") {
        unsigned int from,to; mcpRange(args,s->samples,from,to);
        e->lockEngine([&]() {
          if (is16) { for (unsigned int i=from; i<to; i++) s->data16[i]=0; }
          else { for (unsigned int i=from; i<to; i++) s->data8[i]=0; }
          e->renderSamples(idx);
        });
      } else if (op=="insert_silence") {
        int at=mcpOptInt(args,"at",(int)s->samples);
        int length=mcpArgInt(args,"length");
        if (at<0 || at>(int)s->samples) throw std::runtime_error(fmt::sprintf("'at' out of range: %d (0..%u)",at,s->samples));
        if (length<1) throw std::runtime_error("'length' must be >= 1");
        bool ok=true;
        e->lockEngine([&]() {
          if (!s->insert(at,length)) ok=false;
          e->renderSamples(idx);
        });
        if (!ok) throw std::runtime_error("insert failed");
      } else if (op=="trim") {
        unsigned int from,to; mcpRange(args,s->samples,from,to);
        if (to<=from) throw std::runtime_error("trim needs a non-empty range");
        bool ok=true;
        e->lockEngine([&]() {
          if (!s->trim(from,to)) ok=false;
          e->renderSamples(idx);
        });
        if (!ok) throw std::runtime_error("trim failed");
      } else if (op=="resample") {
        double toRate=mcpOptFloat(args,"to_rate",0.0);
        if (toRate<100.0) throw std::runtime_error("'to_rate' must be >= 100");
        int filter=mcpOptInt(args,"filter",DIV_RESAMPLE_LINEAR);
        if (filter<0 || filter>DIV_RESAMPLE_BEST) throw std::runtime_error(fmt::sprintf("filter out of range: %d (0..%d)",filter,(int)DIV_RESAMPLE_BEST));
        double srcRate=(double)s->centerRate;
        bool ok=true;
        e->lockEngine([&]() {
          if (!s->resample(srcRate,toRate,filter)) ok=false;
          e->renderSamples(idx);
        });
        if (!ok) throw std::runtime_error("resample failed (needs 8/16-bit and target rate >= 100)");
      } else if (op=="reverse") {
        unsigned int from,to; mcpRange(args,s->samples,from,to);
        e->lockEngine([&]() {
          if (is16) { for (unsigned int i=from; i<to; i++) { unsigned int ri=to-i-1+from; if (ri<=i) break; short t=s->data16[i]; s->data16[i]=s->data16[ri]; s->data16[ri]=t; } }
          else { for (unsigned int i=from; i<to; i++) { unsigned int ri=to-i-1+from; if (ri<=i) break; signed char t=s->data8[i]; s->data8[i]=s->data8[ri]; s->data8[ri]=t; } }
          e->renderSamples(idx);
        });
      } else if (op=="invert") {
        unsigned int from,to; mcpRange(args,s->samples,from,to);
        e->lockEngine([&]() {
          if (is16) { for (unsigned int i=from; i<to; i++) { s->data16[i]=-s->data16[i]; if (s->data16[i]==-32768) s->data16[i]=32767; } }
          else { for (unsigned int i=from; i<to; i++) { s->data8[i]=-s->data8[i]; if (s->data8[i]==-128) s->data8[i]=127; } }
          e->renderSamples(idx);
        });
      } else if (op=="sign_flip") {
        unsigned int from,to; mcpRange(args,s->samples,from,to);
        e->lockEngine([&]() {
          if (is16) { for (unsigned int i=from; i<to; i++) s->data16[i]^=0x8000; }
          else { for (unsigned int i=from; i<to; i++) s->data8[i]^=0x80; }
          e->renderSamples(idx);
        });
      } else if (op=="filter") {
        unsigned int from,to; mcpRange(args,s->samples,from,to);
        double cutStart=mcpOptFloat(args,"cutoff",(double)s->centerRate*0.25);
        double cutEnd=mcpOptFloat(args,"cutoffEnd",cutStart);
        bool sweep=mcpOptBool(args,"sweep",false);
        double reson=mcpOptFloat(args,"resonance",0.0);
        if (reson<0.0) reson=0.0; if (reson>0.99) reson=0.99;
        int fpow=mcpOptInt(args,"power",1);
        if (fpow<1 || fpow>3) throw std::runtime_error("power must be 1..3");
        double lowMix=mcpOptFloat(args,"low",1.0);
        double bandMix=mcpOptFloat(args,"band",0.0);
        double highMix=mcpOptFloat(args,"high",0.0);
        double nyq=s->centerRate*0.5;
        if (cutStart<0.0) cutStart=0.0; if (cutStart>nyq) cutStart=nyq;
        if (cutEnd<0.0) cutEnd=0.0; if (cutEnd>nyq) cutEnd=nyq;
        e->lockEngine([&]() {
          double res=1.0-pow(reson,0.5);
          double low=0,band=0,high=0;
          double power=(cutStart>cutEnd)?0.5:2.0;
          for (unsigned int i=from; i<to; i++) {
            double freq=cutStart+(sweep?((cutEnd-cutStart)*pow((double)(i-from)/(double)(to-from),power)):0.0);
            double cut=sin((freq/(double)s->centerRate)*M_PI);
            double in=is16?(double)s->data16[i]:(double)s->data8[i];
            for (int j=0; j<fpow; j++) { low=low+cut*band; high=in-low-(res*band); band=cut*high+band; }
            double v=low*lowMix+band*bandMix+high*highMix;
            if (is16) { if (v<-32768) v=-32768; if (v>32767) v=32767; s->data16[i]=(short)v; }
            else { if (v<-128) v=-128; if (v>127) v=127; s->data8[i]=(signed char)v; }
          }
          e->renderSamples(idx);
        });
      } else if (op=="crossfade_loop") {
        int length=mcpArgInt(args,"length");
        int law=mcpOptInt(args,"law",0);
        if (law<0 || law>100) throw std::runtime_error("law must be 0..100");
        if (s->loopStart<0 || s->loopEnd<0 || s->loopEnd<=s->loopStart) throw std::runtime_error("crossfade_loop requires valid loopStart/loopEnd (set them with set_sample_props)");
        if (length<1) throw std::runtime_error("'length' must be >= 1");
        if (length>s->loopStart) throw std::runtime_error("crossfade length would go before the sample start");
        if (length>(s->loopEnd-s->loopStart)) throw std::runtime_error("crossfade length would overflow the loop region");
        e->lockEngine([&]() {
          double l=1.0/(double)length;
          double evar=1.0-law/200.0;
          if (is16) {
            unsigned int ci=s->loopStart-length;
            unsigned int co=s->loopEnd-length;
            for (int i=0; i<length; i++) { double f1=pow(i*l,evar); double f2=pow((length-i)*l,evar); double out=((double)s->data16[ci])*f1+((double)s->data16[co])*f2; if (out<-32768) out=-32768; if (out>32767) out=32767; s->data16[co]=(short)out; ci++; co++; }
          } else {
            unsigned int ci=s->loopStart-length;
            unsigned int co=s->loopEnd-length;
            for (int i=0; i<length; i++) { double f1=pow(i*l,evar); double f2=pow((length-i)*l,evar); double out=((double)s->data8[ci])*f1+((double)s->data8[co])*f2; if (out<-128) out=-128; if (out>127) out=127; s->data8[co]=(signed char)out; ci++; co++; }
          }
          e->renderSamples(idx);
        });
      } else if (op=="trim_side_noise") {
        // port of GUI_ACTION_SAMPLE_TRIM_SIDE_NOISE: walk a 128-frame window in
        // from each end and cut until a quarter of the window is above threshold,
        // so a decaying tail isn't mistaken for the noise floor.
        unsigned int from,to; mcpRange(args,s->samples,from,to);
        double thresholdDb=mcpOptFloat(args,"threshold",-60.0);
        if (thresholdDb<-144.0 || thresholdDb>0.0) throw std::runtime_error("'threshold' must be -144..0 (dBFS)");
        if (to<=from) throw std::runtime_error("trim_side_noise needs a non-empty range");
        unsigned int newStart=from, newEnd=to;
        e->lockEngine([&]() {
          float linThreshold=powf(10.0f,(float)thresholdDb/20.0f)*(is16?32767.0f:127.0f);
          unsigned int windowSize=128;
          if (windowSize>(to-from)) windowSize=to-from;
          unsigned int minCount=windowSize/4;
          if (minCount<1) minCount=1;
          auto at=[&](unsigned int i)->float { return fabsf(is16?(float)s->data16[i]:(float)s->data8[i]); };

          unsigned int count=0;
          for (unsigned int j=0; j<windowSize; j++) if (at(from+j)>=linThreshold) count++;
          for (unsigned int i=from; i+windowSize<=to; i++) {
            if (count>=minCount) { newStart=i; break; }
            if (at(i)>=linThreshold) count--;
            if (i+windowSize<to && at(i+windowSize)>=linThreshold) count++;
          }
          count=0;
          for (unsigned int j=0; j<windowSize; j++) if (at(to-windowSize+j)>=linThreshold) count++;
          for (unsigned int i=to; (i-from)>=windowSize; i--) {
            if (count>=minCount) { newEnd=i; break; }
            if (at(i-1)>=linThreshold) count--;
            if (i>=from+windowSize && at(i-windowSize-1)>=linThreshold) count++;
          }

          if (newStart<newEnd && (newStart>from || newEnd<to)) {
            if (from==0 && to==s->samples) {
              s->trim(newStart,newEnd);
            } else {
              if (newEnd<to) s->strip(newEnd,to);
              if (newStart>from) s->strip(from,newStart);
            }
          }
          e->renderSamples(idx);
        });
        result["trimmedStart"]=(int)(newStart-from);
        result["trimmedEnd"]=(int)(to-newEnd);
      } else if (op=="tune_loop") {
        // port of GUI_ACTION_SAMPLE_FIX_LOOP: resample so the loop length lands on
        // the target chip's block alignment, then snap both loop points onto it.
        // this is what makes a looped sample stop clicking on SNES/Amiga hardware.
        static const int startAlign[7]={2,16,2,2,4,8,4};
        static const int lengthAlign[7]={2,16,2,2,4,8,16};
        static const char* targetName[7]={"amiga","snes","c219","nds16","nds8","ndsima","gba_dma"};
        int target=mcpOptInt(args,"target",0);
        if (target<0 || target>=7) throw std::runtime_error("'target' must be 0..6 (0=Amiga 1=SNES 2=Namco C219 3=NDS 16-bit 4=NDS 8-bit 5=NDS IMA 6=GBA DMA)");
        int strat=mcpOptInt(args,"filter",DIV_RESAMPLE_LINEAR);
        if (!s->isLoopable() || s->loopEnd<=s->loopStart) throw std::runtime_error("tune_loop requires a valid loop (set loop/loopStart/loopEnd with set_sample_props)");
        int currentLoopLength=s->loopEnd-s->loopStart;
        if (currentLoopLength<1) throw std::runtime_error("loop length must be greater than zero");

        int alignLength=lengthAlign[target];
        int targetLoopLength=((currentLoopLength+(alignLength>>1))/alignLength)*alignLength;
        if (targetLoopLength<alignLength) targetLoopLength=alignLength;
        double currentRate=s->centerRate;
        double targetFixRate=currentRate*((double)targetLoopLength/(double)currentLoopLength);
        if (targetFixRate<100.0) targetFixRate=100.0;
        if (targetFixRate>384000.0) targetFixRate=384000.0;

        auto snapAlignedInRange=[](int value, int align, int minValue, int maxValue, int& out)->bool {
          if (minValue>maxValue) return false;
          if (align<=1) { out=CLAMP(value,minValue,maxValue); return true; }
          int first=((minValue+align-1)/align)*align;
          int last=(maxValue/align)*align;
          if (first>last) return false;
          int down=(value/align)*align;
          if (value<0 && (value%align)!=0) down-=align;
          int up=down+align;
          if (down<first) down=first;
          if (down>last) down=last;
          if (up<first) up=first;
          if (up>last) up=last;
          int downDist=value-down; if (downDist<0) downDist=-downDist;
          int upDist=up-value; if (upDist<0) upDist=-upDist;
          out=(upDist<=downDist)?up:down;
          return true;
        };

        String failure;
        e->lockEngine([&]() {
          if (!s->resample(currentRate,targetFixRate,strat)) { failure="couldn't resample (8/16-bit samples only, target rate >= 100Hz)"; return; }
          int sampleCount=(int)s->samples;
          if (sampleCount<lengthAlign[target]) { failure="sample is too short for the selected target alignment"; return; }
          int currentStart=s->loopStart;
          int currentLength=s->loopEnd-s->loopStart;
          if (currentLength<1) { failure="loop became invalid after resampling"; return; }
          int snappedLength=0;
          if (!snapAlignedInRange(currentLength,lengthAlign[target],lengthAlign[target],sampleCount,snappedLength)) { failure="unable to fit an aligned loop length into the sample"; return; }
          int snappedStart=0;
          if (!snapAlignedInRange(currentStart,startAlign[target],0,sampleCount-snappedLength,snappedStart)) { failure="unable to fit an aligned loop start into the sample"; return; }
          s->loopStart=snappedStart;
          s->loopEnd=snappedStart+snappedLength;
          if (s->loopEnd>sampleCount) s->loopEnd=sampleCount;
          if (s->loopEnd<=s->loopStart) { failure="failed to produce a valid aligned loop"; return; }
          e->renderSamples(idx);
        });
        if (!failure.empty()) throw std::runtime_error(failure);
        result["target"]=targetName[target];
        result["centerRate"]=(int)s->centerRate;
        result["loopStart"]=s->loopStart;
        result["loopEnd"]=s->loopEnd;
      } else {
        throw std::runtime_error(fmt::sprintf("unknown op: %s",op));
      }

      e->notifySampleChange(idx);
      e->notifyPitchTable();
      e->lockEngine([&]() { result["length"]=(int)s->samples; });
      return result;
    }
  ));
}
