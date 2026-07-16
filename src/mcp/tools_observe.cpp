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

// MCP tools: observability domain — everything a user can see (or hear)
// while the song plays: per-channel state, chip register pools, master and
// per-channel oscilloscopes, chip memory composition, engine stats, the log
// ring, and capture_audio (record the live master output the user hears).

#include "mcp.h"
#include "tools_common.h"
#include "../ta-log.h"
#include "../baseutils.h"
#include "../engine/dispatch.h"

#include <chrono>
#include <thread>

using nlohmann::json;

// encode interleaved float PCM as a 16-bit PCM WAV file in memory.
static std::string floatToWav16(const std::vector<float>& data, int chans, double rate) {
  size_t frames=(chans>0)?(data.size()/chans):0;
  size_t dataBytes=frames*chans*2;
  std::string out;
  out.reserve(44+dataBytes);
  auto u32=[&](unsigned int v) {
    out+=(char)(v&0xff); out+=(char)((v>>8)&0xff);
    out+=(char)((v>>16)&0xff); out+=(char)((v>>24)&0xff);
  };
  auto u16=[&](unsigned short v) {
    out+=(char)(v&0xff); out+=(char)((v>>8)&0xff);
  };
  unsigned int irate=(unsigned int)(rate+0.5);
  out+="RIFF"; u32((unsigned int)(36+dataBytes)); out+="WAVE";
  out+="fmt "; u32(16); u16(1); u16((unsigned short)chans);
  u32(irate); u32(irate*chans*2); u16((unsigned short)(chans*2)); u16(16);
  out+="data"; u32((unsigned int)dataBytes);
  for (size_t i=0; i<frames*chans; i++) {
    float f=data[i];
    if (f<-1.0f) f=-1.0f;
    if (f>1.0f) f=1.0f;
    short s=(short)(f*32767.0f);
    out+=(char)(s&0xff);
    out+=(char)((s>>8)&0xff);
  }
  return out;
}

