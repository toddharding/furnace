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

// MCP tools: render/export domain. offline audio render (WAV/Opus/FLAC/
// Vorbis/MP3), VGM, command stream, text, JSON, DMF (downgrade) and ROM/
// native (ZSM/TIunA/SAP-R/iPod/GRUB/Amiga-validation) export.

#include "mcp.h"
#include "tools_common.h"
#include "../ta-log.h"
#include "../fileutils.h"
#include "../engine/export.h"

#include <chrono>
#include <thread>
#include <cstdio>
#include <cerrno>
#include <cstring>

using nlohmann::json;

namespace {

// ---------------------------------------------------------------------------
// small local helpers (mirrors of the conventions in tools_common.h /
// mcp.cpp's save_song, generalized for this file's several export tools)

// write a byte blob to disk, verifying the full write. throws on any failure.
size_t writeBytesToFile(const String& path, const void* data, size_t len) {
  FILE* f=ps_fopen(path.c_str(),"wb");
  if (f==NULL) {
    throw std::runtime_error(fmt::sprintf("could not open %s for writing: %s",path,strerror(errno)));
  }
  size_t written=(len>0)?fwrite(data,1,len,f):0;
  fclose(f);
  if (written!=len) {
    throw std::runtime_error(fmt::sprintf("short write to %s (%d/%d bytes)",path,(int)written,(int)len));
  }
  return written;
}

size_t writeStringToFile(const String& path, const String& data) {
  return writeBytesToFile(path,data.data(),data.size());
}

// best-effort file size probe, used to report bytes for files the engine
// itself wrote directly to disk (saveAudio's libsndfile path), and to
// discover which predicted multi-file export names actually got written.
long fileSizeOf(const String& path) {
  FILE* f=ps_fopen(path.c_str(),"rb");
  if (f==NULL) return -1;
  if (fseek(f,0,SEEK_END)!=0) { fclose(f); return -1; }
  long sz=ftell(f);
  fclose(f);
  return sz;
}

String lowerCaseOf(String s) {
  for (char& c: s) {
    if (c>='A' && c<='Z') c+='a'-'A';
  }
  return s;
}

// strip a trailing .wav extension the way DivEngine::saveAudio does for its
// multi-file modes, so we can predict the filenames it will produce.
String stripWavExt(const String& path) {
  String l=lowerCaseOf(path);
  size_t pos=l.rfind(".wav");
  if (pos!=String::npos) return path.substr(0,pos);
  return path;
}

DivAudioExportModes parseAudioExportMode(const String& s) {
  String l=lowerCaseOf(s);
  if (l=="one") return DIV_EXPORT_MODE_ONE;
  if (l=="per_system" || l=="per_sys" || l=="many_sys") return DIV_EXPORT_MODE_MANY_SYS;
  if (l=="per_channel" || l=="per_chan" || l=="many_chan") return DIV_EXPORT_MODE_MANY_CHAN;
  throw std::runtime_error(fmt::sprintf("unknown render mode: %s (expected one/per_system/per_channel)",s));
}

DivAudioExportFormats parseAudioExportFormat(const String& s) {
  String l=lowerCaseOf(s);
  if (l=="wav") return DIV_EXPORT_FORMAT_WAV;
  if (l=="opus") return DIV_EXPORT_FORMAT_OPUS;
  if (l=="flac") return DIV_EXPORT_FORMAT_FLAC;
  if (l=="vorbis" || l=="ogg") return DIV_EXPORT_FORMAT_VORBIS;
  if (l=="mp3" || l=="mpeg" || l=="mpeg_l3") return DIV_EXPORT_FORMAT_MPEG_L3;
  throw std::runtime_error(fmt::sprintf("unknown audio format: %s (expected wav/opus/flac/vorbis/mp3)",s));
}

DivAudioExportWavFormats parseWavFormat(const String& s) {
  String l=lowerCaseOf(s);
  if (l=="u8" || l=="uint8" || l=="8") return DIV_EXPORT_WAV_U8;
  if (l=="s16" || l=="int16" || l=="16") return DIV_EXPORT_WAV_S16;
  if (l=="f32" || l=="float32" || l=="32") return DIV_EXPORT_WAV_F32;
  throw std::runtime_error(fmt::sprintf("unknown wav sample format: %s (expected u8/s16/f32)",s));
}

DivAudioExportBitrateModes parseBitRateMode(const String& s) {
  String l=lowerCaseOf(s);
  if (l=="constant" || l=="cbr") return DIV_EXPORT_BITRATE_CONSTANT;
  if (l=="variable" || l=="vbr") return DIV_EXPORT_BITRATE_VARIABLE;
  if (l=="average" || l=="abr") return DIV_EXPORT_BITRATE_AVERAGE;
  throw std::runtime_error(fmt::sprintf("unknown bit rate mode: %s (expected constant/variable/average)",s));
}

// switch subsong before a playback-driven export (render_wav/export_vgm/
// export_cmdstream all "play" the song to capture their output). no-op when
// "subsong" is absent from args. throws on an out-of-range index.
void switchSubsongIfRequested(DivEngine* e, const json& args) {
  int subsong=mcpOptInt(args,"subsong",-1);
  if (subsong<0) return;
  if ((size_t)subsong>=e->song.subsong.size()) {
    throw std::runtime_error(fmt::sprintf("subsong index out of range: %d (song has %d)",subsong,(int)e->song.subsong.size()));
  }
  e->changeSongP((size_t)subsong);
}

// ROM/native export target short ids, indexed by DivROMExportOptions.
// DIV_ROM_ABSTRACT (index 0) is a base marker, not a selectable target.
const char* const romTargetIds[DIV_ROM_MAX]={
  "abstract",
  "amiga_validation",
  "zsm",
  "tiuna",
  "sap_r",
  "ipod",
  "grub",
  "n64m"
};

const char* romReqPolicyName(DivROMExportReqPolicy p) {
  switch (p) {
    case DIV_REQPOL_EXACT: return "exact";
    case DIV_REQPOL_ANY: return "any";
    case DIV_REQPOL_LAX: return "lax";
  }
  return "unknown";
}

} // namespace

