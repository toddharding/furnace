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

// MCP tools: pattern / orders / speeds / grooves domain.
//
// note-encoding contract (mirrors src/engine/pattern.h + fileOps/json.cpp):
//   the pattern note cell (newData[row][DIV_PAT_NOTE]) is a single combined
//   value. json.cpp emits it as a raw integer; these tools additionally decode
//   it to a human string so an agent can read/write notes by name:
//     -1  / 252 (DIV_NOTE_NULL_PAT) -> "..."   (empty)
//     0..179                        -> pitched, name = semitone(v%12) + (v/12-5)
//                                      e.g. 60 -> "C-4"? no: 60 -> "C-0",
//                                      108 -> "C-4", 179 -> "B-9"
//     253 (DIV_NOTE_OFF)            -> "OFF"
//     254 (DIV_NOTE_REL)            -> "REL"
//     255 (DIV_MACRO_REL)           -> "MACRO_REL"
//     251 (DIV_NOTE_RAW)            -> "RAW" (+ rawFreq field)
//   naturals use a '-' filler in the 2-char name ("C-4"), sharps use '#'
//   ("C#4"), so read output round-trips straight back into write input.
//   read_pattern also exposes "noteRaw" (the raw integer) so callers can
//   confirm agreement with song_json's integer note field.

#include "mcp.h"
#include "tools_common.h"
#include "../ta-log.h"

#include <vector>

using nlohmann::json;

// ---------------------------------------------------------------------------
// note-name codec

static const char* mcpNoteNames[12]={
  "C-","C#","D-","D#","E-","F-","F#","G-","G#","A-","A#","B-"
};

// decode a raw note cell value to its string form.
static String mcpNoteToString(int n) {
  if (n==-1 || n==DIV_NOTE_NULL_PAT) return "...";
  if (n==DIV_NOTE_OFF) return "OFF";
  if (n==DIV_NOTE_REL) return "REL";
  if (n==DIV_MACRO_REL) return "MACRO_REL";
  if (n==DIV_NOTE_RAW) return "RAW";
  if (n>=0 && n<=179) {
    return fmt::sprintf("%s%d",mcpNoteNames[n%12],(n/12)-5);
  }
  return "???";
}

// letter -> base semitone (natural). -1 if not A-G.
static int mcpLetterSemitone(char c) {
  switch (c) {
    case 'C': case 'c': return 0;
    case 'D': case 'd': return 2;
    case 'E': case 'e': return 4;
    case 'F': case 'f': return 5;
    case 'G': case 'g': return 7;
    case 'A': case 'a': return 9;
    case 'B': case 'b': return 11;
    default: return -1;
  }
}

// parse a note field (string / integer / null) from a row object into a raw
// cell value, or throw with a clear message. accepted string vocabulary:
//   "..."/"" -> empty(-1), "OFF", "REL", "MACRO_REL", or a pitched name in the
//   same 2-char form read_pattern emits ("C-4"/"C#4"/"C--5"). an integer is
//   taken as the raw cell value (must be -1, 0..179, or 251..255).
static short mcpParseNote(const json& v) {
  if (v.is_null()) return -1;
  if (v.is_number_integer()) {
    int n=v.get<int>();
    if (n==-1) return -1;
    if ((n>=0 && n<=179) || (n>=DIV_NOTE_RAW && n<=DIV_MACRO_REL)) return (short)n;
    throw std::runtime_error(fmt::sprintf("invalid raw note value: %d (expected -1, 0..179, or 251..255)",n));
  }
  if (!v.is_string()) throw std::runtime_error("note must be a string, integer, or null");
  String s=v.get<String>();
  if (s.empty() || s=="...") return -1;
  if (s=="OFF") return DIV_NOTE_OFF;
  if (s=="REL") return DIV_NOTE_REL;
  if (s=="MACRO_REL") return DIV_MACRO_REL;
  // pitched: <letter><accidental><octave>, accidental is '#' or '-'
  if (s.size()<3) throw std::runtime_error(fmt::sprintf("unrecognized note: '%s'",s));
  int semi=mcpLetterSemitone(s[0]);
  if (semi<0) throw std::runtime_error(fmt::sprintf("unrecognized note letter in '%s'",s));
  char acc=s[1];
  if (acc=='#') {
    semi+=1;
  } else if (acc=='-') {
    // natural filler
  } else {
    throw std::runtime_error(fmt::sprintf("note '%s' must have '#' or '-' after the letter",s));
  }
  int octave=atoi(s.c_str()+2);
  int value=(octave+5)*12+semi;
  if (value<0 || value>179) throw std::runtime_error(fmt::sprintf("note '%s' out of range (must map to 0..179)",s));
  return (short)value;
}