void registerObserveTools(FurnaceMCP& m) {
  m.addTool(FurnaceMCPTool(
    "get_channel_states",
    "Live per-channel playback state (what the pattern playhead and channel headers show): current note, instrument, volume, pitch/porta/vibrato/tremolo state, key on/off.",
    json{{"type","object"},{"properties",{
      {"channel",{{"type","integer"},{"description","only this channel (optional)"}}}
    }}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int total=e->getTotalChannelCount();
      int from=0, to=total;
      if (args.contains("channel")) {
        from=mcpArgInt(args,"channel");
        if (from<0 || from>=total) throw std::runtime_error("channel out of range");
        to=from+1;
      }
      json chans=json::array();
      for (int i=from; i<to; i++) {
        DivChannelState* cs=e->getChanState(i);
        if (cs==NULL) continue;
        chans.push_back(json{
          {"channel",i},
          {"note",cs->note},
          {"lastIns",cs->lastIns},
          {"volume",cs->volume>>8},
          {"pitch",cs->pitch},
          {"portaSpeed",cs->portaSpeed},
          {"portaNote",cs->portaNote},
          {"vibratoDepth",cs->vibratoDepth},
          {"vibratoRate",cs->vibratoRate},
          {"tremoloDepth",cs->tremoloDepth},
          {"tremoloRate",cs->tremoloRate},
          {"panL",(int)cs->panL},
          {"panR",(int)cs->panR},
          {"keyOn",cs->keyOn},
          {"keyOff",cs->keyOff},
          {"releasing",cs->releasing},
          {"legato",cs->legato},
          {"inPorta",cs->inPorta}
        });
      }
      return json{{"channels",chans}};
    }
  ));

  m.addTool(FurnaceMCPTool(
    "get_registers",
    "A chip's live register pool (what the Register View window shows). depth is 8 or 16 bits per entry.",
    json{{"type","object"},{"properties",{
      {"chip",{{"type","integer"},{"description","system/chip index"}}},
      {"offset",{{"type","integer"},{"description","first register (default 0)"}}},
      {"count",{{"type","integer"},{"description","max registers to return (default all)"}}}
    }},{"required",json::array({"chip"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int chip=mcpArgInt(args,"chip");
      if (chip<0 || chip>=e->song.systemLen) throw std::runtime_error("chip out of range");
      int size=0, depth=8;
      unsigned char* pool=e->getRegisterPool(chip,size,depth);
      if (pool==NULL || size<=0) {
        return json{{"chip",chip},{"available",false},{"note","this chip does not expose a register pool"}};
      }
      int offset=mcpOptInt(args,"offset",0);
      int count=mcpOptInt(args,"count",size);
      if (offset<0 || offset>=size) throw std::runtime_error("offset out of range");
      if (count<1) throw std::runtime_error("count must be positive");
      if (offset+count>size) count=size-offset;
      json values=json::array();
      if (depth==16) {
        unsigned short* pool16=(unsigned short*)pool;
        for (int i=offset; i<offset+count; i++) values.push_back((int)pool16[i]);
      } else {
        for (int i=offset; i<offset+count; i++) values.push_back((int)pool[i]);
      }
      return json{{"chip",chip},{"available",true},{"size",size},{"depth",depth},{"offset",offset},{"values",values}};
    }
  ));

  m.addTool(FurnaceMCPTool(
    "get_oscilloscope",
    "The most recent master-output samples (what the Oscilloscope window shows). Per output channel, newest last.",
    json{{"type","object"},{"properties",{
      {"samples",{{"type","integer"},{"description","how many samples per channel (default 1024, max 32768)"}}}
    }}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int n=mcpOptInt(args,"samples",1024);
      if (n<1 || n>32768) throw std::runtime_error("samples must be 1..32768");
      TAAudioDesc& desc=e->getAudioDescGot();
      int outs=desc.outChans<1?2:desc.outChans;
      if (outs>DIV_MAX_OUTPUTS) outs=DIV_MAX_OUTPUTS;
      json chans=json::array();
      int writePos=e->oscWritePos;
      for (int ch=0; ch<outs; ch++) {
        if (e->oscBuf[ch]==NULL) continue;
        json vals=json::array();
        for (int i=n; i>0; i--) {
          int pos=(writePos-i)&0x7fff;
          vals.push_back(e->oscBuf[ch][pos]);
        }
        chans.push_back(json{{"channel",ch},{"values",vals}});
      }
      return json{{"rate",desc.rate},{"outputs",chans}};
    }
  ));

  m.addTool(FurnaceMCPTool(
    "get_channel_oscilloscope",
    "The most recent per-channel oscilloscope samples (what the Per-channel Oscilloscope window shows). Fixed 65536 Hz sample rate, 16-bit values, newest last.",
    json{{"type","object"},{"properties",{
      {"channel",{{"type","integer"}}},
      {"samples",{{"type","integer"},{"description","how many samples (default 2048, max 65536)"}}}
    }},{"required",json::array({"channel"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int chan=mcpArgInt(args,"channel");
      if (chan<0 || chan>=e->getTotalChannelCount()) throw std::runtime_error("channel out of range");
      int n=mcpOptInt(args,"samples",2048);
      if (n<1 || n>65536) throw std::runtime_error("samples must be 1..65536");
      DivDispatchOscBuffer* buf=e->getOscBuffer(chan);
      if (buf==NULL) {
        return json{{"channel",chan},{"available",false},{"note","this channel does not expose an oscilloscope buffer"}};
      }
      // data[] semantics: -1 means "hold the previous sample" (real -1
      // samples are stored as -2/0xfffe). walk forward from the oldest
      // requested position carrying the held value, like the Chan Osc
      // window does.
      unsigned short writePos=(unsigned short)(buf->needle>>16);
      json vals=json::array();
      short held=0;
      // prime the held value by scanning slightly further back for a concrete sample
      for (int i=n+64; i>n; i--) {
        short v=buf->data[(unsigned short)(writePos-i)];
        if (v!=-1) held=(v==-2)?-1:v;
      }
      for (int i=n; i>0; i--) {
        short v=buf->data[(unsigned short)(writePos-i)];
        if (v!=-1) held=(v==-2)?-1:v;
        vals.push_back((int)held);
      }
      return json{{"channel",chan},{"available",true},{"rate",65536},{"values",vals}};
    }
  ));

  m.addTool(FurnaceMCPTool(
    "capture_audio",
    "Record the live master output (what the user hears) for a duration while playback continues, returned as a base64 16-bit PCM WAV. Start playback first; requires a real audio backend (headless DUMMY audio cannot capture).",
    json{{"type","object"},{"properties",{
      {"seconds",{{"type","number"},{"description","duration, 0.1 to 30 (default 2)"}}}
    }}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      double seconds=mcpOptFloat(args,"seconds",2.0);
      if (seconds<0.1 || seconds>30.0) throw std::runtime_error("seconds must be 0.1..30");
      TAAudioDesc& desc=e->getAudioDescGot();
      double rate=desc.rate<1?44100.0:desc.rate;
      size_t frames=(size_t)(seconds*rate);
      if (!e->startAudioCapture(frames)) throw std::runtime_error("a capture is already active");
      // wait for the audio thread to fill the buffer, with generous margin;
      // if no real audio backend drives the mixer, time out cleanly.
      auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds((long)(seconds*2000.0)+3000);
      while (e->isAudioCapturing()) {
        if (std::chrono::steady_clock::now()>deadline) {
          e->abortAudioCapture();
          while (e->isAudioCapturing()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
          int chans=0; double r=0.0;
          e->takeAudioCapture(chans,r);
          throw std::runtime_error("capture timed out - is a real audio backend running? (headless DUMMY audio does not drive the mixer)");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
      }
      int chans=0; double outRate=0.0;
      std::vector<float> data=e->takeAudioCapture(chans,outRate);
      if (data.empty() || chans<1) throw std::runtime_error("capture returned no data");
      // report silence honestly so agents can assert on it
      float peak=0.0f;
      for (float f: data) { float a=f<0?-f:f; if (a>peak) peak=a; }
      std::string wav=floatToWav16(data,chans,outRate);
      return json{
        {"seconds",(double)(data.size()/chans)/outRate},
        {"rate",outRate},
        {"channels",chans},
        {"frames",(int)(data.size()/chans)},
        {"peak",peak},
        {"silent",peak<0.0001f},
        {"wav_base64",taEncodeBase64(wav)}
      };
    }
  ));

  m.addTool(FurnaceMCPTool(
    "get_memory_composition",
    "Per-chip memory composition (what the Memory window shows): each memory space's capacity, usage, and entries (samples/waves placed in chip memory).",
    json{{"type","object"},{"properties",{
      {"chip",{{"type","integer"},{"description","only this chip (optional)"}}}
    }}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int from=0, to=e->song.systemLen;
      if (args.contains("chip")) {
        from=mcpArgInt(args,"chip");
        if (from<0 || from>=e->song.systemLen) throw std::runtime_error("chip out of range");
        to=from+1;
      }
      json chips=json::array();
      for (int i=from; i<to; i++) {
        DivDispatch* disp=e->getDispatch(i);
        if (disp==NULL) continue;
        json spaces=json::array();
        for (int idx=0; ; idx++) {
          const DivMemoryComposition* mc=disp->getMemCompo(idx);
          if (mc==NULL) break;
          json entries=json::array();
          for (const DivMemoryEntry& en: mc->entries) {
            entries.push_back(json{
              {"name",en.name},
              {"asset",en.asset},
              {"begin",(int)en.begin},
              {"end",(int)en.end}
            });
          }
          spaces.push_back(json{
            {"name",mc->name},
            {"capacity",(int)mc->capacity},
            {"used",(int)mc->used},
            {"entries",entries}
          });
        }
        chips.push_back(json{{"chip",i},{"name",e->getSystemName(e->song.system[i])},{"memory",spaces}});
      }
      return json{{"chips",chips}};
    }
  ));

  m.addTool(FurnaceMCPTool(
    "get_stats",
    "Engine performance stats (what the Statistics window shows): audio processing time per buffer and derived audio load.",
    json{{"type","object"},{"properties",json::object()}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      TAAudioDesc& desc=e->getAudioDescGot();
      double bufSeconds=(desc.rate>0)?((double)desc.bufsize/desc.rate):0.0;
      double procSeconds=(double)e->processTime.load()/1e9;
      return json{
        {"processTimeNs",(double)e->processTime.load()},
        {"audioLoad",(bufSeconds>0)?(procSeconds/bufSeconds):0.0},
        {"rate",desc.rate},
        {"bufferSize",desc.bufsize},
        {"fragments",desc.fragments},
        {"outChannels",desc.outChans}
      };
    }
  ));

  m.addTool(FurnaceMCPTool(
    "get_audio_config",
    "The active audio device configuration.",
    json{{"type","object"},{"properties",json::object()}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      TAAudioDesc& desc=e->getAudioDescGot();
      return json{
        {"deviceName",desc.deviceName},
        {"name",desc.name},
        {"rate",desc.rate},
        {"bufferSize",desc.bufsize},
        {"fragments",desc.fragments},
        {"inChannels",desc.inChans},
        {"outChannels",desc.outChans}
      };
    }
  ));

  m.addTool(FurnaceMCPTool(
    "read_log",
    "Recent engine log entries (what the Log Viewer window shows), newest last.",
    json{{"type","object"},{"properties",{
      {"count",{{"type","integer"},{"description","max entries (default 50)"}}},
      {"level",{{"type","integer"},{"description","only entries at this level or more severe (0=error..4=trace; default all)"}}}
    }}},
    [](FurnaceMCP& m, const json& args) -> json {
      int count=mcpOptInt(args,"count",50);
      if (count<1 || count>TA_LOG_SIZE) throw std::runtime_error("count out of range");
      int maxLevel=mcpOptInt(args,"level",99);
      unsigned short pos=logPosition.load();
      json entries=json::array();
      // walk backwards collecting, then reverse for newest-last order
      std::vector<json> collected;
      for (int i=1; i<=TA_LOG_SIZE && (int)collected.size()<count; i++) {
        const LogEntry& en=logEntries[(unsigned short)(pos-i)&(TA_LOG_SIZE-1)];
        if (!en.ready) continue;
        if (en.loglevel>maxLevel) continue;
        char stamp[32];
        snprintf(stamp,sizeof(stamp),"%02d:%02d:%02d",en.time.tm_hour,en.time.tm_min,en.time.tm_sec);
        collected.push_back(json{{"level",en.loglevel},{"time",stamp},{"text",en.text}});
      }
      for (auto it=collected.rbegin(); it!=collected.rend(); ++it) entries.push_back(*it);
      return json{{"entries",entries}};
    }
  ));
}