void registerRenderTools(FurnaceMCP& m) {
  // --- audio render (offline, waits for completion) ---
  m.addTool(FurnaceMCPTool(
    "render_wav",
    "Render the song to an audio file via offline export (WAV/Opus/FLAC/Vorbis/MP3). "
    "Blocks until the render finishes (or times out) before returning.",
    json{{"type","object"},{"properties",{
      {"path",{{"type","string"},{"description","destination path. for mode=per_system/per_channel this is a BASE path: the engine strips a trailing .wav and appends _s01.wav/_c01.wav-style suffixes per file"}}},
      {"mode",{{"type","string"},{"enum",json::array({"one","per_system","per_channel"})},{"description","one file, one file per chip, or one file per channel. default: one"}}},
      {"format",{{"type","string"},{"enum",json::array({"wav","opus","flac","vorbis","mp3"})},{"description","container/codec for mode=one or per_channel. mode=per_system always writes 16-bit WAV regardless of this option. default: wav"}}},
      {"wavFormat",{{"type","string"},{"enum",json::array({"u8","s16","f32"})},{"description","sample format when format=wav. default: s16"}}},
      {"sampleRate",{{"type","integer"},{"description","output sample rate, 8000-384000. default 44100. ignored (forced 48000) when format=opus"}}},
      {"outputChannels",{{"type","integer"},{"description","interleaved channel count in the output file, 1-16. default 2. ignored for mode=per_system (fixed to each chip's native output count)"}}},
      {"loops",{{"type","integer"},{"description","number of extra loops to render before stopping. default 0"}}},
      {"fadeOut",{{"type","number"},{"description","fade-out length in seconds, applied on the final loop. default 0"}}},
      {"bitRate",{{"type","integer"},{"description","bit rate in bps, for opus/vorbis(cbr)/mp3(cbr). default 128000"}}},
      {"bitRateMode",{{"type","string"},{"enum",json::array({"constant","variable","average"})},{"description","mp3 only. default: constant"}}},
      {"vbrQuality",{{"type","number"},{"description","quality knob: FLAC compression level 0-8, or vorbis/mp3-vbr quality 0-10. default 6"}}},
      {"channelIndices",{{"type","array"},{"items",{{"type","integer"}}},{"description","mode=per_channel only: which song channels (0-based, as in song_info) to render. default: all channels"}}},
      {"subsong",{{"type","integer"},{"description","subsong index to switch to before rendering. default: whatever subsong is currently selected"}}},
      {"timeoutMs",{{"type","integer"},{"description","how long to wait for the render before aborting it. default 300000 (5 minutes)"}}}
    }},{"required",json::array({"path"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      String path=mcpArgStr(args,"path");

      if (e->isExporting()) {
        throw std::runtime_error("an export is already in progress");
      }

      switchSubsongIfRequested(e,args);

      DivAudioExportOptions opts; // constructor default: mode=ONE, all channels enabled, 44100/stereo/s16
      opts.mode=parseAudioExportMode(mcpOptStr(args,"mode","one"));
      opts.format=parseAudioExportFormat(mcpOptStr(args,"format","wav"));
      opts.wavFormat=parseWavFormat(mcpOptStr(args,"wavFormat","s16"));

      opts.sampleRate=mcpOptInt(args,"sampleRate",44100);
      if (opts.sampleRate<8000) opts.sampleRate=8000;
      if (opts.sampleRate>384000) opts.sampleRate=384000;

      opts.chans=mcpOptInt(args,"outputChannels",2);
      if (opts.chans<1) opts.chans=1;
      if (opts.chans>16) opts.chans=16;

      opts.loops=mcpOptInt(args,"loops",0);
      if (opts.loops<0) opts.loops=0;

      opts.fadeOut=mcpOptFloat(args,"fadeOut",0.0);
      if (opts.fadeOut<0.0) opts.fadeOut=0.0;

      opts.bitRate=mcpOptInt(args,"bitRate",128000);
      opts.bitRateMode=parseBitRateMode(mcpOptStr(args,"bitRateMode","constant"));
      opts.vbrQuality=(float)mcpOptFloat(args,"vbrQuality",6.0);

      if (args.contains("channelIndices")) {
        if (!args["channelIndices"].is_array()) {
          throw std::runtime_error("channelIndices must be an array of integers");
        }
        for (int i=0; i<DIV_MAX_CHANS; i++) opts.channelMask[i]=false;
        for (const json& v: args["channelIndices"]) {
          if (!v.is_number_integer()) throw std::runtime_error("channelIndices must contain integers");
          int idx=v.get<int>();
          if (idx<0 || idx>=e->getTotalChannelCount()) {
            throw std::runtime_error(fmt::sprintf("channel index out of range: %d",idx));
          }
          opts.channelMask[idx]=true;
        }
      }

      int timeoutMs=mcpOptInt(args,"timeoutMs",300000);
      if (timeoutMs<1000) timeoutMs=1000;

      String modeStr=mcpOptStr(args,"mode","one");

      // saveAudio swaps dispatch cores (quitDispatch/initDispatch): in window
      // mode that MUST happen on the GUI thread, which reads dispatch pointers
      // mid-frame (racing it from the net thread crashes; see mcp.h).
      bool started=false;
      String startErr;
      furnaceMCPRunOnGUIOrInline([&]() {
        started=e->saveAudio(path.c_str(),opts);
        if (!started) startErr=e->getLastError();
      });
      if (!started) {
        throw std::runtime_error(fmt::sprintf("could not start audio export: %s",startErr));
      }

      // wait for the export thread to finish, polling isExporting() rather
      // than joining directly (the underlying std::thread is private) so we
      // can enforce a timeout and abort a stuck/looping-forever render.
      auto start=std::chrono::steady_clock::now();
      bool timedOut=false;
      while (e->isExporting()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        long elapsed=(long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
        if (elapsed>timeoutMs) {
          timedOut=true;
          break;
        }
      }

      if (timedOut) {
        // stops, joins the export thread and restores playback cores — another
        // dispatch swap, so GUI thread again.
        furnaceMCPRunOnGUIOrInline([&]() { e->haltAudioFile(); });
        throw std::runtime_error(fmt::sprintf("render_wav timed out after %dms and was aborted",timeoutMs));
      }

      // the export thread has already flagged itself done; join it and hand
      // playback cores back (mirrors what the GUI's render progress popup
      // does once isExporting() goes false). finishAudioFile swaps dispatch
      // cores back — GUI thread, same as the start.
      furnaceMCPRunOnGUIOrInline([&]() {
        e->waitAudioFile();
        e->finishAudioFile();
      });

      json files=json::array();
      if (opts.mode==DIV_EXPORT_MODE_ONE) {
        long sz=fileSizeOf(path);
        if (sz<0) throw std::runtime_error(fmt::sprintf("export reported success but %s was not found on disk",path));
        files.push_back(json{{"path",path},{"bytes",(int)sz}});
      } else {
        String base=stripWavExt(path);
        if (opts.mode==DIV_EXPORT_MODE_MANY_SYS) {
          for (int i=0; i<e->song.systemLen; i++) {
            String fname=fmt::sprintf("%s_s%02d.wav",base,i+1);
            long sz=fileSizeOf(fname);
            if (sz<0) continue; // shouldn't happen, but don't claim a file that isn't there
            files.push_back(json{{"path",fname},{"system",e->getSystemName(e->song.system[i])},{"bytes",(int)sz}});
          }
        } else { // DIV_EXPORT_MODE_MANY_CHAN
          for (int i=0; i<e->getTotalChannelCount(); i++) {
            if (!opts.channelMask[i]) continue;
            String fname=fmt::sprintf("%s_c%02d.wav",base,i+1);
            long sz=fileSizeOf(fname);
            // channels folded into a preceding "combo" channel (e.g. a
            // linked OPL2/OPN2 pair) don't get their own file even when
            // masked in; skip silently rather than reporting a phantom file.
            if (sz<0) continue;
            files.push_back(json{{"path",fname},{"channel",i},{"channelName",e->getChannelName(i)},{"bytes",(int)sz}});
          }
        }
      }

      if (files.empty()) {
        throw std::runtime_error("export reported success but produced no files");
      }

      return json{{"ok",true},{"mode",modeStr},{"files",files}};
    }
  ));

  // --- VGM ---
  m.addTool(FurnaceMCPTool(
    "export_vgm",
    "Export the song to a VGM (Video Game Music) register-dump file by playing it back through the engine.",
    json{{"type","object"},{"properties",{
      {"path",{{"type","string"}}},
      {"loop",{{"type","boolean"},{"description","default true"}}},
      {"version",{{"type","integer"},{"description","VGM format version, e.g. 0x171 (=369) for 1.71, 0x151 for 1.51. default 0x171"}}},
      {"chipMask",{{"type","array"},{"items",{{"type","integer"}}},{"description","system indices (as in song_info) to include, up to 2 of each type. default: all systems (that the chosen version supports)"}}},
      {"patternHints",{{"type","boolean"},{"description","insert pattern-change data blocks, useful for writing a playback routine. default false"}}},
      {"directStream",{{"type","boolean"},{"description","required for DualPCM/MSM6258 export; allows volume/direction changes on samples at the cost of much bigger files. default false"}}},
      {"trailingTicks",{{"type","integer"},{"description","-1 auto-detect (default), -2 add a whole extra loop, N>=0 add N+1 ticks of trailing audio"}}},
      {"dpcm07",{{"type","boolean"},{"description","NES DPCM bank switching via RAM write commands (07) instead of data blocks. not all players support this. default false"}}},
      {"correctedRate",{{"type","integer"},{"description","speed drift compensation sample rate, e.g. 43148 for the DeadFish VgmPlay correction. default 44100 (no compensation)"}}},
      {"subsong",{{"type","integer"},{"description","subsong index to switch to before exporting. default: current"}}}
    }},{"required",json::array({"path"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      String path=mcpArgStr(args,"path");

      switchSubsongIfRequested(e,args);

      bool sysMaskStorage[DIV_MAX_CHIPS];
      bool* sysMask=NULL;
      if (args.contains("chipMask")) {
        if (!args["chipMask"].is_array()) throw std::runtime_error("chipMask must be an array of integers");
        for (int i=0; i<DIV_MAX_CHIPS; i++) sysMaskStorage[i]=false;
        for (const json& v: args["chipMask"]) {
          if (!v.is_number_integer()) throw std::runtime_error("chipMask must contain integers");
          int idx=v.get<int>();
          if (idx<0 || idx>=e->song.systemLen) {
            throw std::runtime_error(fmt::sprintf("chip index out of range: %d",idx));
          }
          sysMaskStorage[idx]=true;
        }
        sysMask=sysMaskStorage;
      }

      SafeWriter* w=e->saveVGM(
        sysMask,
        mcpOptBool(args,"loop",true),
        mcpOptInt(args,"version",0x171),
        mcpOptBool(args,"patternHints",false),
        mcpOptBool(args,"directStream",false),
        mcpOptInt(args,"trailingTicks",-1),
        mcpOptBool(args,"dpcm07",false),
        mcpOptInt(args,"correctedRate",44100)
      );
      if (w==NULL) throw std::runtime_error(fmt::sprintf("could not export VGM: %s",e->getLastError()));
      String data=mcpWriterToString(w);
      size_t bytes=writeStringToFile(path,data);
      json result{{"ok",true},{"path",path},{"bytes",(int)bytes}};
      String warnings=e->getWarnings();
      if (!warnings.empty()) result["warnings"]=warnings;
      return result;
    }
  ));

  // --- command stream ---
  m.addTool(FurnaceMCPTool(
    "export_cmdstream",
    "Export the internal command stream produced by playing the song back (technical/development-use binary format).",
    json{{"type","object"},{"properties",{
      {"path",{{"type","string"}}},
      {"longPointers",{{"type","boolean"},{"description","use for 64K+ size streams. default false"}}},
      {"bigEndian",{{"type","boolean"},{"description","default false"}}},
      {"noCmdCallOpt",{{"type","boolean"},{"description","don't optimize command calls. default false"}}},
      {"noDelayCondense",{{"type","boolean"},{"description","don't condense delays. default false"}}},
      {"noSubBlock",{{"type","boolean"},{"description","don't perform sub-block search. default false"}}},
      {"subsong",{{"type","integer"},{"description","subsong index to switch to before exporting. default: current"}}}
    }},{"required",json::array({"path"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      String path=mcpArgStr(args,"path");

      switchSubsongIfRequested(e,args);

      DivCSOptions opts;
      opts.longPointers=mcpOptBool(args,"longPointers",false);
      opts.bigEndian=mcpOptBool(args,"bigEndian",false);
      opts.noCmdCallOpt=mcpOptBool(args,"noCmdCallOpt",false);
      opts.noDelayCondense=mcpOptBool(args,"noDelayCondense",false);
      opts.noSubBlock=mcpOptBool(args,"noSubBlock",false);

      SafeWriter* w=e->saveCommand(NULL,opts);
      if (w==NULL) throw std::runtime_error(fmt::sprintf("could not export command stream: %s",e->getLastError()));
      String data=mcpWriterToString(w);
      size_t bytes=writeStringToFile(path,data);
      return json{{"ok",true},{"path",path},{"bytes",(int)bytes}};
    }
  ));

  // --- text ---
  m.addTool(FurnaceMCPTool(
    "export_text",
    "Export the song to a human-readable text dump of its orders and patterns.",
    json{{"type","object"},{"properties",{
      {"path",{{"type","string"}}},
      {"separatePatterns",{{"type","boolean"},{"description","list each unique pattern once in its own section instead of inlining pattern data per order. default true"}}}
    }},{"required",json::array({"path"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      String path=mcpArgStr(args,"path");
      SafeWriter* w=e->saveText(mcpOptBool(args,"separatePatterns",true));
      if (w==NULL) throw std::runtime_error(fmt::sprintf("could not export text: %s",e->getLastError()));
      String data=mcpWriterToString(w);
      size_t bytes=writeStringToFile(path,data);
      return json{{"ok",true},{"path",path},{"bytes",(int)bytes}};
    }
  ));

  // --- JSON/BSON/CBOR ---
  m.addTool(FurnaceMCPTool(
    "export_json",
    "Export the song to a JSON, BSON or CBOR file (the engine's serialized song form) on disk. "
    "Section toggles mirror the song_json tool, but this writes the exact chosen format's bytes rather than reparsing into a JSON value.",
    json{{"type","object"},{"properties",{
      {"path",{{"type","string"}}},
      {"format",{{"type","string"},{"enum",json::array({"json","bson","cbor"})},{"description","default: json"}}},
      {"pretty",{{"type","boolean"},{"description","indent the output (format=json only). default false"}}},
      {"metadata",{{"type","boolean"},{"description","default true"}}},
      {"chips",{{"type","boolean"},{"description","default true"}}},
      {"orders",{{"type","boolean"},{"description","default true"}}},
      {"patterns",{{"type","boolean"},{"description","default true"}}},
      {"optimizePatterns",{{"type","boolean"},{"description","patterns only. default true"}}},
      {"instruments",{{"type","boolean"},{"description","default true"}}},
      {"waves",{{"type","boolean"},{"description","default true"}}},
      {"samples",{{"type","boolean"},{"description","default true"}}},
      {"compatFlags",{{"type","boolean"},{"description","default false"}}}
    }},{"required",json::array({"path"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      String path=mcpArgStr(args,"path");

      DivJSONExportOptions opts;
      String fmtStr=mcpOptStr(args,"format","json");
      String fmtLower=lowerCaseOf(fmtStr);
      if (fmtLower=="json") opts.format=DivJSONExportOptions::EXPORT_JSON;
      else if (fmtLower=="bson") opts.format=DivJSONExportOptions::EXPORT_BSON;
      else if (fmtLower=="cbor") opts.format=DivJSONExportOptions::EXPORT_CBOR;
      else throw std::runtime_error(fmt::sprintf("unknown JSON export format: %s (expected json/bson/cbor)",fmtStr));

      opts.jsonPretty=mcpOptBool(args,"pretty",false);
      opts.exportMetadata=mcpOptBool(args,"metadata",true);
      opts.exportChips=mcpOptBool(args,"chips",true);
      opts.exportOrders=mcpOptBool(args,"orders",true);
      opts.exportPatterns=mcpOptBool(args,"patterns",true);
      opts.optimizePatterns=mcpOptBool(args,"optimizePatterns",true);
      opts.exportInstruments=mcpOptBool(args,"instruments",true);
      opts.exportWaves=mcpOptBool(args,"waves",true);
      opts.exportSamples=mcpOptBool(args,"samples",true);
      opts.exportCompatFlags=mcpOptBool(args,"compatFlags",false);

      SafeWriter* w=e->saveJSON(&opts);
      if (w==NULL) throw std::runtime_error(fmt::sprintf("could not export JSON: %s",e->getLastError()));
      String data=mcpWriterToString(w);
      size_t bytes=writeStringToFile(path,data);
      return json{{"ok",true},{"path",path},{"format",fmtLower},{"bytes",(int)bytes}};
    }
  ));

  // --- DMF (downgrade) ---
  m.addTool(FurnaceMCPTool(
    "export_dmf",
    "Export (downgrade) the song to a DefleMask module file. Only useful for interop with DefleMask or older Furnace versions.",
    json{{"type","object"},{"properties",{
      {"path",{{"type","string"}}},
      {"version",{{"type","integer"},{"description","DMF format version byte: 26 = 1.1.3 and higher (default), 24 = legacy 1.0/0.12. clamped to [24,26]"}}}
    }},{"required",json::array({"path"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      String path=mcpArgStr(args,"path");
      int version=mcpOptInt(args,"version",26);
      if (version<24) version=24;
      if (version>26) version=26;
      SafeWriter* w=e->saveDMF((unsigned char)version);
      if (w==NULL) throw std::runtime_error(fmt::sprintf("could not export DMF: %s",e->getLastError()));
      String data=mcpWriterToString(w);
      size_t bytes=writeStringToFile(path,data);
      json result{{"ok",true},{"path",path},{"version",version},{"bytes",(int)bytes}};
      String warnings=e->getWarnings();
      if (!warnings.empty()) result["warnings"]=warnings;
      return result;
    }
  ));

  // --- ROM / native export targets ---
  m.addTool(FurnaceMCPTool(
    "list_rom_exports",
    "List available ROM/native export targets (ZSM, TIunA, SAP-R, iPod .tone, GRUB_INIT_TUNE, Amiga validation) "
    "and whether each is viable for the currently loaded song's chip setup.",
    json{{"type","object"},{"properties",json::object()}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      json list=json::array();
      bool anyViable=false;
      for (int i=1; i<DIV_ROM_MAX; i++) { // skip DIV_ROM_ABSTRACT (index 0): a base marker, not a real target
        DivROMExportOptions opt=(DivROMExportOptions)i;
        const DivROMExportDef* def=e->getROMExportDef(opt);
        if (def==NULL) continue; // not registered on this build
        bool viable=e->isROMExportViable(opt);
        if (viable) anyViable=true;

        json requisites=json::array();
        for (DivSystem sys: def->requisites) {
          requisites.push_back(e->getSystemName(sys));
        }

        json entry{
          {"id",romTargetIds[i]},
          {"name",def->name},
          {"author",def->author},
          {"description",def->description},
          {"multiOutput",def->multiOutput},
          {"requisites",requisites},
          {"requisitePolicy",romReqPolicyName(def->requisitePolicy)},
          {"viable",viable}
        };
        entry["fileType"]=def->fileType?json(def->fileType):json(nullptr);
        entry["fileExt"]=def->fileExt?json(def->fileExt):json(nullptr);
        list.push_back(entry);
      }

      json result{{"targets",list},{"anyViable",anyViable}};
      if (!anyViable) {
        result["note"]="no ROM export target is viable for the current song's chip setup. "
                        "each target needs specific chips present (see each entry's \"requisites\"/\"requisitePolicy\").";
      }
      return result;
    }
  ));

  m.addTool(FurnaceMCPTool(
    "export_rom",
    "Run a ROM/native export target (see list_rom_exports for ids and viability) and write its output to disk. "
    "Multi-output targets (multiOutput=true in list_rom_exports) treat 'path' as an existing output directory.",
    json{{"type","object"},{"properties",{
      {"target",{{"type","string"},{"description","target id from list_rom_exports: amiga_validation, zsm, tiuna, sap_r, ipod, grub, or n64m"}}},
      {"path",{{"type","string"},{"description","output file path, or output directory (must already exist) for multi-output targets"}}},
      {"config",{{"type","object"},{"description",
        "target-specific config keys (string/int/float/bool values). known keys: "
        "zsm: zsmrate(int), loop(bool), optimize(bool); "
        "tiuna: baseLabel(string), firstBankSize(int), otherBankSize(int), sysToExport(int, index of the TIA system); "
        "grub: exportBin(bool). "
        "amiga_validation/sap_r/ipod take no config."
      }}}
    }},{"required",json::array({"target","path"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      String targetStr=mcpArgStr(args,"target");
      String path=mcpArgStr(args,"path");
      String targetLower=lowerCaseOf(targetStr);

      int found=-1;
      for (int i=1; i<DIV_ROM_MAX; i++) {
        if (targetLower==romTargetIds[i]) { found=i; break; }
      }
      if (found<0) {
        throw std::runtime_error(fmt::sprintf("unknown ROM export target: %s (see list_rom_exports)",targetStr));
      }
      DivROMExportOptions opt=(DivROMExportOptions)found;

      const DivROMExportDef* def=e->getROMExportDef(opt);
      if (def==NULL) {
        throw std::runtime_error(fmt::sprintf("ROM export target not registered on this build: %s",targetStr));
      }
      if (!e->isROMExportViable(opt)) {
        throw std::runtime_error(fmt::sprintf(
          "ROM export target \"%s\" is not viable for this song's chip setup (see list_rom_exports for its requirements)",
          def->name
        ));
      }

      DivConfig conf;
      if (args.contains("config")) {
        if (!args["config"].is_object()) throw std::runtime_error("config must be an object");
        for (auto it=args["config"].begin(); it!=args["config"].end(); ++it) {
          const json& v=it.value();
          if (v.is_boolean()) conf.set(it.key(),v.get<bool>());
          else if (v.is_number_integer()) conf.set(it.key(),v.get<int>());
          else if (v.is_number_float()) conf.set(it.key(),v.get<double>());
          else if (v.is_string()) conf.set(it.key(),v.get<String>());
          else throw std::runtime_error(fmt::sprintf("unsupported config value type for key: %s",it.key()));
        }
      }

      DivROMExport* exp=e->buildROM(opt);
      if (exp==NULL) throw std::runtime_error("could not create ROM exporter (buildROM returned NULL)");
      exp->setConf(conf);
      if (!exp->go(e)) {
        delete exp;
        throw std::runtime_error("could not begin ROM export process");
      }
      exp->wait();
      if (exp->hasFailed()) {
        String err=e->getLastError();
        delete exp;
        throw std::runtime_error(fmt::sprintf("ROM export failed: %s",err));
      }

      json files=json::array();
      for (DivROMExportOutput& out: exp->getResult()) {
        String outPath=path;
        if (def->multiOutput) {
          outPath=path+DIR_SEPARATOR_STR+out.name;
        }
        String data=mcpWriterToString(out.data); // drains and frees the output SafeWriter
        size_t bytes=writeStringToFile(outPath,data);
        files.push_back(json{{"path",outPath},{"bytes",(int)bytes}});
      }
      delete exp;

      if (files.empty()) {
        throw std::runtime_error("ROM export succeeded but produced no output files");
      }

      return json{{"ok",true},{"target",targetLower},{"files",files}};
    }
  ));
}
