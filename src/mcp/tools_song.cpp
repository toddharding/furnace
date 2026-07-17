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

// MCP tools: song / systems / config domain.
//
// key-name contract: get/set_song_meta and get/set_compat_flags mirror the
// key names src/engine/fileOps/json.cpp's saveJSON() uses for the
// "songInfo" and "compatFlags" sections exactly, so a value read here agrees
// with song_json's output. chip flags (get/set_chip_flags) mirror how
// saveJSON dumps song.systemFlags[i].configMap() too: DivConfig stores every
// value as a string internally, so both saveJSON and this file surface chip
// flags as raw strings (e.g. "true"/"3"), not JSON booleans/numbers -
// set_chip_flags still accepts typed JSON values in, formatting them the
// same way DivConfig::set()'s typed overloads would.

#include "mcp.h"
#include "tools_common.h"
#include "../ta-log.h"

#include <vector>
#include <cctype>
#include <algorithm>

using nlohmann::json;

// ---------------------------------------------------------------------------
// resolvers / validators

// resolve a subsong index defaulting to the currently active one.
static int mcpSSIndex(FurnaceMCP& m, const json& args, const char* key="subsong") {
  DivEngine* e=m.engine();
  int idx=mcpOptInt(args,key,(int)e->getCurrentSubSong());
  if (idx<0 || (size_t)idx>=e->song.subsong.size()) {
    throw std::runtime_error(fmt::sprintf("subsong out of range: %d (have %d)",idx,(int)e->song.subsong.size()));
  }
  return idx;
}

// resolve an explicit (non-defaulted) subsong index. returns -1 when the key
// is absent so callers can tell "not given" from "given as 0".
static int mcpOptSSIndex(FurnaceMCP& m, const json& args, const char* key="subsong") {
  if (!args.contains(key) || args[key].is_null()) return -1;
  int idx=mcpArgInt(args,key);
  if (idx<0 || (size_t)idx>=m.engine()->song.subsong.size()) {
    throw std::runtime_error(fmt::sprintf("subsong out of range: %d (have %d)",idx,(int)m.engine()->song.subsong.size()));
  }
  return idx;
}

static int mcpReqChannel(FurnaceMCP& m, const json& args) {
  int ch=mcpArgInt(args,"channel");
  int chans=m.engine()->song.chans;
  if (ch<0 || ch>=chans) throw std::runtime_error(fmt::sprintf("channel out of range: %d (have %d)",ch,chans));
  return ch;
}

static int mcpReqSystemIndex(FurnaceMCP& m, const json& args, const char* key="index") {
  int idx=mcpArgInt(args,key);
  int len=m.engine()->song.systemLen;
  if (idx<0 || idx>=len) throw std::runtime_error(fmt::sprintf("system index out of range: %d (have %d)",idx,len));
  return idx;
}

// case-insensitive ASCII compare (system display names are ASCII).
static bool mcpStrEqCI(const String& a, const String& b) {
  if (a.size()!=b.size()) return false;
  for (size_t i=0; i<a.size(); i++) {
    if (tolower((unsigned char)a[i])!=tolower((unsigned char)b[i])) return false;
  }
  return true;
}

// resolve a DivSystem from a "system" argument: either its numeric DivSystem
// id, or its display name (exact match first, then case-insensitive) as
// returned by list_available_systems / getSystemName.
static DivSystem mcpResolveSystem(const json& v) {
  if (v.is_number_integer()) {
    int id=v.get<int>();
    if (id<=0 || id>=DIV_SYSTEM_MAX || DivEngine::getSystemDef((DivSystem)id)==NULL) {
      throw std::runtime_error(fmt::sprintf("unknown system id: %d (see list_available_systems)",id));
    }
    return (DivSystem)id;
  }
  if (v.is_string()) {
    String s=v.get<String>();
    for (int i=1; i<DIV_SYSTEM_MAX; i++) {
      const DivSysDef* d=DivEngine::getSystemDef((DivSystem)i);
      if (d==NULL) continue;
      if (s==d->name) return (DivSystem)i;
    }
    for (int i=1; i<DIV_SYSTEM_MAX; i++) {
      const DivSysDef* d=DivEngine::getSystemDef((DivSystem)i);
      if (d==NULL) continue;
      if (mcpStrEqCI(s,d->name)) return (DivSystem)i;
    }
    throw std::runtime_error(fmt::sprintf("unknown system: '%s' (see list_available_systems)",s.c_str()));
  }
  throw std::runtime_error("'system' must be a string (display name) or integer (system id)");
}

// build the "systems" array shared by song_info/add_system/remove_system/etc.
static json mcpSystemsJSON(DivEngine* e) {
  json systems=json::array();
  for (int i=0; i<e->song.systemLen; i++) {
    systems.push_back(json{
      {"index",i},
      {"id",(int)e->song.system[i]},
      {"name",e->getSystemName(e->song.system[i])},
      {"channels",e->song.systemChans[i]}
    });
  }
  return systems;
}