// ---------------------------------------------------------------------------
// resolvers / validators

// resolve the target subsong index (defaults to the current one).
static int mcpSubSongIndex(FurnaceMCP& m, const json& args) {
  DivEngine* e=m.engine();
  int idx=mcpOptInt(args,"subsong",(int)e->getCurrentSubSong());
  if (idx<0 || (size_t)idx>=e->song.subsong.size()) {
    throw std::runtime_error(fmt::sprintf("subsong out of range: %d (have %d)",idx,(int)e->song.subsong.size()));
  }
  return idx;
}

static int mcpReqChannel(FurnaceMCP& m, const json& args) {
  int ch=mcpArgInt(args,"channel");
  int chans=m.engine()->song.chans;
  if (ch<0 || ch>=chans) throw std::runtime_error(fmt::sprintf("channel out of range: %d (have %d)",ch,chans));
  return ch;
}

// resolve a pattern slot for a channel from either "index" (a pattern slot) or
// "order" (an order-list position, mapped through the order matrix).
static int mcpResolvePattern(DivSubSong* s, int channel, const json& args) {
  bool hasIndex=args.contains("index") && !args["index"].is_null();
  bool hasOrder=args.contains("order") && !args["order"].is_null();
  if (hasIndex && hasOrder) throw std::runtime_error("provide either 'index' or 'order', not both");
  if (!hasIndex && !hasOrder) throw std::runtime_error("provide 'index' (pattern slot) or 'order' (order-list position)");
  if (hasOrder) {
    int ord=mcpArgInt(args,"order");
    if (ord<0 || ord>=s->ordersLen) throw std::runtime_error(fmt::sprintf("order out of range: %d (ordersLen %d)",ord,s->ordersLen));
    return s->orders.ord[channel][ord];
  }
  int idx=mcpArgInt(args,"index");
  if (idx<0 || idx>=DIV_MAX_PATTERNS) throw std::runtime_error(fmt::sprintf("pattern index out of range: %d (0..%d)",idx,DIV_MAX_PATTERNS-1));
  return idx;
}

// build a row JSON object from a pattern's cell data. caller holds the lock.
static json mcpRowJSON(DivPattern* p, int row, int effectCols) {
  short* nd=p->newData[row];
  json r;
  r["row"]=row;
  r["note"]=mcpNoteToString(nd[DIV_PAT_NOTE]);
  r["noteRaw"]=(int)nd[DIV_PAT_NOTE];
  if (nd[DIV_PAT_NOTE]==DIV_NOTE_RAW) {
    unsigned int freq=
      (unsigned short)nd[DIV_PAT_RAW0]|
      ((unsigned short)nd[DIV_PAT_RAW1]<<8)|
      ((unsigned short)nd[DIV_PAT_RAW2]<<16)|
      ((unsigned short)nd[DIV_PAT_RAW3]<<24);
    r["rawFreq"]=freq;
  }
  r["ins"]=(int)nd[DIV_PAT_INS];
  r["vol"]=(int)nd[DIV_PAT_VOL];
  json fx=json::array();
  for (int j=0; j<effectCols; j++) {
    fx.push_back(json::array({(int)nd[DIV_PAT_FX(j)],(int)nd[DIV_PAT_FXVAL(j)]}));
  }
  r["effects"]=fx;
  return r;
}

// parse an ins/vol field: null -> -1, integer -1..255.
static short mcpParseByteField(const json& v, const char* what) {
  if (v.is_null()) return -1;
  if (!v.is_number_integer()) throw std::runtime_error(fmt::sprintf("%s must be an integer or null",what));
  int n=v.get<int>();
  if (n<-1 || n>255) throw std::runtime_error(fmt::sprintf("%s out of range: %d (-1..255)",what,n));
  return (short)n;
}

// ---------------------------------------------------------------------------

