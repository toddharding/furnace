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

#include "mcp.h"
#include "../ta-log.h"
#include "../fileutils.h"

#include <stdexcept>

using nlohmann::json;

#define MCP_PROTOCOL_VERSION "2024-11-05"

// ---------------------------------------------------------------------------
// helpers

static json jsonRPCError(const json& id, int code, const String& message) {
  return json{
    {"jsonrpc","2.0"},
    {"id",id},
    {"error",{{"code",code},{"message",message}}}
  };
}

static json jsonRPCResult(const json& id, const json& result) {
  return json{
    {"jsonrpc","2.0"},
    {"id",id},
    {"result",result}
  };
}

// required argument accessors: throw with a clear message when missing/wrong.
static int argInt(const json& args, const char* name) {
  if (!args.contains(name) || !args[name].is_number_integer()) {
    throw std::runtime_error(fmt::sprintf("missing or non-integer argument: %s",name));
  }
  return args[name].get<int>();
}

static String argStr(const json& args, const char* name) {
  if (!args.contains(name) || !args[name].is_string()) {
    throw std::runtime_error(fmt::sprintf("missing or non-string argument: %s",name));
  }
  return args[name].get<String>();
}

static int optInt(const json& args, const char* name, int def) {
  if (!args.contains(name)) return def;
  if (!args[name].is_number_integer()) {
    throw std::runtime_error(fmt::sprintf("non-integer argument: %s",name));
  }
  return args[name].get<int>();
}

static bool optBool(const json& args, const char* name, bool def) {
  if (!args.contains(name)) return def;
  if (!args[name].is_boolean()) {
    throw std::runtime_error(fmt::sprintf("non-boolean argument: %s",name));
  }
  return args[name].get<bool>();
}

// drain a SafeWriter into a String and dispose of it.
static String writerToString(SafeWriter* w) {
  if (w==NULL) throw std::runtime_error("engine returned no data");
  String out((const char*)w->getFinalBuf(),w->size());
  w->finish();
  delete w;
  return out;
}

// ---------------------------------------------------------------------------
// core tools (phase 1 vertical slice: lifecycle, transport, inspect, live input)