// best-effort "is this channel soloed" read: true when this channel is
// unmuted and every other channel in the song is muted (mirrors the check
// DivEngine::toggleSolo() itself uses to decide whether to un-solo).
static bool mcpChannelIsSolo(DivEngine* e, int chan) {
  if (e->isChannelMuted(chan)) return false;
  for (int i=0; i<e->song.chans; i++) {
    if (i==chan) continue;
    if (!e->isChannelMuted(i)) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// compat flags: X-macro lists mirroring DivCompatFlags' field order exactly
// (same order fileOps/json.cpp's serializeCompatFlags() emits them in).

#define MCP_COMPAT_INT_FLAGS(X) \
  X(linearPitch) \
  X(pitchSlideSpeed) \
  X(loopModality) \
  X(delayBehavior) \
  X(jumpTreatment)

#define MCP_COMPAT_BOOL_FLAGS(X) \
  X(limitSlides) \
  X(properNoiseLayout) \
  X(waveDutyIsVol) \
  X(resetMacroOnPorta) \
  X(legacyVolumeSlides) \
  X(compatibleArpeggio) \
  X(noteOffResetsSlides) \
  X(targetResetsSlides) \
  X(arpNonPorta) \
  X(algMacroBehavior) \
  X(brokenShortcutSlides) \
  X(ignoreDuplicateSlides) \
  X(stopPortaOnNoteOff) \
  X(continuousVibrato) \
  X(brokenDACMode) \
  X(oneTickCut) \
  X(newInsTriggersInPorta) \
  X(arp0Reset) \
  X(brokenSpeedSel) \
  X(noSlidesOnFirstTick) \
  X(rowResetsArpPos) \
  X(ignoreJumpAtEnd) \
  X(buggyPortaAfterSlide) \
  X(gbInsAffectsEnvelope) \
  X(sharedExtStat) \
  X(ignoreDACModeOutsideIntendedChannel) \
  X(e1e2AlsoTakePriority) \
  X(newSegaPCM) \
  X(fbPortaPause) \
  X(snDutyReset) \
  X(pitchMacroIsLinear) \
  X(oldOctaveBoundary) \
  X(noOPN2Vol) \
  X(newVolumeScaling) \
  X(volMacroLinger) \
  X(brokenOutVol) \
  X(brokenOutVol2) \
  X(e1e2StopOnSameNote) \
  X(brokenPortaArp) \
  X(snNoLowPeriods) \
  X(disableSampleMacro) \
  X(oldArpStrategy) \
  X(brokenPortaLegato) \
  X(brokenFMOff) \
  X(preNoteNoEffect) \
  X(oldDPCM) \
  X(resetArpPhaseOnNewNote) \
  X(ceilVolumeScaling) \
  X(oldAlwaysSetVolume) \
  X(oldSampleOffset) \
  X(oldCenterRate) \
  X(noVolSlideReset)

static json mcpCompatFlagsJSON(const DivCompatFlags& flags) {
  json j;
#define MCP_CF_GETB(f) j[#f]=(bool)flags.f;
  MCP_COMPAT_BOOL_FLAGS(MCP_CF_GETB)
#undef MCP_CF_GETB
#define MCP_CF_GETI(f) j[#f]=(int)flags.f;
  MCP_COMPAT_INT_FLAGS(MCP_CF_GETI)
#undef MCP_CF_GETI
  return j;
}

// ---------------------------------------------------------------------------

void registerSongTools(FurnaceMCP& m) {
  // -------------------------------------------------------------------------
  // get_song_meta
  m.addTool(FurnaceMCPTool(
    "get_song_meta",
    "Get song metadata: name, author, album, systemName (+ systemNameAuto), tuning (A-4 Hz), notes (song comments). Pass 'subsong' to also get that subsong's name/notes.",
    json{{"type","object"},{"properties",{
      {"subsong",{{"type","integer"},{"description","optional: also return this subsong's name/notes"}}}
    }}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      json out{
        {"name",e->song.name},
        {"author",e->song.author},
        {"album",e->song.category},
        {"systemName",e->song.systemName},
        {"systemNameAuto",e->song.autoSystem},
        {"tuning",e->song.tuning},
        {"notes",e->song.notes}
      };
      int ss=mcpOptSSIndex(m,args);
      if (ss>=0) {
        DivSubSong* s=e->song.subsong[ss];
        out["subsong"]=json{{"index",ss},{"name",s->name},{"notes",s->notes}};
      }
      return out;
    }
  ));

  // -------------------------------------------------------------------------
  // set_song_meta
  m.addTool(FurnaceMCPTool(
    "set_song_meta",
    "Set song metadata. Any of: name, author, album, systemName (typing it clears systemNameAuto, matching the editor), systemNameAuto (bool; if set true with no systemName given in the same call, systemName is recomputed via the engine's legacy-name resolver - an approximation of the editor's system-detection dialog, not identical to it), tuning (A-4 Hz, 220..880), notes (song comments). Pass 'subsong' + subsongName/subsongNotes to rename/annotate one subsong instead (subsong is required for those two fields).",
    json{{"type","object"},{"properties",{
      {"name",{{"type","string"}}},
      {"author",{{"type","string"}}},
      {"album",{{"type","string"}}},
      {"systemName",{{"type","string"}}},
      {"systemNameAuto",{{"type","boolean"}}},
      {"tuning",{{"type","number"},{"description","A-4 Hz, 220..880"}}},
      {"notes",{{"type","string"},{"description","song comments"}}},
      {"subsong",{{"type","integer"},{"description","required only for subsongName/subsongNotes"}}},
      {"subsongName",{{"type","string"}}},
      {"subsongNotes",{{"type","string"}}}
    }}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      bool hasName=args.contains("name");
      bool hasAuthor=args.contains("author");
      bool hasAlbum=args.contains("album");
      bool hasSysName=args.contains("systemName");
      bool hasSysAuto=args.contains("systemNameAuto");
      bool hasTuning=args.contains("tuning");
      bool hasNotes=args.contains("notes");
      bool hasSubName=args.contains("subsongName");
      bool hasSubNotes=args.contains("subsongNotes");

      String name=hasName?mcpArgStr(args,"name"):"";
      String author=hasAuthor?mcpArgStr(args,"author"):"";
      String album=hasAlbum?mcpArgStr(args,"album"):"";
      String sysName=hasSysName?mcpArgStr(args,"systemName"):"";
      bool sysAuto=hasSysAuto?args["systemNameAuto"].get<bool>():false;
      if (hasSysAuto && !args["systemNameAuto"].is_boolean()) throw std::runtime_error("systemNameAuto must be a boolean");
      double tuning=hasTuning?mcpOptFloat(args,"tuning",e->song.tuning):e->song.tuning;
      if (hasTuning && (tuning<220.0 || tuning>880.0)) throw std::runtime_error(fmt::sprintf("tuning out of range: %g (220..880)",tuning));
      String notes=hasNotes?mcpArgStr(args,"notes"):"";
      String subName=hasSubName?mcpArgStr(args,"subsongName"):"";
      String subNotes=hasSubNotes?mcpArgStr(args,"subsongNotes"):"";

      int ss=-1;
      if (hasSubName || hasSubNotes) {
        if (!args.contains("subsong")) throw std::runtime_error("'subsong' is required to set subsongName/subsongNotes");
        ss=mcpOptSSIndex(m,args);
      } else if (args.contains("subsong")) {
        ss=mcpOptSSIndex(m,args);
      }

      e->lockEngine([&]() {
        if (hasName) e->song.name=name;
        if (hasAuthor) e->song.author=author;
        if (hasAlbum) e->song.category=album;
        if (hasSysName) { e->song.systemName=sysName; e->song.autoSystem=false; }
        if (hasSysAuto) {
          e->song.autoSystem=sysAuto;
          if (sysAuto && !hasSysName) {
            e->song.systemName=e->getSongSystemLegacyName(e->song,true);
          }
        }
        if (hasTuning) e->song.tuning=(float)tuning;
        if (hasNotes) e->song.notes=notes;
        if (hasSubName) e->song.subsong[ss]->name=subName;
        if (hasSubNotes) e->song.subsong[ss]->notes=subNotes;
      });
      if (hasTuning) e->notifyPitchTable();

      json out{
        {"name",e->song.name},
        {"author",e->song.author},
        {"album",e->song.category},
        {"systemName",e->song.systemName},
        {"systemNameAuto",e->song.autoSystem},
        {"tuning",e->song.tuning},
        {"notes",e->song.notes}
      };
      if (ss>=0) {
        DivSubSong* s=e->song.subsong[ss];
        out["subsong"]=json{{"index",ss},{"name",s->name},{"notes",s->notes}};
      }
      return out;
    }
  ));

  // -------------------------------------------------------------------------
  // list_subsongs
  m.addTool(FurnaceMCPTool(
    "list_subsongs",
    "List all subsongs: index, name, notes, patLen, ordersLen, hz, and whether each is the currently active one.",
    json{{"type","object"},{"properties",json::object()}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int cur=(int)e->getCurrentSubSong();
      json list=json::array();
      for (size_t i=0; i<e->song.subsong.size(); i++) {
        DivSubSong* s=e->song.subsong[i];
        list.push_back(json{
          {"index",(int)i},
          {"name",s->name},
          {"notes",s->notes},
          {"patLen",s->patLen},
          {"ordersLen",s->ordersLen},
          {"hz",s->hz},
          {"current",(int)i==cur}
        });
      }
      return json{{"subsongs",list},{"currentSubsong",cur}};
    }
  ));

  // -------------------------------------------------------------------------
  // add_subsong
  m.addTool(FurnaceMCPTool(
    "add_subsong",
    "Add a new (blank) subsong. Returns its index. Fails past the 127-subsong limit.",
    json{{"type","object"},{"properties",json::object()}},
    [](FurnaceMCP& m, const json& args) -> json {
      int idx=m.engine()->addSubSong();
      if (idx<0) throw std::runtime_error("could not add subsong (limit of 127 reached)");
      return json{{"index",idx},{"subsongs",(int)m.engine()->song.subsong.size()}};
    }
  ));

  // -------------------------------------------------------------------------
  // remove_subsong
  m.addTool(FurnaceMCPTool(
    "remove_subsong",
    "Remove a subsong by index. Refuses when it's the only subsong left (a song must keep at least one).",
    json{{"type","object"},{"properties",{
      {"index",{{"type","integer"}}}
    }},{"required",json::array({"index"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int idx=mcpArgInt(args,"index");
      if (idx<0 || (size_t)idx>=e->song.subsong.size()) {
        throw std::runtime_error(fmt::sprintf("subsong out of range: %d (have %d)",idx,(int)e->song.subsong.size()));
      }
      if (e->song.subsong.size()<=1) {
        throw std::runtime_error("cannot remove the last subsong (a song must have at least one)");
      }
      if (!e->removeSubSong(idx)) throw std::runtime_error("could not remove subsong");
      return json{{"removed",idx},{"subsongs",(int)e->song.subsong.size()},{"currentSubsong",(int)e->getCurrentSubSong()}};
    }
  ));

  // -------------------------------------------------------------------------
  // move_subsong
  m.addTool(FurnaceMCPTool(
    "move_subsong",
    "Move a subsong from one position to another (reorders the subsong list). Implemented as a sequence of adjacent swaps via the engine's move-up/move-down verbs.",
    json{{"type","object"},{"properties",{
      {"from",{{"type","integer"}}},
      {"to",{{"type","integer"}}}
    }},{"required",json::array({"from","to"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int n=(int)e->song.subsong.size();
      int from=mcpArgInt(args,"from");
      int to=mcpArgInt(args,"to");
      if (from<0 || from>=n) throw std::runtime_error(fmt::sprintf("'from' out of range: %d (have %d)",from,n));
      if (to<0 || to>=n) throw std::runtime_error(fmt::sprintf("'to' out of range: %d (have %d)",to,n));
      if (from<to) {
        for (int pos=from; pos<to; pos++) e->moveSubSongDown((size_t)pos);
      } else if (from>to) {
        for (int pos=from; pos>to; pos--) e->moveSubSongUp((size_t)pos);
      }
      json list=json::array();
      for (size_t i=0; i<e->song.subsong.size(); i++) list.push_back(e->song.subsong[i]->name);
      return json{{"from",from},{"to",to},{"names",list},{"currentSubsong",(int)e->getCurrentSubSong()}};
    }
  ));

  // -------------------------------------------------------------------------
  // change_subsong
  m.addTool(FurnaceMCPTool(
    "change_subsong",
    "Switch the currently active subsong (what patterns/order tools default to, and what plays on 'play').",
    json{{"type","object"},{"properties",{
      {"index",{{"type","integer"}}}
    }},{"required",json::array({"index"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int idx=mcpArgInt(args,"index");
      if (idx<0 || (size_t)idx>=e->song.subsong.size()) {
        throw std::runtime_error(fmt::sprintf("subsong out of range: %d (have %d)",idx,(int)e->song.subsong.size()));
      }
      e->changeSongP((size_t)idx);
      return json{{"currentSubsong",(int)e->getCurrentSubSong()}};
    }
  ));

  // -------------------------------------------------------------------------
  // list_available_systems
  m.addTool(FurnaceMCPTool(
    "list_available_systems",
    "Enumerate every sound chip (DivSystem) this build knows how to emulate: id (pass to add_system/change_system), name (display name, e.g. \"Game Boy\"), nameJ (Japanese name, empty if none), channels/minChannels/maxChannels, and fm/std/compound flags. 'compound' systems (e.g. Genesis, which is really YM2612+SMS) are informational only - the engine expects compound pairs to be added as their constituent systems, not the compound id itself.",
    json{{"type","object"},{"properties",json::object()}},
    [](FurnaceMCP& m, const json& args) -> json {
      json list=json::array();
      for (int i=1; i<DIV_SYSTEM_MAX; i++) {
        const DivSysDef* d=DivEngine::getSystemDef((DivSystem)i);
        if (d==NULL) continue;
        list.push_back(json{
          {"id",i},
          {"name",d->name},
          {"nameJ",d->nameJ?String(d->nameJ):String("")},
          {"channels",d->channels},
          {"minChannels",d->minChans},
          {"maxChannels",d->maxChans},
          {"fm",d->isFM},
          {"std",d->isSTD},
          {"compound",d->isCompound}
        });
      }
      return json{{"systems",list}};
    }
  ));

  // -------------------------------------------------------------------------
  // add_system
  m.addTool(FurnaceMCPTool(
    "add_system",
    "Add a chip to the song (identify it by display name or numeric id - see list_available_systems). Can fail (max system/channel limits); the error message is the engine's own. Returns the updated systems list.",
    json{{"type","object"},{"properties",{
      {"system",{{"description","system display name (string) or id (integer)"}}}
    }},{"required",json::array({"system"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      if (!args.contains("system")) throw std::runtime_error("missing argument: system");
      DivSystem sys=mcpResolveSystem(args["system"]);
      if (!e->addSystem(sys)) throw std::runtime_error(e->getLastError());
      return json{{"systems",mcpSystemsJSON(e)},{"totalChannels",e->song.chans}};
    }
  ));

  // -------------------------------------------------------------------------
  // remove_system
  m.addTool(FurnaceMCPTool(
    "remove_system",
    "Remove a chip by index. Refuses on the last remaining system. Returns the updated systems list.",
    json{{"type","object"},{"properties",{
      {"index",{{"type","integer"}}},
      {"preserveOrder",{{"type","boolean"},{"description","keep channel positions of the surviving systems stable (default true)"}}}
    }},{"required",json::array({"index"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int idx=mcpReqSystemIndex(m,args);
      bool preserveOrder=mcpOptBool(args,"preserveOrder",true);
      if (!e->removeSystem(idx,preserveOrder)) throw std::runtime_error(e->getLastError());
      return json{{"systems",mcpSystemsJSON(e)},{"totalChannels",e->song.chans}};
    }
  ));

  // -------------------------------------------------------------------------
  // change_system
  m.addTool(FurnaceMCPTool(
    "change_system",
    "Replace the chip at 'index' with a different one (identify it by display name or numeric id). Resets that system's chip flags. Can fail (channel limits). Returns the updated systems list.",
    json{{"type","object"},{"properties",{
      {"index",{{"type","integer"}}},
      {"system",{{"description","system display name (string) or id (integer)"}}},
      {"preserveOrder",{{"type","boolean"},{"description","keep channel positions stable (default true)"}}}
    }},{"required",json::array({"index","system"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int idx=mcpReqSystemIndex(m,args);
      if (!args.contains("system")) throw std::runtime_error("missing argument: system");
      DivSystem sys=mcpResolveSystem(args["system"]);
      bool preserveOrder=mcpOptBool(args,"preserveOrder",true);
      if (!e->changeSystem(idx,sys,preserveOrder)) throw std::runtime_error(e->getLastError());
      return json{{"systems",mcpSystemsJSON(e)},{"totalChannels",e->song.chans}};
    }
  ));

  // -------------------------------------------------------------------------
  // swap_systems
  m.addTool(FurnaceMCPTool(
    "swap_systems",
    "Swap the positions of two chips (and their channel ranges). Returns the updated systems list.",
    json{{"type","object"},{"properties",{
      {"a",{{"type","integer"}}},
      {"b",{{"type","integer"}}},
      {"preserveOrder",{{"type","boolean"},{"description","default true"}}}
    }},{"required",json::array({"a","b"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int a=mcpArgInt(args,"a");
      int b=mcpArgInt(args,"b");
      bool preserveOrder=mcpOptBool(args,"preserveOrder",true);
      if (!e->swapSystem(a,b,preserveOrder)) throw std::runtime_error(e->getLastError());
      return json{{"systems",mcpSystemsJSON(e)},{"totalChannels",e->song.chans}};
    }
  ));

  // -------------------------------------------------------------------------
  // get_chip_flags
  m.addTool(FurnaceMCPTool(
    "get_chip_flags",
    "Get a chip's configuration flags (clock, chip sub-type, per-chip quirks - keys vary by chip). Values are raw strings, same as song_json's chips[].flags (DivConfig stores everything as text internally).",
    json{{"type","object"},{"properties",{
      {"index",{{"type","integer"}}}
    }},{"required",json::array({"index"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int idx=mcpReqSystemIndex(m,args);
      json flags=json::object();
      e->lockEngine([&]() {
        for (auto& kv: e->song.systemFlags[idx].configMap()) flags[kv.first]=kv.second;
      });
      return json{{"index",idx},{"flags",flags}};
    }
  ));

  // -------------------------------------------------------------------------
  // set_chip_flags
  m.addTool(FurnaceMCPTool(
    "set_chip_flags",
    "Set one or more of a chip's configuration flags, then apply them the way the editor's chip manager does (DivEngine::updateSysFlags). Give each value with its natural JSON type (boolean/integer/float/string) - it's stored the same way the equivalent DivConfig::set() overload would. 'restart' (default true) restarts playback/dispatch to pick up the change live; 'render' (default true) re-renders any chip-derived sample data. Unknown keys are accepted (chips read only the keys they understand) - check the chip's current flags via get_chip_flags to see which keys apply.",
    json{{"type","object"},{"properties",{
      {"index",{{"type","integer"}}},
      {"flags",{{"type","object"},{"description","key -> boolean/integer/number/string"}}},
      {"restart",{{"type","boolean"}}},
      {"render",{{"type","boolean"}}}
    }},{"required",json::array({"index","flags"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int idx=mcpReqSystemIndex(m,args);
      if (!args.contains("flags") || !args["flags"].is_object()) throw std::runtime_error("missing or non-object argument: flags");
      bool restart=mcpOptBool(args,"restart",true);
      bool render=mcpOptBool(args,"render",true);
      e->lockEngine([&]() {
        DivConfig& flags=e->song.systemFlags[idx];
        for (auto it=args["flags"].begin(); it!=args["flags"].end(); ++it) {
          const json& v=it.value();
          if (v.is_boolean()) flags.set(it.key(),v.get<bool>());
          else if (v.is_number_integer()) flags.set(it.key(),v.get<int>());
          else if (v.is_number_float()) flags.set(it.key(),(float)v.get<double>());
          else if (v.is_string()) flags.set(it.key(),v.get<String>());
          else throw std::runtime_error(fmt::sprintf("flag '%s' must be a boolean, number, or string",it.key().c_str()));
        }
      });
      e->updateSysFlags(idx,restart,render);
      json flags=json::object();
      e->lockEngine([&]() {
        for (auto& kv: e->song.systemFlags[idx].configMap()) flags[kv.first]=kv.second;
      });
      return json{{"index",idx},{"flags",flags}};
    }
  ));

  // -------------------------------------------------------------------------
  // get_channels
  m.addTool(FurnaceMCPTool(
    "get_channels",
    "List every channel: index, name/abbrev (the subsong's custom names, falling back to the chip's default channel names), chip (the system index it belongs to), muted, solo (best-effort: true when this is the only unmuted channel), show (visible in the pattern view).",
    json{{"type","object"},{"properties",{
      {"subsong",{{"type","integer"},{"description","subsong index (default: current) - only affects custom name lookup"}}}
    }}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int ss=mcpSSIndex(m,args);
      DivSubSong* s=e->song.subsong[ss];
      json list=json::array();
      for (int i=0; i<e->song.chans; i++) {
        String name=s->chanName[i].empty()?e->song.chanDef[i].name:s->chanName[i];
        String abbrev=s->chanShortName[i].empty()?e->song.chanDef[i].shortName:s->chanShortName[i];
        list.push_back(json{
          {"index",i},
          {"name",name},
          {"abbrev",abbrev},
          {"chip",e->song.dispatchOfChan[i]},
          {"muted",e->isChannelMuted(i)},
          {"solo",mcpChannelIsSolo(e,i)},
          {"show",s->chanShow[i]}
        });
      }
      return json{{"subsong",ss},{"channels",list}};
    }
  ));

  // -------------------------------------------------------------------------
  // set_channel
  m.addTool(FurnaceMCPTool(
    "set_channel",
    "Set a channel's per-subsong display name/abbreviation/visibility. Any of name, abbrev, show; only provided fields change.",
    json{{"type","object"},{"properties",{
      {"subsong",{{"type","integer"},{"description","subsong index (default: current)"}}},
      {"channel",{{"type","integer"}}},
      {"name",{{"type","string"}}},
      {"abbrev",{{"type","string"}}},
      {"show",{{"type","boolean"}}}
    }},{"required",json::array({"channel"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int ss=mcpSSIndex(m,args);
      int ch=mcpReqChannel(m,args);
      DivSubSong* s=e->song.subsong[ss];
      bool hasName=args.contains("name");
      bool hasAbbrev=args.contains("abbrev");
      bool hasShow=args.contains("show");
      String name=hasName?mcpArgStr(args,"name"):"";
      String abbrev=hasAbbrev?mcpArgStr(args,"abbrev"):"";
      bool show=hasShow?args["show"].get<bool>():true;
      if (hasShow && !args["show"].is_boolean()) throw std::runtime_error("show must be a boolean");
      e->lockEngine([&]() {
        if (hasName) s->chanName[ch]=name;
        if (hasAbbrev) s->chanShortName[ch]=abbrev;
        if (hasShow) s->chanShow[ch]=show;
      });
      return json{
        {"subsong",ss},
        {"channel",ch},
        {"name",s->chanName[ch].empty()?e->song.chanDef[ch].name:s->chanName[ch]},
        {"abbrev",s->chanShortName[ch].empty()?e->song.chanDef[ch].shortName:s->chanShortName[ch]},
        {"show",s->chanShow[ch]}
      };
    }
  ));

  // -------------------------------------------------------------------------
  // mute_channel
  m.addTool(FurnaceMCPTool(
    "mute_channel",
    "Mute/unmute a channel. Give 'muted' to set it explicitly, or omit it to toggle.",
    json{{"type","object"},{"properties",{
      {"channel",{{"type","integer"}}},
      {"muted",{{"type","boolean"},{"description","omit to toggle"}}}
    }},{"required",json::array({"channel"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int ch=mcpReqChannel(m,args);
      if (args.contains("muted")) {
        if (!args["muted"].is_boolean()) throw std::runtime_error("muted must be a boolean");
        e->muteChannel(ch,args["muted"].get<bool>());
      } else {
        e->toggleMute(ch);
      }
      return json{{"channel",ch},{"muted",e->isChannelMuted(ch)}};
    }
  ));

  // -------------------------------------------------------------------------
  // solo_channel
  m.addTool(FurnaceMCPTool(
    "solo_channel",
    "Toggle solo on a channel (mutes every other channel; toggling solo again on an already-soloed channel unmutes everyone). Same behavior as the editor's per-channel solo click.",
    json{{"type","object"},{"properties",{
      {"channel",{{"type","integer"}}}
    }},{"required",json::array({"channel"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int ch=mcpReqChannel(m,args);
      e->toggleSolo(ch);
      json muted=json::array();
      for (int i=0; i<e->song.chans; i++) muted.push_back(e->isChannelMuted(i));
      return json{{"channel",ch},{"muted",muted}};
    }
  ));

  // -------------------------------------------------------------------------
  // unmute_all
  m.addTool(FurnaceMCPTool(
    "unmute_all",
    "Unmute every channel.",
    json{{"type","object"},{"properties",json::object()}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      e->unmuteAll();
      json muted=json::array();
      for (int i=0; i<e->song.chans; i++) muted.push_back(e->isChannelMuted(i));
      return json{{"muted",muted}};
    }
  ));

  // -------------------------------------------------------------------------
  // get_mixer
  m.addTool(FurnaceMCPTool(
    "get_mixer",
    "Get mixer state: masterVol, per-chip volume/panning/frontRear (song.systemVol/systemPan/systemPanFR), and the patchbay (patchbayAuto flag + raw 'connections' - opaque packed values, same form song_json's patchbay.connections uses; round-trip them through set_patchbay rather than decoding).",
    json{{"type","object"},{"properties",json::object()}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      json chips=json::array();
      json connections=json::array();
      float masterVol=0;
      bool patchbayAuto=false;
      e->lockEngine([&]() {
        masterVol=e->song.masterVol;
        patchbayAuto=e->song.patchbayAuto;
        for (int i=0; i<e->song.systemLen; i++) {
          chips.push_back(json{
            {"index",i},
            {"volume",e->song.systemVol[i]},
            {"panning",e->song.systemPan[i]},
            {"frontRear",e->song.systemPanFR[i]}
          });
        }
        for (unsigned int c: e->song.patchbay) connections.push_back(c);
      });
      return json{{"masterVol",masterVol},{"chips",chips},{"patchbayAuto",patchbayAuto},{"connections",connections}};
    }
  ));

  // -------------------------------------------------------------------------
  // set_mixer
  m.addTool(FurnaceMCPTool(
    "set_mixer",
    "Set mixer state. 'masterVol' (0..10). 'chips' is a sparse list of {index, volume?, panning?, frontRear?} (volume -10..10, panning/frontRear -1..1). 'patchbayAuto' toggles automatic patchbay routing. Only provided fields change.",
    json{{"type","object"},{"properties",{
      {"masterVol",{{"type","number"}}},
      {"chips",{{"type","array"},{"items",{{"type","object"}}},{"description","[{index, volume?, panning?, frontRear?}]"}}},
      {"patchbayAuto",{{"type","boolean"}}}
    }}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      bool hasMaster=args.contains("masterVol");
      double masterVol=hasMaster?mcpOptFloat(args,"masterVol",e->song.masterVol):0;
      if (hasMaster && (masterVol<0 || masterVol>10)) throw std::runtime_error(fmt::sprintf("masterVol out of range: %g (0..10)",masterVol));
      bool hasAuto=args.contains("patchbayAuto");
      bool patchbayAuto=hasAuto?args["patchbayAuto"].get<bool>():false;
      if (hasAuto && !args["patchbayAuto"].is_boolean()) throw std::runtime_error("patchbayAuto must be a boolean");

      struct ChipWrite { int index; bool hasVol,hasPan,hasFR; float vol,pan,fr; };
      std::vector<ChipWrite> writes;
      if (args.contains("chips")) {
        if (!args["chips"].is_array()) throw std::runtime_error("chips must be an array");
        for (const json& c: args["chips"]) {
          if (!c.is_object() || !c.contains("index")) throw std::runtime_error("each chips[] entry needs an integer 'index'");
          int idx=c["index"].get<int>();
          if (idx<0 || idx>=e->song.systemLen) throw std::runtime_error(fmt::sprintf("chip index out of range: %d (have %d)",idx,e->song.systemLen));
          ChipWrite w{idx,false,false,false,0,0,0};
          if (c.contains("volume")) {
            w.hasVol=true; w.vol=(float)c["volume"].get<double>();
            if (w.vol<-10 || w.vol>10) throw std::runtime_error(fmt::sprintf("chip %d volume out of range: %g (-10..10)",idx,w.vol));
          }
          if (c.contains("panning")) {
            w.hasPan=true; w.pan=(float)c["panning"].get<double>();
            if (w.pan<-1 || w.pan>1) throw std::runtime_error(fmt::sprintf("chip %d panning out of range: %g (-1..1)",idx,w.pan));
          }
          if (c.contains("frontRear")) {
            w.hasFR=true; w.fr=(float)c["frontRear"].get<double>();
            if (w.fr<-1 || w.fr>1) throw std::runtime_error(fmt::sprintf("chip %d frontRear out of range: %g (-1..1)",idx,w.fr));
          }
          writes.push_back(w);
        }
      }

      e->lockEngine([&]() {
        if (hasMaster) e->song.masterVol=(float)masterVol;
        if (hasAuto) e->song.patchbayAuto=patchbayAuto;
        for (ChipWrite& w: writes) {
          if (w.hasVol) e->song.systemVol[w.index]=w.vol;
          if (w.hasPan) e->song.systemPan[w.index]=w.pan;
          if (w.hasFR) e->song.systemPanFR[w.index]=w.fr;
        }
      });

      json chips=json::array();
      e->lockEngine([&]() {
        for (int i=0; i<e->song.systemLen; i++) {
          chips.push_back(json{
            {"index",i},
            {"volume",e->song.systemVol[i]},
            {"panning",e->song.systemPan[i]},
            {"frontRear",e->song.systemPanFR[i]}
          });
        }
      });
      return json{{"masterVol",e->song.masterVol},{"chips",chips},{"patchbayAuto",e->song.patchbayAuto}};
    }
  ));

  // -------------------------------------------------------------------------
  // set_patchbay
  m.addTool(FurnaceMCPTool(
    "set_patchbay",
    "Replace the patchbay's connection list wholesale with 'connections' (a list of raw packed values, the same opaque form get_mixer's 'connections' and song_json's patchbay.connections use - read it from get_mixer, edit sparingly, write it back). Optionally also set 'patchbayAuto'. Setting patchbayAuto true does not recompute connections here; add/remove_system already do that automatically when it's on.",
    json{{"type","object"},{"properties",{
      {"connections",{{"type","array"},{"items",{{"type","integer"}}}}},
      {"patchbayAuto",{{"type","boolean"}}}
    }},{"required",json::array({"connections"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      if (!args.contains("connections") || !args["connections"].is_array()) throw std::runtime_error("missing or non-array argument: connections");
      std::vector<unsigned int> conns;
      for (const json& c: args["connections"]) {
        if (!c.is_number_integer()) throw std::runtime_error("each connection must be an integer");
        long long v=c.get<long long>();
        if (v<0 || v>0xffffffffLL) throw std::runtime_error("connection value out of range for a 32-bit patchbay entry");
        conns.push_back((unsigned int)v);
      }
      bool hasAuto=args.contains("patchbayAuto");
      bool patchbayAuto=hasAuto?args["patchbayAuto"].get<bool>():false;
      if (hasAuto && !args["patchbayAuto"].is_boolean()) throw std::runtime_error("patchbayAuto must be a boolean");
      e->lockEngine([&]() {
        e->song.patchbay=conns;
        if (hasAuto) e->song.patchbayAuto=patchbayAuto;
      });
      json connections=json::array();
      for (unsigned int c: e->song.patchbay) connections.push_back(c);
      return json{{"connections",connections},{"patchbayAuto",e->song.patchbayAuto}};
    }
  ));

  // -------------------------------------------------------------------------
  // get_compat_flags
  m.addTool(FurnaceMCPTool(
    "get_compat_flags",
    "Get the song's compatibility/quirk flags (playback-behavior toggles, e.g. legacy slide/porta/arp quirks). Key set mirrors song_json's compatFlags section exactly.",
    json{{"type","object"},{"properties",json::object()}},
    [](FurnaceMCP& m, const json& args) -> json {
      return mcpCompatFlagsJSON(m.engine()->song.compatFlags);
    }
  ));

  // -------------------------------------------------------------------------
  // set_compat_flags
  m.addTool(FurnaceMCPTool(
    "set_compat_flags",
    "Set one or more compatibility flags (see get_compat_flags for the full key set - it mirrors song_json's compatFlags exactly). Most keys are booleans; a few (linearPitch, pitchSlideSpeed, loopModality, delayBehavior, jumpTreatment) are small integers (0..255). An unknown key is a tool error naming it.",
    json{{"type","object"},{"properties",{
      {"flags",{{"type","object"},{"description","key -> boolean or integer (see get_compat_flags)"}}}
    }},{"required",json::array({"flags"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      if (!args.contains("flags") || !args["flags"].is_object()) throw std::runtime_error("missing or non-object argument: flags");
      DivCompatFlags newFlags=e->song.compatFlags;
      for (auto it=args["flags"].begin(); it!=args["flags"].end(); ++it) {
        const String& key=it.key();
        const json& v=it.value();
        bool matched=false;
#define MCP_CF_SETB(f) if (key==#f) { if(!v.is_boolean()) throw std::runtime_error(fmt::sprintf("compat flag '%s' must be a boolean",key.c_str())); newFlags.f=v.get<bool>(); matched=true; }
        MCP_COMPAT_BOOL_FLAGS(MCP_CF_SETB)
#undef MCP_CF_SETB
        if (!matched) {
#define MCP_CF_SETI(f) if (key==#f) { if(!v.is_number_integer()) throw std::runtime_error(fmt::sprintf("compat flag '%s' must be an integer",key.c_str())); int iv=v.get<int>(); if(iv<0||iv>255) throw std::runtime_error(fmt::sprintf("compat flag '%s' out of range: %d (0..255)",key.c_str(),iv)); newFlags.f=(unsigned char)iv; matched=true; }
          MCP_COMPAT_INT_FLAGS(MCP_CF_SETI)
#undef MCP_CF_SETI
        }
        if (!matched) throw std::runtime_error(fmt::sprintf("unknown compat flag: %s",key.c_str()));
      }
      e->lockEngine([&]() {
        e->song.compatFlags=newFlags;
      });
      return mcpCompatFlagsJSON(e->song.compatFlags);
    }
  ));

  // -------------------------------------------------------------------------
  // new_song
  m.addTool(FurnaceMCPTool(
    "new_song",
    "Reset to a fresh, playable song. With no 'systems', uses the engine's configured default system preset (createNewFromDefaults - same as File > New). With 'systems' (a list of display names/ids), starts from that default and then changes/adds/removes systems until the song's chip list matches the requested one, in order (simplest robust path: it does not try to reuse partially-matching systems beyond a positional diff). 'keepConfig' is accepted for interface completeness but currently has no effect: a song reset only ever touches song data, never the engine's global config (audio device, UI settings, etc), so there is nothing extra for it to preserve either way.",
    json{{"type","object"},{"properties",{
      {"systems",{{"type","array"},{"description","list of system display names or ids"}}},
      {"keepConfig",{{"type","boolean"}}}
    }}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      e->createNewFromDefaults();
      if (args.contains("systems") && !args["systems"].is_null()) {
        if (!args["systems"].is_array()) throw std::runtime_error("systems must be an array");
        std::vector<DivSystem> want;
        for (const json& v: args["systems"]) want.push_back(mcpResolveSystem(v));
        if (want.empty()) throw std::runtime_error("systems must not be empty");

        int cur=e->song.systemLen;
        int n=(int)want.size();
        for (int i=0; i<std::min(cur,n); i++) {
          if (e->song.system[i]!=want[i]) {
            if (!e->changeSystem(i,want[i])) throw std::runtime_error(e->getLastError());
          }
        }
        if (n>cur) {
          for (int i=cur; i<n; i++) {
            if (!e->addSystem(want[i])) throw std::runtime_error(e->getLastError());
          }
        } else if (n<cur) {
          for (int i=cur-1; i>=n; i--) {
            if (!e->removeSystem(i)) throw std::runtime_error(e->getLastError());
          }
        }
      }
      return json{
        {"name",e->song.name},
        {"systems",mcpSystemsJSON(e)},
        {"totalChannels",e->song.chans},
        {"subsongs",(int)e->song.subsong.size()}
      };
    }
  ));

  // -------------------------------------------------------------------------
  // get_config
  m.addTool(FurnaceMCPTool(
    "get_config",
    "Get the engine's persistent config (config.cfg) as a flat key->value map, optionally filtered to keys starting with 'prefix'. Values are the raw strings DivConfig stores internally. Many keys are GUI-cosmetic (window layout, colors, etc) and are inert in headless MCP use.",
    json{{"type","object"},{"properties",{
      {"prefix",{{"type","string"}}}
    }}},
    [](FurnaceMCP& m, const json& args) -> json {
      String prefix=mcpOptStr(args,"prefix","");
      json out=json::object();
      for (auto& kv: m.engine()->getConfObject().configMap()) {
        if (!prefix.empty() && kv.first.compare(0,prefix.size(),prefix)!=0) continue;
        out[kv.first]=kv.second;
      }
      return json{{"values",out}};
    }
  ));

  // -------------------------------------------------------------------------
  // set_config
  m.addTool(FurnaceMCPTool(
    "set_config",
    "Set one or more engine config keys. Give each value with its natural JSON type (boolean/integer/float/string); it's stored the same way the matching DivEngine::setConf() overload would. Does not persist to disk on its own - call save_config afterward if you want it to survive a restart.",
    json{{"type","object"},{"properties",{
      {"values",{{"type","object"},{"description","key -> boolean/integer/number/string"}}}
    }},{"required",json::array({"values"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      if (!args.contains("values") || !args["values"].is_object()) throw std::runtime_error("missing or non-object argument: values");
      for (auto it=args["values"].begin(); it!=args["values"].end(); ++it) {
        const json& v=it.value();
        if (v.is_boolean()) e->setConf(it.key(),v.get<bool>());
        else if (v.is_number_integer()) e->setConf(it.key(),v.get<int>());
        else if (v.is_number_float()) e->setConf(it.key(),(double)v.get<double>());
        else if (v.is_string()) e->setConf(it.key(),v.get<String>());
        else throw std::runtime_error(fmt::sprintf("value for '%s' must be a boolean, number, or string",it.key().c_str()));
      }
      json out=json::object();
      for (auto it=args["values"].begin(); it!=args["values"].end(); ++it) {
        out[it.key()]=e->getConfObject().getString(it.key(),"");
      }
      return json{{"values",out}};
    }
  ));

  // -------------------------------------------------------------------------
  // save_config
  m.addTool(FurnaceMCPTool(
    "save_config",
    "Persist the engine's config (config.cfg) to disk.",
    json{{"type","object"},{"properties",json::object()}},
    [](FurnaceMCP& m, const json& args) -> json {
      if (!m.engine()->saveConf()) throw std::runtime_error("could not save config");
      return json{{"ok",true}};
    }
  ));

  // -------------------------------------------------------------------------
  // checkpoints: headless undo. a checkpoint is a full in-memory .fur
  // snapshot; restore loads it back. this is the headless counterpart of the
  // GUI's undo stack (which lives in FurnaceGUI, not the engine).
  static std::map<String,String>* checkpoints=new std::map<String,String>();

  m.addTool(FurnaceMCPTool(
    "checkpoint_save",
    "Snapshot the full song state in memory (headless undo). Up to 16 named slots; restoring rolls everything back.",
    json{{"type","object"},{"properties",{
      {"slot",{{"type","string"},{"description","slot name (default \"default\")"}}}
    }}},
    [](FurnaceMCP& m, const json& args) -> json {
      String slot=mcpOptStr(args,"slot","default");
      if (checkpoints->find(slot)==checkpoints->end() && checkpoints->size()>=16) {
        throw std::runtime_error("too many checkpoints (max 16) - drop one first");
      }
      SafeWriter* w=m.engine()->saveFur();
      if (w==NULL) throw std::runtime_error("could not serialize song");
      (*checkpoints)[slot]=String((const char*)w->getFinalBuf(),w->size());
      w->finish();
      delete w;
      return json{{"ok",true},{"slot",slot},{"bytes",(int)(*checkpoints)[slot].size()}};
    }
  ));

  m.addTool(FurnaceMCPTool(
    "checkpoint_restore",
    "Restore the song state saved in a checkpoint slot.",
    json{{"type","object"},{"properties",{
      {"slot",{{"type","string"},{"description","slot name (default \"default\")"}}}
    }}},
    [](FurnaceMCP& m, const json& args) -> json {
      String slot=mcpOptStr(args,"slot","default");
      auto it=checkpoints->find(slot);
      if (it==checkpoints->end()) throw std::runtime_error(fmt::sprintf("no checkpoint named %s",slot));
      const String& data=it->second;
      // the engine takes ownership of the buffer on success
      unsigned char* buf=new unsigned char[data.size()];
      memcpy(buf,data.data(),data.size());
      if (!m.engine()->load(buf,data.size(),"checkpoint.fur")) {
        throw std::runtime_error(fmt::sprintf("restore failed: %s",m.engine()->getLastError()));
      }
      return json{{"ok",true},{"slot",slot}};
    }
  ));

  m.addTool(FurnaceMCPTool(
    "list_checkpoints",
    "List saved checkpoint slots.",
    json{{"type","object"},{"properties",json::object()}},
    [](FurnaceMCP& m, const json& args) -> json {
      json out=json::array();
      for (auto& kv: *checkpoints) {
        out.push_back(json{{"slot",kv.first},{"bytes",(int)kv.second.size()}});
      }
      return json{{"checkpoints",out}};
    }
  ));

  m.addTool(FurnaceMCPTool(
    "drop_checkpoint",
    "Delete a checkpoint slot.",
    json{{"type","object"},{"properties",{
      {"slot",{{"type","string"}}}
    }},{"required",json::array({"slot"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      String slot=mcpArgStr(args,"slot");
      if (checkpoints->erase(slot)==0) throw std::runtime_error(fmt::sprintf("no checkpoint named %s",slot));
      return json{{"ok",true}};
    }
  ));
}