void registerPatternTools(FurnaceMCP& m) {
  // -------------------------------------------------------------------------
  // read_pattern
  m.addTool(FurnaceMCPTool(
    "read_pattern",
    "Read rows of a channel's pattern. Identify the pattern by 'index' (a pattern slot 0..255) OR 'order' (an order-list position, mapped through the order matrix). Returns rows with note (string form: \"C-4\"/\"C#4\"/\"OFF\"/\"REL\"/\"MACRO_REL\"/\"...\"), noteRaw (the raw integer, agrees with song_json's note), ins, vol, and effects as [[code,value],...] (one pair per effect column, -1 = empty).",
    json{{"type","object"},{"properties",{
      {"subsong",{{"type","integer"},{"description","subsong index (default: current)"}}},
      {"channel",{{"type","integer"}}},
      {"index",{{"type","integer"},{"description","pattern slot 0..255"}}},
      {"order",{{"type","integer"},{"description","order-list position (alternative to index)"}}},
      {"rowFrom",{{"type","integer"},{"description","first row (default 0)"}}},
      {"rowTo",{{"type","integer"},{"description","last row inclusive (default patLen-1)"}}}
    }},{"required",json::array({"channel"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int ssIdx=mcpSubSongIndex(m,args);
      DivSubSong* s=e->song.subsong[ssIdx];
      int channel=mcpReqChannel(m,args);
      int patIdx=mcpResolvePattern(s,channel,args);
      int patLen=s->patLen;
      int rowFrom=mcpOptInt(args,"rowFrom",0);
      int rowTo=mcpOptInt(args,"rowTo",patLen-1);
      if (rowFrom<0 || rowFrom>=patLen) throw std::runtime_error(fmt::sprintf("rowFrom out of range: %d (0..%d)",rowFrom,patLen-1));
      if (rowTo<0 || rowTo>=patLen) throw std::runtime_error(fmt::sprintf("rowTo out of range: %d (0..%d)",rowTo,patLen-1));
      if (rowFrom>rowTo) throw std::runtime_error("rowFrom must be <= rowTo");

      json rows=json::array();
      int effectCols=0;
      e->lockEngine([&]() {
        effectCols=s->pat[channel].effectCols;
        DivPattern* p=s->pat[channel].getPattern(patIdx,false);
        for (int r=rowFrom; r<=rowTo; r++) {
          rows.push_back(mcpRowJSON(p,r,effectCols));
        }
      });
      return json{
        {"subsong",ssIdx},
        {"channel",channel},
        {"pattern",patIdx},
        {"patLen",patLen},
        {"effectCols",effectCols},
        {"rowFrom",rowFrom},
        {"rowTo",rowTo},
        {"rows",rows}
      };
    }
  ));

  // -------------------------------------------------------------------------
  // write_pattern
  m.addTool(FurnaceMCPTool(
    "write_pattern",
    "Bulk-write cells into a channel's pattern (identified by 'index' or 'order'). Each entry in 'rows' is {row, note?, ins?, vol?, effects?}: a field present sets that cell, a field absent leaves it untouched. note accepts the read_pattern vocabulary (\"C-4\"/\"C#4\"/\"OFF\"/\"REL\"/\"MACRO_REL\"/\"...\"/null to clear, or a raw integer). ins/vol are integers (-1 or null to clear). effects is an array of [code,value] pairs (or null to clear a column) indexed by effect column; its length must not exceed the channel's effect-column count. Returns the written rows re-read from the data.",
    json{{"type","object"},{"properties",{
      {"subsong",{{"type","integer"},{"description","subsong index (default: current)"}}},
      {"channel",{{"type","integer"}}},
      {"index",{{"type","integer"},{"description","pattern slot 0..255"}}},
      {"order",{{"type","integer"},{"description","order-list position (alternative to index)"}}},
      {"rows",{{"type","array"},{"description","list of {row, note?, ins?, vol?, effects?}"},{"items",{{"type","object"}}}}}
    }},{"required",json::array({"channel","rows"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int ssIdx=mcpSubSongIndex(m,args);
      DivSubSong* s=e->song.subsong[ssIdx];
      int channel=mcpReqChannel(m,args);
      int patIdx=mcpResolvePattern(s,channel,args);
      int patLen=s->patLen;
      int effectCols=s->pat[channel].effectCols;
      if (!args.contains("rows") || !args["rows"].is_array()) throw std::runtime_error("missing or non-array argument: rows");
      const json& rows=args["rows"];

      // validate everything up front so a bad batch touches nothing.
      struct CellWrite {
        int row;
        bool setNote,setIns,setVol;
        short note,ins,vol;
        std::vector<std::pair<bool,std::pair<short,short>>> fx; // (set?, (code,val)) per column
      };
      std::vector<CellWrite> writes;
      std::vector<int> touchedRows;
      for (const json& rowObj: rows) {
        if (!rowObj.is_object()) throw std::runtime_error("each rows[] entry must be an object");
        if (!rowObj.contains("row") || !rowObj["row"].is_number_integer()) throw std::runtime_error("each rows[] entry needs an integer 'row'");
        int row=rowObj["row"].get<int>();
        if (row<0 || row>=patLen) throw std::runtime_error(fmt::sprintf("row out of range: %d (0..%d, patLen=%d)",row,patLen-1,patLen));
        CellWrite cw;
        cw.row=row;
        cw.setNote=cw.setIns=cw.setVol=false;
        cw.note=cw.ins=cw.vol=-1;
        if (rowObj.contains("note")) { cw.setNote=true; cw.note=mcpParseNote(rowObj["note"]); }
        if (rowObj.contains("ins")) { cw.setIns=true; cw.ins=mcpParseByteField(rowObj["ins"],"ins"); }
        if (rowObj.contains("vol")) { cw.setVol=true; cw.vol=mcpParseByteField(rowObj["vol"],"vol"); }
        if (rowObj.contains("effects")) {
          const json& fx=rowObj["effects"];
          if (!fx.is_array()) throw std::runtime_error("'effects' must be an array of [code,value] pairs");
          if ((int)fx.size()>effectCols) throw std::runtime_error(fmt::sprintf("row %d gives %d effect columns but the channel has %d (raise it with set_effect_columns)",row,(int)fx.size(),effectCols));
          for (const json& pair: fx) {
            if (pair.is_null()) {
              cw.fx.push_back({true,{-1,-1}});
              continue;
            }
            if (!pair.is_array() || pair.size()!=2) throw std::runtime_error("each effect must be a [code,value] pair or null");
            short code=mcpParseByteField(pair[0],"effect code");
            short val=mcpParseByteField(pair[1],"effect value");
            cw.fx.push_back({true,{code,val}});
          }
        }
        writes.push_back(cw);
        touchedRows.push_back(row);
      }

      // apply under the engine lock (direct pattern-cell writes).
      e->lockEngine([&]() {
        DivPattern* p=s->pat[channel].getPattern(patIdx,true);
        for (CellWrite& cw: writes) {
          short* nd=p->newData[cw.row];
          if (cw.setNote) nd[DIV_PAT_NOTE]=cw.note;
          if (cw.setIns) nd[DIV_PAT_INS]=cw.ins;
          if (cw.setVol) nd[DIV_PAT_VOL]=cw.vol;
          for (size_t j=0; j<cw.fx.size(); j++) {
            if (!cw.fx[j].first) continue;
            nd[DIV_PAT_FX((int)j)]=cw.fx[j].second.first;
            nd[DIV_PAT_FXVAL((int)j)]=cw.fx[j].second.second;
          }
        }
      });

      // read back the written rows from the data.
      json readback=json::array();
      e->lockEngine([&]() {
        DivPattern* p=s->pat[channel].getPattern(patIdx,false);
        for (int row: touchedRows) {
          readback.push_back(mcpRowJSON(p,row,effectCols));
        }
      });
      return json{
        {"subsong",ssIdx},
        {"channel",channel},
        {"pattern",patIdx},
        {"written",(int)writes.size()},
        {"rows",readback}
      };
    }
  ));

  // -------------------------------------------------------------------------
  // get_pattern_meta
  m.addTool(FurnaceMCPTool(
    "get_pattern_meta",
    "Get a subsong's pattern/order dimensions: patLen (rows per pattern), ordersLen (length of the order list), highlights [A,B], timeBase (raw effectDivider), plus channel count and tick rate.",
    json{{"type","object"},{"properties",{
      {"subsong",{{"type","integer"},{"description","subsong index (default: current)"}}}
    }}},
    [](FurnaceMCP& m, const json& args) -> json {
      int ssIdx=mcpSubSongIndex(m,args);
      DivSubSong* s=m.engine()->song.subsong[ssIdx];
      return json{
        {"subsong",ssIdx},
        {"patLen",s->patLen},
        {"ordersLen",s->ordersLen},
        {"highlights",json::array({(int)s->hilightA,(int)s->hilightB})},
        {"timeBase",(int)s->effectDivider},
        {"channels",m.engine()->song.chans},
        {"tickRate",s->hz}
      };
    }
  ));

  // -------------------------------------------------------------------------
  // set_pattern_meta
  m.addTool(FurnaceMCPTool(
    "set_pattern_meta",
    "Set a subsong's pattern/order dimensions. Any of: patLen (1..256), ordersLen (1..256), highlights [A,B] (0..255 each), timeBase (raw effectDivider, 0..255). Only provided fields change.",
    json{{"type","object"},{"properties",{
      {"subsong",{{"type","integer"},{"description","subsong index (default: current)"}}},
      {"patLen",{{"type","integer"},{"description","rows per pattern, 1..256"}}},
      {"ordersLen",{{"type","integer"},{"description","order-list length, 1..256"}}},
      {"highlights",{{"type","array"},{"description","[A,B], 0..255 each"},{"items",{{"type","integer"}}}}},
      {"timeBase",{{"type","integer"},{"description","raw effectDivider, 0..255"}}}
    }}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int ssIdx=mcpSubSongIndex(m,args);
      DivSubSong* s=e->song.subsong[ssIdx];

      bool hasPatLen=args.contains("patLen");
      bool hasOrdLen=args.contains("ordersLen");
      bool hasHi=args.contains("highlights");
      bool hasTB=args.contains("timeBase");

      int patLen=s->patLen, ordersLen=s->ordersLen, hiA=s->hilightA, hiB=s->hilightB, tb=s->effectDivider;
      if (hasPatLen) {
        patLen=mcpArgInt(args,"patLen");
        if (patLen<1 || patLen>DIV_MAX_ROWS) throw std::runtime_error(fmt::sprintf("patLen out of range: %d (1..%d)",patLen,DIV_MAX_ROWS));
      }
      if (hasOrdLen) {
        ordersLen=mcpArgInt(args,"ordersLen");
        if (ordersLen<1 || ordersLen>DIV_MAX_PATTERNS) throw std::runtime_error(fmt::sprintf("ordersLen out of range: %d (1..%d)",ordersLen,DIV_MAX_PATTERNS));
      }
      if (hasHi) {
        if (!args["highlights"].is_array() || args["highlights"].size()!=2) throw std::runtime_error("highlights must be a [A,B] array");
        hiA=args["highlights"][0].get<int>();
        hiB=args["highlights"][1].get<int>();
        if (hiA<0 || hiA>255 || hiB<0 || hiB>255) throw std::runtime_error("highlights must be 0..255");
      }
      if (hasTB) {
        tb=mcpArgInt(args,"timeBase");
        if (tb<0 || tb>255) throw std::runtime_error(fmt::sprintf("timeBase out of range: %d (0..255)",tb));
      }

      e->lockEngine([&]() {
        if (hasPatLen) s->patLen=patLen;
        if (hasOrdLen) s->ordersLen=ordersLen;
        if (hasHi) { s->hilightA=(unsigned char)hiA; s->hilightB=(unsigned char)hiB; }
        if (hasTB) s->effectDivider=(unsigned char)tb;
      });
      // keep the play cursor valid if we shrank the order list of the live subsong
      if (hasOrdLen && ssIdx==(int)e->getCurrentSubSong() && e->getOrder()>=ordersLen) {
        e->setOrder(ordersLen-1);
      }
      return json{
        {"subsong",ssIdx},
        {"patLen",s->patLen},
        {"ordersLen",s->ordersLen},
        {"highlights",json::array({(int)s->hilightA,(int)s->hilightB})},
        {"timeBase",(int)s->effectDivider}
      };
    }
  ));

  // -------------------------------------------------------------------------
  // read_orders
  m.addTool(FurnaceMCPTool(
    "read_orders",
    "Read the full order matrix of a subsong. 'orders' is row-major: orders[orderPosition][channel] is the pattern slot played by that channel at that order position (rows = ordersLen, columns = channel count). This is the orientation write_orders expects back.",
    json{{"type","object"},{"properties",{
      {"subsong",{{"type","integer"},{"description","subsong index (default: current)"}}}
    }}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int ssIdx=mcpSubSongIndex(m,args);
      DivSubSong* s=e->song.subsong[ssIdx];
      int chans=e->song.chans;
      int ordersLen=s->ordersLen;
      json matrix=json::array();
      e->lockEngine([&]() {
        for (int o=0; o<ordersLen; o++) {
          json row=json::array();
          for (int c=0; c<chans; c++) row.push_back((int)s->orders.ord[c][o]);
          matrix.push_back(row);
        }
      });
      return json{
        {"subsong",ssIdx},
        {"ordersLen",ordersLen},
        {"channels",chans},
        {"orders",matrix}
      };
    }
  ));

  // -------------------------------------------------------------------------
  // write_orders
  m.addTool(FurnaceMCPTool(
    "write_orders",
    "Replace the full order matrix of a subsong. 'orders' is row-major (same shape read_orders returns): each inner array is one order position with one pattern slot (0..255) per channel; its length must equal the channel count. The number of rows becomes the new ordersLen (1..256).",
    json{{"type","object"},{"properties",{
      {"subsong",{{"type","integer"},{"description","subsong index (default: current)"}}},
      {"orders",{{"type","array"},{"description","row-major matrix [orderPosition][channel] of pattern slots"},{"items",{{"type","array"},{"items",{{"type","integer"}}}}}}}
    }},{"required",json::array({"orders"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int ssIdx=mcpSubSongIndex(m,args);
      DivSubSong* s=e->song.subsong[ssIdx];
      int chans=e->song.chans;
      if (!args.contains("orders") || !args["orders"].is_array()) throw std::runtime_error("missing or non-array argument: orders");
      const json& matrix=args["orders"];
      int newLen=(int)matrix.size();
      if (newLen<1 || newLen>DIV_MAX_PATTERNS) throw std::runtime_error(fmt::sprintf("orders must have 1..%d rows (got %d)",DIV_MAX_PATTERNS,newLen));
      // validate the whole matrix first
      std::vector<std::vector<int>> parsed;
      parsed.reserve(newLen);
      for (int o=0; o<newLen; o++) {
        const json& row=matrix[o];
        if (!row.is_array() || (int)row.size()!=chans) throw std::runtime_error(fmt::sprintf("order row %d must have exactly %d entries (one per channel)",o,chans));
        std::vector<int> vals;
        vals.reserve(chans);
        for (int c=0; c<chans; c++) {
          if (!row[c].is_number_integer()) throw std::runtime_error(fmt::sprintf("order[%d][%d] must be an integer",o,c));
          int v=row[c].get<int>();
          if (v<0 || v>255) throw std::runtime_error(fmt::sprintf("order[%d][%d] out of range: %d (0..255)",o,c,v));
          vals.push_back(v);
        }
        parsed.push_back(vals);
      }
      e->lockEngine([&]() {
        for (int c=0; c<chans; c++) {
          for (int o=0; o<newLen; o++) {
            s->orders.ord[c][o]=(unsigned char)parsed[o][c];
          }
        }
        s->ordersLen=newLen;
      });
      if (ssIdx==(int)e->getCurrentSubSong() && e->getOrder()>=newLen) {
        e->setOrder(newLen-1);
      }
      return json{{"subsong",ssIdx},{"ordersLen",newLen},{"channels",chans}};
    }
  ));

  // -------------------------------------------------------------------------
  // order_ops (operates on the CURRENT subsong via the engine's order verbs)
  m.addTool(FurnaceMCPTool(
    "order_ops",
    "Structural order-list edit on the CURRENT subsong. op is one of: add (insert a blank order after 'at'), duplicate (insert a copy after 'at'), deep_clone (copy after 'at', cloning the patterns too), duplicate_end / deep_clone_end (same but appended at the end), delete (remove order 'at'), move_up / move_down (swap order 'at' with its neighbour). 'at' defaults to the current order. Returns the new ordersLen and current order.",
    json{{"type","object"},{"properties",{
      {"op",{{"type","string"},{"enum",json::array({"add","duplicate","deep_clone","duplicate_end","deep_clone_end","delete","move_up","move_down"})}}},
      {"at",{{"type","integer"},{"description","order-list position to act on (default: current order)"}}}
    }},{"required",json::array({"op"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      String op=mcpArgStr(args,"op");
      int ordersLen=e->curSubSong->ordersLen;
      int at=mcpOptInt(args,"at",(int)e->getOrder());
      if (at<0 || at>=ordersLen) throw std::runtime_error(fmt::sprintf("'at' out of range: %d (0..%d)",at,ordersLen-1));

      // these verbs are internally locked; call them directly (never wrapped).
      if (op=="add") {
        e->addOrder(at,false,false);
      } else if (op=="duplicate") {
        e->addOrder(at,true,false);
      } else if (op=="deep_clone") {
        e->deepCloneOrder(at,false);
      } else if (op=="duplicate_end") {
        e->addOrder(at,true,true);
      } else if (op=="deep_clone_end") {
        e->deepCloneOrder(at,true);
      } else if (op=="delete") {
        e->deleteOrder(at);
      } else if (op=="move_up") {
        int pos=at; e->moveOrderUp(pos);
      } else if (op=="move_down") {
        int pos=at; e->moveOrderDown(pos);
      } else {
        throw std::runtime_error(fmt::sprintf("unknown op: %s",op));
      }
      json res{
        {"op",op},
        {"ordersLen",e->curSubSong->ordersLen},
        {"order",(int)e->getOrder()}
      };
      String warn=e->getWarnings();
      if (!warn.empty()) res["warning"]=warn;
      return res;
    }
  ));

  // -------------------------------------------------------------------------
  // get_speeds
  m.addTool(FurnaceMCPTool(
    "get_speeds",
    "Get a subsong's timing: the groove-pattern 'speeds' list (ticks per row, alternating/looping), virtualTempoN/D (virtual tempo ratio), and hz (tick rate).",
    json{{"type","object"},{"properties",{
      {"subsong",{{"type","integer"},{"description","subsong index (default: current)"}}}
    }}},
    [](FurnaceMCP& m, const json& args) -> json {
      int ssIdx=mcpSubSongIndex(m,args);
      DivSubSong* s=m.engine()->song.subsong[ssIdx];
      json speeds=json::array();
      int len=s->speeds.len; if (len>16) len=16;
      for (int i=0; i<len; i++) speeds.push_back((int)s->speeds.val[i]);
      return json{
        {"subsong",ssIdx},
        {"speeds",speeds},
        {"virtualTempoN",(int)s->virtualTempoN},
        {"virtualTempoD",(int)s->virtualTempoD},
        {"hz",s->hz}
      };
    }
  ));

  // -------------------------------------------------------------------------
  // set_speeds
  m.addTool(FurnaceMCPTool(
    "set_speeds",
    "Set a subsong's timing. Any of: speeds (array of 1..16 tick counts, each 1..512), virtualTempoN (1..255), virtualTempoD (1..255), hz (tick rate 1..999). Only provided fields change.",
    json{{"type","object"},{"properties",{
      {"subsong",{{"type","integer"},{"description","subsong index (default: current)"}}},
      {"speeds",{{"type","array"},{"description","1..16 tick-per-row values, each 1..512"},{"items",{{"type","integer"}}}}},
      {"virtualTempoN",{{"type","integer"},{"description","virtual tempo numerator 1..255"}}},
      {"virtualTempoD",{{"type","integer"},{"description","virtual tempo denominator 1..255"}}},
      {"hz",{{"type","number"},{"description","tick rate in Hz, 1..999"}}}
    }}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int ssIdx=mcpSubSongIndex(m,args);
      DivSubSong* s=e->song.subsong[ssIdx];
      bool isCurrent=(ssIdx==(int)e->getCurrentSubSong());

      bool hasSpeeds=args.contains("speeds");
      bool hasN=args.contains("virtualTempoN");
      bool hasD=args.contains("virtualTempoD");
      bool hasHz=args.contains("hz");

      std::vector<int> speeds;
      if (hasSpeeds) {
        if (!args["speeds"].is_array()) throw std::runtime_error("speeds must be an array");
        int len=(int)args["speeds"].size();
        if (len<1 || len>16) throw std::runtime_error(fmt::sprintf("speeds must have 1..16 entries (got %d)",len));
        for (const json& v: args["speeds"]) {
          if (!v.is_number_integer()) throw std::runtime_error("each speed must be an integer");
          int sv=v.get<int>();
          if (sv<1 || sv>512) throw std::runtime_error(fmt::sprintf("speed out of range: %d (1..512)",sv));
          speeds.push_back(sv);
        }
      }
      int vN=s->virtualTempoN, vD=s->virtualTempoD;
      if (hasN) {
        vN=mcpArgInt(args,"virtualTempoN");
        if (vN<1 || vN>255) throw std::runtime_error(fmt::sprintf("virtualTempoN out of range: %d (1..255)",vN));
      }
      if (hasD) {
        vD=mcpArgInt(args,"virtualTempoD");
        if (vD<1 || vD>255) throw std::runtime_error(fmt::sprintf("virtualTempoD out of range: %d (1..255)",vD));
      }
      double hz=s->hz;
      if (hasHz) {
        hz=mcpOptFloat(args,"hz",s->hz);
        if (hz<1 || hz>999) throw std::runtime_error(fmt::sprintf("hz out of range: %g (1..999)",hz));
      }

      // direct field writes under the lock (speeds list + virtual tempo, plus
      // hz when the target is not the live subsong).
      e->lockEngine([&]() {
        if (hasSpeeds) {
          s->speeds.len=(unsigned short)speeds.size();
          for (size_t i=0; i<speeds.size() && i<16; i++) s->speeds.val[i]=(unsigned short)speeds[i];
        }
        if (hasN) s->virtualTempoN=(short)vN;
        if (hasD) s->virtualTempoD=(short)vD;
        if (hasHz && !isCurrent) s->hz=(float)hz;
      });
      // for the live subsong, use the locked verbs so the engine picks up the
      // change immediately.
      if (isCurrent) {
        if (hasN || hasD) e->virtualTempoChanged();
        if (hasHz) e->setSongRate((float)hz);
      }

      json outSpeeds=json::array();
      int len=s->speeds.len; if (len>16) len=16;
      for (int i=0; i<len; i++) outSpeeds.push_back((int)s->speeds.val[i]);
      return json{
        {"subsong",ssIdx},
        {"speeds",outSpeeds},
        {"virtualTempoN",(int)s->virtualTempoN},
        {"virtualTempoD",(int)s->virtualTempoD},
        {"hz",s->hz}
      };
    }
  ));

  // -------------------------------------------------------------------------
  // get_grooves
  m.addTool(FurnaceMCPTool(
    "get_grooves",
    "Get the song's groove list (selected in patterns via effect 09xx). Each groove is an array of tick-per-row values.",
    json{{"type","object"},{"properties",json::object()}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      json grooves=json::array();
      e->lockEngine([&]() {
        for (DivGroovePattern& g: e->song.grooves) {
          json one=json::array();
          int len=g.len; if (len>16) len=16;
          for (int i=0; i<len; i++) one.push_back((int)g.val[i]);
          grooves.push_back(one);
        }
      });
      return json{{"grooves",grooves}};
    }
  ));

  // -------------------------------------------------------------------------
  // set_grooves
  m.addTool(FurnaceMCPTool(
    "set_grooves",
    "Replace the song's groove list. 'grooves' is an array of grooves; each groove is an array of 1..16 tick-per-row values (each 1..255).",
    json{{"type","object"},{"properties",{
      {"grooves",{{"type","array"},{"description","array of grooves, each an array of 1..16 values (1..255)"},{"items",{{"type","array"},{"items",{{"type","integer"}}}}}}}
    }},{"required",json::array({"grooves"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      if (!args.contains("grooves") || !args["grooves"].is_array()) throw std::runtime_error("missing or non-array argument: grooves");
      const json& list=args["grooves"];
      std::vector<DivGroovePattern> parsed;
      for (const json& g: list) {
        if (!g.is_array()) throw std::runtime_error("each groove must be an array of values");
        int len=(int)g.size();
        if (len<1 || len>16) throw std::runtime_error(fmt::sprintf("each groove must have 1..16 values (got %d)",len));
        DivGroovePattern gp;
        gp.len=(unsigned short)len;
        for (int i=0; i<16; i++) gp.val[i]=6;
        for (int i=0; i<len; i++) {
          if (!g[i].is_number_integer()) throw std::runtime_error("each groove value must be an integer");
          int v=g[i].get<int>();
          if (v<1 || v>255) throw std::runtime_error(fmt::sprintf("groove value out of range: %d (1..255)",v));
          gp.val[i]=(unsigned short)v;
        }
        parsed.push_back(gp);
      }
      e->lockEngine([&]() {
        e->song.grooves=parsed;
      });
      return json{{"grooves",(int)parsed.size()}};
    }
  ));

  // -------------------------------------------------------------------------
  // set_effect_columns
  m.addTool(FurnaceMCPTool(
    "set_effect_columns",
    "Set how many effect columns a channel shows/stores in a subsong (1..8).",
    json{{"type","object"},{"properties",{
      {"subsong",{{"type","integer"},{"description","subsong index (default: current)"}}},
      {"channel",{{"type","integer"}}},
      {"count",{{"type","integer"},{"description","effect column count, 1..8"}}}
    }},{"required",json::array({"channel","count"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      DivEngine* e=m.engine();
      int ssIdx=mcpSubSongIndex(m,args);
      DivSubSong* s=e->song.subsong[ssIdx];
      int channel=mcpReqChannel(m,args);
      int count=mcpArgInt(args,"count");
      if (count<1 || count>DIV_MAX_EFFECTS) throw std::runtime_error(fmt::sprintf("count out of range: %d (1..%d)",count,DIV_MAX_EFFECTS));
      e->lockEngine([&]() {
        s->pat[channel].effectCols=(unsigned char)count;
      });
      return json{{"subsong",ssIdx},{"channel",channel},{"effectCols",count}};
    }
  ));

  m.addTool(FurnaceMCPTool(
    "list_effects",
    "Enumerate every pattern effect the given channel understands (what the Effect List window shows): code, hex, and description. Effects are chip- and channel-specific - always consult this before writing effect columns.",
    nlohmann::json{{"type","object"},{"properties",{
      {"channel",{{"type","integer"},{"description","channel whose effect vocabulary to list (default 0)"}}}
    }}},
    [](FurnaceMCP& m, const nlohmann::json& args) -> nlohmann::json {
      DivEngine* e=m.engine();
      int chan=mcpOptInt(args,"channel",0);
      if (chan<0 || chan>=e->getTotalChannelCount()) throw std::runtime_error("channel out of range");
      nlohmann::json effects=nlohmann::json::array();
      for (int fx=0; fx<256; fx++) {
        const char* desc=e->getEffectDesc((unsigned char)fx,chan,false);
        if (desc==NULL) continue;
        char hex[4];
        snprintf(hex,sizeof(hex),"%02X",fx);
        effects.push_back(nlohmann::json{
          {"code",fx},
          {"hex",hex},
          {"description",desc}
        });
      }
      return nlohmann::json{{"channel",chan},{"effects",effects}};
    }
  ));
}