void FurnaceMCP::registerCoreTools() {
  // --- inspect ---
  addTool(FurnaceMCPTool(
    "song_info",
    "Get a summary of the loaded song: metadata, systems (chips), channels, subsongs, timing.",
    json{{"type","object"},{"properties",json::object()}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      json systems=json::array();
      for (int i=0; i<e->song.systemLen; i++) {
        systems.push_back(json{
          {"index",i},
          {"name",e->getSystemName(e->song.system[i])},
          {"channels",e->getChannelCount(e->song.system[i])}
        });
      }
      return json{
        {"name",e->song.name},
        {"author",e->song.author},
        {"systems",systems},
        {"totalChannels",e->getTotalChannelCount()},
        {"subsongs",(int)e->song.subsong.size()},
        {"currentSubsong",(int)e->getCurrentSubSong()},
        {"instruments",e->song.insLen},
        {"wavetables",e->song.waveLen},
        {"samples",e->song.sampleLen},
        {"tickRate",e->getHz()},
        {"version",DIV_VERSION}
      };
    }
  ));

  addTool(FurnaceMCPTool(
    "song_json",
    "Get the full song as structured JSON (the engine's saveJSON form). Section toggles let you fetch only what you need.",
    json{{"type","object"},{"properties",{
      {"pretty",{{"type","boolean"},{"description","indent the JSON (default false)"}}},
      {"metadata",{{"type","boolean"}}},
      {"chips",{{"type","boolean"}}},
      {"orders",{{"type","boolean"}}},
      {"patterns",{{"type","boolean"}}},
      {"instruments",{{"type","boolean"}}},
      {"waves",{{"type","boolean"}}},
      {"samples",{{"type","boolean"}}},
      {"compatFlags",{{"type","boolean"}}}
    }}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivJSONExportOptions opts;
      opts.jsonPretty=optBool(args,"pretty",false);
      opts.exportMetadata=optBool(args,"metadata",true);
      opts.exportChips=optBool(args,"chips",true);
      opts.exportOrders=optBool(args,"orders",true);
      opts.exportPatterns=optBool(args,"patterns",true);
      opts.exportInstruments=optBool(args,"instruments",true);
      opts.exportWaves=optBool(args,"waves",true);
      opts.exportSamples=optBool(args,"samples",true);
      opts.exportCompatFlags=optBool(args,"compatFlags",true);
      String out=writerToString(m.engine()->saveJSON(&opts));
      // parse back so the payload is structured JSON, not a string blob
      return json::parse(out);
    }
  ));

  // --- song lifecycle ---
  addTool(FurnaceMCPTool(
    "open_song",
    "Load a song file (.fur/.dmf and supported module formats) from a path on disk.",
    json{{"type","object"},{"properties",{
      {"path",{{"type","string"},{"description","file path to load"}}}
    }},{"required",json::array({"path"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      String path=argStr(args,"path");
      FILE* f=ps_fopen(path.c_str(),"rb");
      if (f==NULL) throw std::runtime_error(fmt::sprintf("could not open %s: %s",path,strerror(errno)));
      if (fseek(f,0,SEEK_END)!=0) { fclose(f); throw std::runtime_error("seek error"); }
      long len=ftell(f);
      if (len<1) { fclose(f); throw std::runtime_error("empty or unreadable file"); }
      fseek(f,0,SEEK_SET);
      unsigned char* buf=new unsigned char[len];
      if (fread(buf,1,(size_t)len,f)!=(size_t)len) {
        fclose(f);
        delete[] buf;
        throw std::runtime_error("read error");
      }
      fclose(f);
      // on success the engine takes ownership of buf (same as main.cpp's load path)
      if (!m.engine()->load(buf,(size_t)len,path.c_str())) {
        throw std::runtime_error(fmt::sprintf("could not load: %s",m.engine()->getLastError()));
      }
      return json{{"ok",true},{"path",path},{"name",m.engine()->song.name}};
    }
  ));

  addTool(FurnaceMCPTool(
    "save_song",
    "Save the song as .fur to a path on disk.",
    json{{"type","object"},{"properties",{
      {"path",{{"type","string"},{"description","destination file path"}}}
    }},{"required",json::array({"path"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      String path=argStr(args,"path");
      SafeWriter* w=m.engine()->saveFur();
      if (w==NULL) throw std::runtime_error("could not serialize song");
      FILE* f=ps_fopen(path.c_str(),"wb");
      if (f==NULL) {
        w->finish();
        delete w;
        throw std::runtime_error(fmt::sprintf("could not open %s for writing: %s",path,strerror(errno)));
      }
      size_t written=fwrite(w->getFinalBuf(),1,w->size(),f);
      size_t total=w->size();
      fclose(f);
      w->finish();
      delete w;
      if (written!=total) throw std::runtime_error("short write");
      return json{{"ok",true},{"path",path},{"bytes",(int)total}};
    }
  ));

  // --- transport ---
  addTool(FurnaceMCPTool(
    "play",
    "Start playback from the beginning, or from a given order.",
    json{{"type","object"},{"properties",{
      {"order",{{"type","integer"},{"description","order to start from (optional)"}}}
    }}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      if (args.contains("order")) {
        e->setOrder((unsigned char)argInt(args,"order"));
      }
      e->play();
      return json{{"playing",e->isPlaying()}};
    }
  ));

  addTool(FurnaceMCPTool(
    "stop",
    "Stop playback.",
    json{{"type","object"},{"properties",json::object()}},
    [](FurnaceMCP& m, const json& args) -> json {
      m.engine()->stop();
      return json{{"playing",m.engine()->isPlaying()}};
    }
  ));

  addTool(FurnaceMCPTool(
    "panic",
    "Immediately silence everything (engine sync reset).",
    json{{"type","object"},{"properties",json::object()}},
    [](FurnaceMCP& m, const json& args) -> json {
      m.engine()->syncReset();
      return json{{"ok",true}};
    }
  ));

  addTool(FurnaceMCPTool(
    "get_position",
    "Get the live playback position (order, row, tick, speed) and transport state.",
    json{{"type","object"},{"properties",json::object()}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int order=0, row=0, tick=0, speed=0;
      e->getPlayPosTick(order,row,tick,speed);
      return json{
        {"order",order},
        {"row",row},
        {"tick",tick},
        {"speed",speed},
        {"playing",e->isPlaying()},
        {"halted",e->isHalted()},
        {"elapsedBars",e->getElapsedBars()},
        {"elapsedBeats",e->getElapsedBeats()}
      };
    }
  ));

  addTool(FurnaceMCPTool(
    "set_order",
    "Set the current order (position in the order list).",
    json{{"type","object"},{"properties",{
      {"order",{{"type","integer"}}}
    }},{"required",json::array({"order"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      m.engine()->setOrder((unsigned char)argInt(args,"order"));
      return json{{"order",(int)m.engine()->getOrder()}};
    }
  ));

  addTool(FurnaceMCPTool(
    "get_order",
    "Get the current order.",
    json{{"type","object"},{"properties",json::object()}},
    [](FurnaceMCP& m, const json& args) -> json {
      return json{{"order",(int)m.engine()->getOrder()}};
    }
  ));

  // --- live note input ---
  addTool(FurnaceMCPTool(
    "note_on",
    "Play a note on a channel with an instrument (like jamming on the keyboard).",
    json{{"type","object"},{"properties",{
      {"channel",{{"type","integer"}}},
      {"instrument",{{"type","integer"}}},
      {"note",{{"type","integer"},{"description","note number; 60 = C-5"}}},
      {"volume",{{"type","integer"},{"description","optional volume"}}}
    }},{"required",json::array({"channel","instrument","note"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int chan=argInt(args,"channel");
      if (chan<0 || chan>=e->getTotalChannelCount()) throw std::runtime_error("channel out of range");
      e->noteOn(chan,argInt(args,"instrument"),argInt(args,"note"),optInt(args,"volume",-1));
      return json{{"ok",true}};
    }
  ));

  addTool(FurnaceMCPTool(
    "note_off",
    "Release the note on a channel.",
    json{{"type","object"},{"properties",{
      {"channel",{{"type","integer"}}}
    }},{"required",json::array({"channel"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int chan=argInt(args,"channel");
      if (chan<0 || chan>=e->getTotalChannelCount()) throw std::runtime_error("channel out of range");
      e->noteOff(chan);
      return json{{"ok",true}};
    }
  ));
}

// ---------------------------------------------------------------------------
// protocol

void FurnaceMCP::addTool(FurnaceMCPTool t) {
  tools.push_back(t);
}

void FurnaceMCP::bindEngine(DivEngine* eng) {
  e=eng;
}

#ifdef HAVE_GUI
void FurnaceMCP::bindGUI(FurnaceGUI* g) {
  gui=g;
  // register the window-only tools exactly once, only when a GUI is bound,
  // so headless transports never surface them in tools/list.
  if (g!=NULL && !windowToolsRegistered) {
    registerWindowTools(*this);
    windowToolsRegistered=true;
  }
}
#endif

json FurnaceMCP::handleRequest(const json& req) {
  json id=nullptr;
  if (req.contains("id")) id=req["id"];
  const bool isNotification=!req.contains("id");

  if (!req.is_object() || !req.contains("method") || !req["method"].is_string()) {
    if (isNotification) return json(nullptr);
    return jsonRPCError(id,-32600,"invalid request");
  }
  String method=req["method"].get<String>();
  json params=req.contains("params")?req["params"]:json::object();

  if (method=="initialize") {
    return jsonRPCResult(id,json{
      {"protocolVersion",MCP_PROTOCOL_VERSION},
      {"capabilities",{{"tools",json::object()}}},
      {"serverInfo",{{"name","furnace-mcp"},{"version",DIV_VERSION}}}
    });
  }
  if (method=="notifications/initialized" || method=="initialized") {
    return json(nullptr);
  }
  if (method=="ping") {
    return jsonRPCResult(id,json::object());
  }
  if (method=="tools/list") {
    json list=json::array();
    for (FurnaceMCPTool& t: tools) {
      list.push_back(json{
        {"name",t.name},
        {"description",t.description},
        {"inputSchema",t.inputSchema}
      });
    }
    return jsonRPCResult(id,json{{"tools",list}});
  }
  if (method=="tools/call") {
    if (!params.contains("name") || !params["name"].is_string()) {
      return jsonRPCError(id,-32602,"tools/call requires a tool name");
    }
    String name=params["name"].get<String>();
    json args=params.contains("arguments")?params["arguments"]:json::object();
    for (FurnaceMCPTool& t: tools) {
      if (name==t.name) {
        try {
          json result=t.handler(*this,args);
          return jsonRPCResult(id,json{
            {"content",json::array({json{{"type","text"},{"text",result.dump()}}})}
          });
        } catch (std::exception& ex) {
          return jsonRPCResult(id,json{
            {"content",json::array({json{{"type","text"},{"text",ex.what()}}})},
            {"isError",true}
          });
        }
      }
    }
    return jsonRPCError(id,-32602,fmt::sprintf("unknown tool: %s",name));
  }

  if (isNotification) return json(nullptr);
  return jsonRPCError(id,-32601,fmt::sprintf("unknown method: %s",method));
}

String FurnaceMCP::handleLine(const String& line) {
  if (line.empty()) return "";
  json req;
  try {
    req=json::parse(line);
  } catch (std::exception& ex) {
    return jsonRPCError(nullptr,-32700,"parse error").dump();
  }
  json resp=handleRequest(req);
  if (resp.is_null()) return "";
  return resp.dump();
}

// ---------------------------------------------------------------------------
// self-test (--mcp-selftest): in-process assertions against the bound engine.

#define MCP_CHECK(cond,what) \
  if (!(cond)) { \
    logE("MCP selftest FAILED: %s",what); \
    return 1; \
  } else { \
    logI("MCP selftest ok: %s",what); \
  }

int FurnaceMCP::selfTest() {
  furnaceMCPFixupStdio();
  // initialize
  json resp=handleRequest(json::parse("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}"));
  MCP_CHECK(resp["result"]["serverInfo"]["name"]=="furnace-mcp","initialize returns serverInfo");

  // tools/list
  resp=handleRequest(json::parse("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}"));
  MCP_CHECK(resp["result"]["tools"].is_array() && resp["result"]["tools"].size()>=10,"tools/list lists the core tools");

  // song_info on the default song
  resp=handleRequest(json{{"jsonrpc","2.0"},{"id",3},{"method","tools/call"},{"params",{{"name","song_info"},{"arguments",json::object()}}}});
  MCP_CHECK(!resp["result"].value("isError",false),"song_info succeeds");
  json info=json::parse(resp["result"]["content"][0]["text"].get<String>());
  MCP_CHECK(info["totalChannels"].get<int>()>0,"song_info reports channels");

  // song_json round-trips as parseable structured JSON
  resp=handleRequest(json{{"jsonrpc","2.0"},{"id",4},{"method","tools/call"},{"params",{{"name","song_json"},{"arguments",{{"patterns",false},{"samples",false}}}}}});
  MCP_CHECK(!resp["result"].value("isError",false),"song_json succeeds");

  // transport state read-back
  resp=handleRequest(json{{"jsonrpc","2.0"},{"id",5},{"method","tools/call"},{"params",{{"name","get_position"},{"arguments",json::object()}}}});
  json pos=json::parse(resp["result"]["content"][0]["text"].get<String>());
  MCP_CHECK(pos.contains("playing") && pos.contains("order"),"get_position reports transport state");

  // unknown tool is a clean JSON-RPC error
  resp=handleRequest(json{{"jsonrpc","2.0"},{"id",6},{"method","tools/call"},{"params",{{"name","no_such_tool"}}}});
  MCP_CHECK(resp.contains("error") && resp["error"]["code"].get<int>()==-32602,"unknown tool is a JSON-RPC error");

  // unknown method is a clean JSON-RPC error
  resp=handleRequest(json{{"jsonrpc","2.0"},{"id",7},{"method","bogus/method"}});
  MCP_CHECK(resp.contains("error") && resp["error"]["code"].get<int>()==-32601,"unknown method is a JSON-RPC error");

  // parse error is a clean JSON-RPC error
  String lineResp=handleLine("this is not json");
  MCP_CHECK(lineResp.find("-32700")!=String::npos,"parse error is a JSON-RPC error");

  // notification produces no reply
  MCP_CHECK(handleLine("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}")=="","notification produces no reply");

  // list_effects enumerates the channel's effect vocabulary (incl. vibrato)
  resp=handleRequest(json{{"jsonrpc","2.0"},{"id",9},{"method","tools/call"},{"params",{{"name","list_effects"},{"arguments",json::object()}}}});
  MCP_CHECK(!resp["result"].value("isError",false),"list_effects succeeds");
  {
    json fx=json::parse(resp["result"]["content"][0]["text"].get<String>());
    bool hasVibrato=false;
    for (auto& en: fx["effects"]) if (en["code"]==4) hasVibrato=true;
    MCP_CHECK(fx["effects"].size()>8 && hasVibrato,"list_effects includes vibrato (04xy)");
  }

  // bad tool args are a tool-level error, not a crash
  resp=handleRequest(json{{"jsonrpc","2.0"},{"id",8},{"method","tools/call"},{"params",{{"name","note_on"},{"arguments",{{"channel",99999},{"instrument",0},{"note",60}}}}}});
  MCP_CHECK(resp["result"].value("isError",false),"out-of-range channel is a tool error");

  logI("MCP selftest passed.");
  return 0;
}

FurnaceMCP::FurnaceMCP():
  e(NULL)
#ifdef HAVE_GUI
  , gui(NULL)
  , windowToolsRegistered(false)
#endif
{
  registerCoreTools();
  registerPatternTools(*this);
  registerInstrumentTools(*this);
  registerAssetTools(*this);
  registerSongTools(*this);
  registerRenderTools(*this);
  registerObserveTools(*this);
}
