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

// MCP window mode (--mcp-window): serve MCP over TCP while the normal GUI
// window runs. Every request is marshalled to the GUI thread at a frame
// boundary (see furnaceMCPWindowPump, called from FurnaceGUI::loop), so ALL
// existing tools operate safely against the live GUI session. This file also
// owns the window-only tools (screenshot, window management, gui_action,
// cursor/selection/edit-control get/set, undo/redo) and the GUI-side accessor
// implementations they use.

#include "mcp.h"
#include "tools_common.h"
#include "../ta-log.h"
#include "../baseutils.h"
#include "../gui/gui.h"
#include "../gui/guiConst.h"

#include <atomic>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#include <chrono>
#include <string>
#include <vector>

#include <zlib.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <SDL_syswm.h>
typedef SOCKET MCPSocket;
#define MCP_INVALID_SOCKET INVALID_SOCKET
#define mcpCloseSocket closesocket
#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int MCPSocket;
#define MCP_INVALID_SOCKET (-1)
#define mcpCloseSocket close
#endif

using nlohmann::json;

// ===========================================================================
// GUI-side accessors (run on the GUI thread via the marshalling pump).
// ===========================================================================

// window name (GUI_WINDOW_* suffix, lowercased) -> enum. only entries that
// have an "open" bool are listed here.
struct MCPWindowName {
  const char* name;
  int win;
};

static const MCPWindowName mcpWindowNames[]={
  {"edit_controls", GUI_WINDOW_EDIT_CONTROLS},
  {"song_info",     GUI_WINDOW_SONG_INFO},
  {"speed",         GUI_WINDOW_SPEED},
  {"orders",        GUI_WINDOW_ORDERS},
  {"ins_list",      GUI_WINDOW_INS_LIST},
  {"pattern",       GUI_WINDOW_PATTERN},
  {"ins_edit",      GUI_WINDOW_INS_EDIT},
  {"wave_list",     GUI_WINDOW_WAVE_LIST},
  {"wave_edit",     GUI_WINDOW_WAVE_EDIT},
  {"sample_list",   GUI_WINDOW_SAMPLE_LIST},
  {"sample_edit",   GUI_WINDOW_SAMPLE_EDIT},
  {"mixer",         GUI_WINDOW_MIXER},
  {"about",         GUI_WINDOW_ABOUT},
  {"settings",      GUI_WINDOW_SETTINGS},
  {"debug",         GUI_WINDOW_DEBUG},
  {"oscilloscope",  GUI_WINDOW_OSCILLOSCOPE},
  {"vol_meter",     GUI_WINDOW_VOL_METER},
  {"stats",         GUI_WINDOW_STATS},
  {"compat_flags",  GUI_WINDOW_COMPAT_FLAGS},
  {"piano",         GUI_WINDOW_PIANO},
  {"notes",         GUI_WINDOW_NOTES},
  {"tuner",         GUI_WINDOW_TUNER},
  {"spectrum",      GUI_WINDOW_SPECTRUM},
  {"channels",      GUI_WINDOW_CHANNELS},
  {"pat_manager",   GUI_WINDOW_PAT_MANAGER},
  {"sys_manager",   GUI_WINDOW_SYS_MANAGER},
  {"register_view", GUI_WINDOW_REGISTER_VIEW},
  {"log",           GUI_WINDOW_LOG},
  {"effect_list",   GUI_WINDOW_EFFECT_LIST},
  {"chan_osc",      GUI_WINDOW_CHAN_OSC},
  {"subsongs",      GUI_WINDOW_SUBSONGS},
  {"find",          GUI_WINDOW_FIND},
  {"clock",         GUI_WINDOW_CLOCK},
  {"grooves",       GUI_WINDOW_GROOVES},
  {"xy_osc",        GUI_WINDOW_XY_OSC},
  {"memory",        GUI_WINDOW_MEMORY},
  {"cs_player",     GUI_WINDOW_CS_PLAYER},
  {"user_presets",  GUI_WINDOW_USER_PRESETS},
  {"ref_player",    GUI_WINDOW_REF_PLAYER},
  {"multi_ins_setup", GUI_WINDOW_MULTI_INS_SETUP},
  {"backups_manager", GUI_WINDOW_BACKUPS_MANAGER},
  {NULL,0}
};

bool* FurnaceGUI::mcpWindowOpenFlag(int w) {
  switch ((FurnaceGUIWindows)w) {
    case GUI_WINDOW_EDIT_CONTROLS: return &editControlsOpen;
    case GUI_WINDOW_SONG_INFO: return &songInfoOpen;
    case GUI_WINDOW_SPEED: return &speedOpen;
    case GUI_WINDOW_ORDERS: return &ordersOpen;
    case GUI_WINDOW_INS_LIST: return &insListOpen;
    case GUI_WINDOW_PATTERN: return &patternOpen;
    case GUI_WINDOW_INS_EDIT: return &insEditOpen;
    case GUI_WINDOW_WAVE_LIST: return &waveListOpen;
    case GUI_WINDOW_WAVE_EDIT: return &waveEditOpen;
    case GUI_WINDOW_SAMPLE_LIST: return &sampleListOpen;
    case GUI_WINDOW_SAMPLE_EDIT: return &sampleEditOpen;
    case GUI_WINDOW_MIXER: return &mixerOpen;
    case GUI_WINDOW_ABOUT: return &aboutOpen;
    case GUI_WINDOW_SETTINGS: return &settingsOpen;
    case GUI_WINDOW_DEBUG: return &debugOpen;
    case GUI_WINDOW_OSCILLOSCOPE: return &oscOpen;
    case GUI_WINDOW_VOL_METER: return &volMeterOpen;
    case GUI_WINDOW_STATS: return &statsOpen;
    case GUI_WINDOW_COMPAT_FLAGS: return &compatFlagsOpen;
    case GUI_WINDOW_PIANO: return &pianoOpen;
    case GUI_WINDOW_NOTES: return &notesOpen;
    case GUI_WINDOW_TUNER: return &tunerOpen;
    case GUI_WINDOW_SPECTRUM: return &spectrumOpen;
    case GUI_WINDOW_CHANNELS: return &channelsOpen;
    case GUI_WINDOW_PAT_MANAGER: return &patManagerOpen;
    case GUI_WINDOW_SYS_MANAGER: return &sysManagerOpen;
    case GUI_WINDOW_REGISTER_VIEW: return &regViewOpen;
    case GUI_WINDOW_LOG: return &logOpen;
    case GUI_WINDOW_EFFECT_LIST: return &effectListOpen;
    case GUI_WINDOW_CHAN_OSC: return &chanOscOpen;
    case GUI_WINDOW_SUBSONGS: return &subSongsOpen;
    case GUI_WINDOW_FIND: return &findOpen;
    case GUI_WINDOW_CLOCK: return &clockOpen;
    case GUI_WINDOW_GROOVES: return &groovesOpen;
    case GUI_WINDOW_XY_OSC: return &xyOscOpen;
    case GUI_WINDOW_MEMORY: return &memoryOpen;
    case GUI_WINDOW_CS_PLAYER: return &csPlayerOpen;
    case GUI_WINDOW_USER_PRESETS: return &userPresetsOpen;
    case GUI_WINDOW_REF_PLAYER: return &refPlayerOpen;
    case GUI_WINDOW_MULTI_INS_SETUP: return &multiInsSetupOpen;
    case GUI_WINDOW_BACKUPS_MANAGER: return &backupsManagerOpen;
    default: return NULL;
  }
}

bool FurnaceGUI::mcpGetWindowOpen(int w) {
  bool* f=mcpWindowOpenFlag(w);
  return f!=NULL && *f;
}

void FurnaceGUI::mcpSetWindowOpen(int w, bool open) {
  bool* f=mcpWindowOpenFlag(w);
  if (f==NULL) return;
  *f=open;
  if (open) nextWindow=(FurnaceGUIWindows)w;
}

void FurnaceGUI::mcpGetCursor(int& order, int& xCoarse, int& xFine, int& y) {
  order=cursor.order;
  xCoarse=cursor.xCoarse;
  xFine=cursor.xFine;
  y=cursor.y;
}

void FurnaceGUI::mcpSetCursor(int order, int xCoarse, int xFine, int y) {
  cursor.order=order;
  cursor.xCoarse=xCoarse;
  cursor.xFine=xFine;
  cursor.y=y;
  curOrder=order;
}

void FurnaceGUI::mcpGetSelection(int& sOrder, int& sX, int& sF, int& sY, int& eOrder, int& eX, int& eF, int& eY) {
  sOrder=selStart.order; sX=selStart.xCoarse; sF=selStart.xFine; sY=selStart.y;
  eOrder=selEnd.order;   eX=selEnd.xCoarse;   eF=selEnd.xFine;   eY=selEnd.y;
}

void FurnaceGUI::mcpSetSelection(int sOrder, int sX, int sF, int sY, int eOrder, int eX, int eF, int eY) {
  selStart.order=sOrder; selStart.xCoarse=sX; selStart.xFine=sF; selStart.y=sY;
  selEnd.order=eOrder;   selEnd.xCoarse=eX;   selEnd.xFine=eF;   selEnd.y=eY;
}

int FurnaceGUI::mcpGetOctave() { return curOctave; }
void FurnaceGUI::mcpSetOctave(int v) { if (v<0) v=0; if (v>8) v=8; curOctave=v; }
int FurnaceGUI::mcpGetEditStep() { return editStep; }
void FurnaceGUI::mcpSetEditStep(int v) { if (v<0) v=0; editStep=v; }
bool FurnaceGUI::mcpGetFollowOrders() { return followOrders; }
void FurnaceGUI::mcpSetFollowOrders(bool v) { followOrders=v; }
bool FurnaceGUI::mcpGetFollowPattern() { return followPattern; }
void FurnaceGUI::mcpSetFollowPattern(bool v) { followPattern=v; }
bool FurnaceGUI::mcpGetPolyInput() { return noteInputMode!=GUI_NOTE_INPUT_MONO; }
void FurnaceGUI::mcpSetPolyInput(bool v) {
  noteInputMode=v?GUI_NOTE_INPUT_POLY:GUI_NOTE_INPUT_MONO;
  if (e!=NULL) e->setAutoNotePoly(v);
}

void FurnaceGUI::mcpDoActionIndex(int what) { doAction(what); }
void FurnaceGUI::mcpDoUndo() { doUndo(); }
void FurnaceGUI::mcpDoRedo() { doRedo(); }
SDL_Window* FurnaceGUI::mcpGetWindow() { return sdlWin; }
int FurnaceGUI::mcpGetRenderBackendId() { return (int)renderBackend; }
const char* FurnaceGUI::mcpGetRenderBackendName() {
  if (rend!=NULL) return rend->getBackendName();
  return "?";
}

// ===========================================================================
// screenshot
// ===========================================================================

// minimal PNG encoder (8-bit RGBA) using zlib for the IDAT stream.
static void mcpPutU32BE(std::string& s, uint32_t v) {
  s.push_back((char)((v>>24)&0xff));
  s.push_back((char)((v>>16)&0xff));
  s.push_back((char)((v>>8)&0xff));
  s.push_back((char)(v&0xff));
}

static void mcpPNGChunk(std::string& out, const char* type, const std::string& data) {
  mcpPutU32BE(out,(uint32_t)data.size());
  size_t crcStart=out.size();
  out.append(type,4);
  out+=data;
  uLong crc=crc32(0L,Z_NULL,0);
  crc=crc32(crc,(const Bytef*)(out.data()+crcStart),(uInt)(4+data.size()));
  mcpPutU32BE(out,(uint32_t)crc);
}

// bgra: top-down BGRA (or BGRX) rows. forces opaque alpha.
static std::string mcpEncodePNG(const unsigned char* bgra, int w, int h) {
  std::string raw;
  raw.reserve((size_t)h*(1+(size_t)w*4));
  for (int y=0; y<h; y++) {
    raw.push_back(0); // filter type 0 (none)
    const unsigned char* row=bgra+(size_t)y*w*4;
    for (int x=0; x<w; x++) {
      raw.push_back((char)row[x*4+2]); // R
      raw.push_back((char)row[x*4+1]); // G
      raw.push_back((char)row[x*4+0]); // B
      raw.push_back((char)0xff);       // A
    }
  }
  uLongf clen=compressBound((uLong)raw.size());
  std::vector<unsigned char> comp(clen);
  if (compress2(comp.data(),&clen,(const Bytef*)raw.data(),(uLong)raw.size(),6)!=Z_OK) {
    throw std::runtime_error("PNG compression failed");
  }
  std::string out;
  const unsigned char sig[8]={137,80,78,71,13,10,26,10};
  out.append((const char*)sig,8);
  std::string ihdr;
  mcpPutU32BE(ihdr,(uint32_t)w);
  mcpPutU32BE(ihdr,(uint32_t)h);
  ihdr.push_back(8);    // bit depth
  ihdr.push_back(6);    // color type: RGBA
  ihdr.push_back(0);    // compression
  ihdr.push_back(0);    // filter
  ihdr.push_back(0);    // interlace
  mcpPNGChunk(out,"IHDR",ihdr);
  mcpPNGChunk(out,"IDAT",std::string((const char*)comp.data(),clen));
  mcpPNGChunk(out,"IEND",std::string());
  return out;
}

static json mcpScreenshot(FurnaceGUI* gui) {
#ifdef _WIN32
  SDL_Window* win=gui->mcpGetWindow();
  if (win==NULL) throw std::runtime_error("no GUI window");
  SDL_SysWMinfo info;
  SDL_VERSION(&info.version);
  if (!SDL_GetWindowWMInfo(win,&info)) throw std::runtime_error("SDL_GetWindowWMInfo failed");
  HWND hwnd=info.info.win.window;
  RECT rc;
  if (!GetClientRect(hwnd,&rc)) throw std::runtime_error("GetClientRect failed");
  int w=rc.right-rc.left;
  int h=rc.bottom-rc.top;
  if (w<=0 || h<=0) throw std::runtime_error("window has zero size");

  HDC hdcWindow=GetDC(hwnd);
  HDC hdcMem=CreateCompatibleDC(hdcWindow);
  HBITMAP hbm=CreateCompatibleBitmap(hdcWindow,w,h);
  HGDIOBJ oldObj=SelectObject(hdcMem,hbm);

  // PW_RENDERFULLCONTENT captures hardware-accelerated (DX11/GL) client content.
  BOOL ok=PrintWindow(hwnd,hdcMem,PW_CLIENTONLY|PW_RENDERFULLCONTENT);

  std::vector<unsigned char> pixels((size_t)w*h*4);
  BITMAPINFO bmi;
  memset(&bmi,0,sizeof(bmi));
  bmi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth=w;
  bmi.bmiHeader.biHeight=-h; // top-down
  bmi.bmiHeader.biPlanes=1;
  bmi.bmiHeader.biBitCount=32;
  bmi.bmiHeader.biCompression=BI_RGB;
  int gotLines=GetDIBits(hdcMem,hbm,0,h,pixels.data(),&bmi,DIB_RGB_COLORS);

  SelectObject(hdcMem,oldObj);
  DeleteObject(hbm);
  DeleteDC(hdcMem);
  ReleaseDC(hwnd,hdcWindow);

  if (gotLines!=h) throw std::runtime_error("GetDIBits readback failed");

  std::string png=mcpEncodePNG(pixels.data(),w,h);
  json result{
    {"format","png"},
    {"width",w},
    {"height",h},
    {"backend",gui->mcpGetRenderBackendName()},
    {"data_base64",taEncodeBase64(png)}
  };
  if (!ok) result["warning"]="PrintWindow reported failure; image may be incomplete";
  return result;
#else
  throw std::runtime_error(fmt::sprintf(
    "screenshot is only implemented on the Windows build (backend=%s); no readback path on this platform",
    gui->mcpGetRenderBackendName()));
#endif
}

// ===========================================================================
// window-only tools
// ===========================================================================

static FurnaceGUI* mcpReqGUI(FurnaceMCP& m) {
  FurnaceGUI* g=m.getGUI();
  if (g==NULL) throw std::runtime_error("this tool is available only in window mode (--mcp-window)");
  return g;
}

// resolve a window name (or throw). returns the GUI_WINDOW_* enum.
static int mcpResolveWindow(const String& name) {
  for (const MCPWindowName* w=mcpWindowNames; w->name!=NULL; w++) {
    if (name==w->name) return w->win;
  }
  throw std::runtime_error(fmt::sprintf("unknown window: %s",name));
}

void registerWindowTools(FurnaceMCP& m) {
  // --- screenshot ---
  m.addTool(FurnaceMCPTool(
    "screenshot",
    "Capture the live GUI window and return it as a base64-encoded image {format,width,height,data_base64}. Window mode only.",
    json{{"type","object"},{"properties",json::object()}},
    [](FurnaceMCP& m, const json& args) -> json {
      return mcpScreenshot(mcpReqGUI(m));
    }
  ));

  // --- window management ---
  m.addTool(FurnaceMCPTool(
    "list_windows",
    "List the GUI's toggleable windows with their name (GUI_WINDOW_* suffix, lowercased) and open state.",
    json{{"type","object"},{"properties",json::object()}},
    [](FurnaceMCP& m, const json& args) -> json {
      FurnaceGUI* g=mcpReqGUI(m);
      json list=json::array();
      for (const MCPWindowName* w=mcpWindowNames; w->name!=NULL; w++) {
        list.push_back(json{{"name",w->name},{"open",g->mcpGetWindowOpen(w->win)}});
      }
      return json{{"windows",list}};
    }
  ));

  m.addTool(FurnaceMCPTool(
    "open_window",
    "Open (and focus) a GUI window by name (see list_windows).",
    json{{"type","object"},{"properties",{
      {"name",{{"type","string"}}}
    }},{"required",json::array({"name"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      FurnaceGUI* g=mcpReqGUI(m);
      int w=mcpResolveWindow(mcpArgStr(args,"name"));
      g->mcpSetWindowOpen(w,true);
      return json{{"name",mcpArgStr(args,"name")},{"open",g->mcpGetWindowOpen(w)}};
    }
  ));

  m.addTool(FurnaceMCPTool(
    "close_window",
    "Close a GUI window by name (see list_windows).",
    json{{"type","object"},{"properties",{
      {"name",{{"type","string"}}}
    }},{"required",json::array({"name"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      FurnaceGUI* g=mcpReqGUI(m);
      int w=mcpResolveWindow(mcpArgStr(args,"name"));
      g->mcpSetWindowOpen(w,false);
      return json{{"name",mcpArgStr(args,"name")},{"open",g->mcpGetWindowOpen(w)}};
    }
  ));

  // --- gui actions ---
  m.addTool(FurnaceMCPTool(
    "list_gui_actions",
    "List every dispatchable GUI action with its index, name and friendlyName (menu/keybind actions). Sentinels are skipped.",
    json{{"type","object"},{"properties",json::object()}},
    [](FurnaceMCP& m, const json& args) -> json {
      mcpReqGUI(m);
      json list=json::array();
      for (int i=0; i<GUI_ACTION_MAX; i++) {
        const FurnaceGUIActionDef& a=guiActions[i];
        if (a.isNotABind()) continue; // GLOBAL_MIN / *_MAX sentinels
        list.push_back(json{{"index",i},{"name",a.name},{"friendlyName",a.friendlyName}});
      }
      return json{{"actions",list}};
    }
  ));

  m.addTool(FurnaceMCPTool(
    "gui_action",
    "Dispatch a GUI action on the GUI thread, by 'name' (from list_gui_actions) or numeric 'id'. Errors on unknown or sentinel actions.",
    json{{"type","object"},{"properties",{
      {"name",{{"type","string"},{"description","action name, e.g. PAT_NOTE_UP"}}},
      {"id",{{"type","integer"},{"description","action index (alternative to name)"}}}
    }}},
    [](FurnaceMCP& m, const json& args) -> json {
      FurnaceGUI* g=mcpReqGUI(m);
      int idx=-1;
      if (args.contains("name")) {
        String name=mcpArgStr(args,"name");
        for (int i=0; i<GUI_ACTION_MAX; i++) {
          if (name==guiActions[i].name) { idx=i; break; }
        }
        if (idx<0) throw std::runtime_error(fmt::sprintf("unknown action: %s",name));
      } else if (args.contains("id")) {
        idx=mcpArgInt(args,"id");
        if (idx<0 || idx>=GUI_ACTION_MAX) throw std::runtime_error("action id out of range");
      } else {
        throw std::runtime_error("gui_action requires 'name' or 'id'");
      }
      if (guiActions[idx].isNotABind()) throw std::runtime_error("that action index is a sentinel, not a real action");
      g->mcpDoActionIndex(idx);
      return json{{"ok",true},{"index",idx},{"name",guiActions[idx].name}};
    }
  ));

  // --- cursor / selection ---
  m.addTool(FurnaceMCPTool(
    "get_cursor",
    "Get the pattern editor cursor: order, channel (xCoarse), column (xFine), row (y).",
    json{{"type","object"},{"properties",json::object()}},
    [](FurnaceMCP& m, const json& args) -> json {
      FurnaceGUI* g=mcpReqGUI(m);
      int order,xc,xf,y;
      g->mcpGetCursor(order,xc,xf,y);
      return json{{"order",order},{"channel",xc},{"column",xf},{"row",y}};
    }
  ));

  m.addTool(FurnaceMCPTool(
    "set_cursor",
    "Set the pattern editor cursor. channel and row required; order and column optional (column is the sub-column: 0=note).",
    json{{"type","object"},{"properties",{
      {"order",{{"type","integer"}}},
      {"channel",{{"type","integer"},{"description","xCoarse"}}},
      {"row",{{"type","integer"},{"description","y"}}},
      {"column",{{"type","integer"},{"description","xFine (0=note)"}}}
    }},{"required",json::array({"channel","row"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      FurnaceGUI* g=mcpReqGUI(m);
      int order,xc,xf,y;
      g->mcpGetCursor(order,xc,xf,y);
      order=mcpOptInt(args,"order",order);
      xc=mcpArgInt(args,"channel");
      y=mcpArgInt(args,"row");
      xf=mcpOptInt(args,"column",0);
      g->mcpSetCursor(order,xc,xf,y);
      g->mcpGetCursor(order,xc,xf,y);
      return json{{"order",order},{"channel",xc},{"column",xf},{"row",y}};
    }
  ));

  m.addTool(FurnaceMCPTool(
    "get_selection",
    "Get the pattern editor selection (from/to), each with order, channel (xCoarse), column (xFine), row (y).",
    json{{"type","object"},{"properties",json::object()}},
    [](FurnaceMCP& m, const json& args) -> json {
      FurnaceGUI* g=mcpReqGUI(m);
      int so,sx,sf,sy,eo,ex,ef,ey;
      g->mcpGetSelection(so,sx,sf,sy,eo,ex,ef,ey);
      return json{
        {"from",{{"order",so},{"channel",sx},{"column",sf},{"row",sy}}},
        {"to",{{"order",eo},{"channel",ex},{"column",ef},{"row",ey}}}
      };
    }
  ));

  m.addTool(FurnaceMCPTool(
    "set_selection",
    "Set the pattern editor selection. 'from' and 'to' each take {channel,row} (required) and optional {order,column}.",
    json{{"type","object"},{"properties",{
      {"from",{{"type","object"}}},
      {"to",{{"type","object"}}}
    }},{"required",json::array({"from","to"})}},
    [](FurnaceMCP& m, const json& args) -> json {
      FurnaceGUI* g=mcpReqGUI(m);
      if (!args.contains("from") || !args["from"].is_object()) throw std::runtime_error("missing object argument: from");
      if (!args.contains("to") || !args["to"].is_object()) throw std::runtime_error("missing object argument: to");
      const json& f=args["from"];
      const json& t=args["to"];
      int so=mcpOptInt(f,"order",0), sx=mcpArgInt(f,"channel"), sf=mcpOptInt(f,"column",0), sy=mcpArgInt(f,"row");
      int eo=mcpOptInt(t,"order",0), ex=mcpArgInt(t,"channel"), ef=mcpOptInt(t,"column",0), ey=mcpArgInt(t,"row");
      g->mcpSetSelection(so,sx,sf,sy,eo,ex,ef,ey);
      g->mcpGetSelection(so,sx,sf,sy,eo,ex,ef,ey);
      return json{
        {"from",{{"order",so},{"channel",sx},{"column",sf},{"row",sy}}},
        {"to",{{"order",eo},{"channel",ex},{"column",ef},{"row",ey}}}
      };
    }
  ));

  // --- edit controls ---
  m.addTool(FurnaceMCPTool(
    "get_edit_controls",
    "Get edit-control state: octave, editStep, followOrders, followPattern, polyInput.",
    json{{"type","object"},{"properties",json::object()}},
    [](FurnaceMCP& m, const json& args) -> json {
      FurnaceGUI* g=mcpReqGUI(m);
      return json{
        {"octave",g->mcpGetOctave()},
        {"editStep",g->mcpGetEditStep()},
        {"followOrders",g->mcpGetFollowOrders()},
        {"followPattern",g->mcpGetFollowPattern()},
        {"polyInput",g->mcpGetPolyInput()}
      };
    }
  ));

  m.addTool(FurnaceMCPTool(
    "set_edit_controls",
    "Set edit-control state. All fields optional: octave, editStep, followOrders, followPattern, polyInput.",
    json{{"type","object"},{"properties",{
      {"octave",{{"type","integer"}}},
      {"editStep",{{"type","integer"}}},
      {"followOrders",{{"type","boolean"}}},
      {"followPattern",{{"type","boolean"}}},
      {"polyInput",{{"type","boolean"}}}
    }}},
    [](FurnaceMCP& m, const json& args) -> json {
      FurnaceGUI* g=mcpReqGUI(m);
      if (args.contains("octave")) g->mcpSetOctave(mcpArgInt(args,"octave"));
      if (args.contains("editStep")) g->mcpSetEditStep(mcpArgInt(args,"editStep"));
      if (args.contains("followOrders")) g->mcpSetFollowOrders(mcpOptBool(args,"followOrders",g->mcpGetFollowOrders()));
      if (args.contains("followPattern")) g->mcpSetFollowPattern(mcpOptBool(args,"followPattern",g->mcpGetFollowPattern()));
      if (args.contains("polyInput")) g->mcpSetPolyInput(mcpOptBool(args,"polyInput",g->mcpGetPolyInput()));
      return json{
        {"octave",g->mcpGetOctave()},
        {"editStep",g->mcpGetEditStep()},
        {"followOrders",g->mcpGetFollowOrders()},
        {"followPattern",g->mcpGetFollowPattern()},
        {"polyInput",g->mcpGetPolyInput()}
      };
    }
  ));

  // --- undo / redo ---
  m.addTool(FurnaceMCPTool(
    "undo",
    "Undo the last edit (GUI undo stack).",
    json{{"type","object"},{"properties",json::object()}},
    [](FurnaceMCP& m, const json& args) -> json {
      mcpReqGUI(m)->mcpDoUndo();
      return json{{"ok",true}};
    }
  ));

  m.addTool(FurnaceMCPTool(
    "redo",
    "Redo the last undone edit (GUI undo stack).",
    json{{"type","object"},{"properties",json::object()}},
    [](FurnaceMCP& m, const json& args) -> json {
      mcpReqGUI(m)->mcpDoRedo();
      return json{{"ok",true}};
    }
  ));
}

// ===========================================================================
// marshalling + serve loop
// ===========================================================================

namespace {
  struct MCPPendingCall {
    json req;
    // when set, this is a closure job (furnaceMCPGUIMarshal) instead of a
    // JSON-RPC request; shared_ptr so a timed-out caller can't dangle it.
    std::shared_ptr<std::function<void()>> fn;
    std::promise<json> prom;
  };

  struct MCPWinServer {
    FurnaceMCP* mcp;
    std::thread netThread;
    std::mutex qMutex;
    std::deque<MCPPendingCall*> queue;
    std::atomic<bool> hasWork;
    std::atomic<bool> running;
    MCPSocket listener;
    std::atomic<MCPSocket> client;

    MCPWinServer():
      mcp(NULL), hasWork(false), running(false),
      listener(MCP_INVALID_SOCKET), client(MCP_INVALID_SOCKET) {}
  };

  MCPWinServer* g_win=NULL;

  // the GUI (pump) thread id, recorded every pump so the marshal hook can
  // detect "already on the GUI thread" and run inline instead of deadlocking.
  std::atomic<std::thread::id> g_pumpThread{};

  // build a JSON-RPC error response object.
  json mcpRPCError(const json& id, int code, const String& message) {
    return json{
      {"jsonrpc","2.0"},
      {"id",id},
      {"error",{{"code",code},{"message",message}}}
    };
  }

  // wake the GUI thread so it services the queue promptly (SDL_WaitEventTimeout).
  void mcpWakeGUI() {
    SDL_Event ev;
    memset(&ev,0,sizeof(ev));
    ev.type=SDL_USEREVENT;
    SDL_PushEvent(&ev);
  }

  // engine-only tools that can run far longer than the GUI marshalling budget
  // (live capture, offline rendering). they touch only the self-locking
  // DivEngine - never GUI state - so they run directly on the net thread,
  // exactly as they would in headless mode. the client protocol is serial
  // (one line at a time), so nothing else runs concurrently while they do.
  // phases that swap dispatch cores (render_wav's saveAudio/finishAudioFile)
  // are the exception: they marshal themselves back to the GUI thread via
  // furnaceMCPGUIMarshal, because the GUI reads dispatch pointers mid-frame.
  static bool mcpIsLongEngineTool(const json& req) {
    if (!req.is_object() || !req.contains("method") || req["method"]!="tools/call") return false;
    if (!req.contains("params") || !req["params"].is_object()) return false;
    const json& p=req["params"];
    if (!p.contains("name") || !p["name"].is_string()) return false;
    String n=p["name"].get<String>();
    return n=="capture_audio" || n=="render_wav" || n=="export_rom";
  }

  // furnaceMCPGUIMarshal implementation: run a closure on the GUI thread via
  // the same pending-call queue the JSON-RPC marshalling uses. used by long
  // tools (render_wav) for their dispatch-swapping phases; see mcp.h.
  bool mcpGUIMarshalImpl(const std::function<void()>& fn) {
    MCPWinServer* w=g_win;
    if (w==NULL) return false;
    if (std::this_thread::get_id()==g_pumpThread.load()) return false; // already on GUI thread
    MCPPendingCall* call=new MCPPendingCall();
    call->fn=std::make_shared<std::function<void()>>(fn);
    std::future<json> fut=call->prom.get_future();
    {
      std::lock_guard<std::mutex> lk(w->qMutex);
      w->queue.push_back(call);
      w->hasWork.store(true,std::memory_order_relaxed);
    }
    mcpWakeGUI();
    if (fut.wait_for(std::chrono::seconds(60))!=std::future_status::ready) {
      throw std::runtime_error("GUI thread did not service the engine-exclusive phase within 60s");
    }
    json r=fut.get();
    if (r.is_object() && r.contains("__err")) {
      throw std::runtime_error(r["__err"].get<String>());
    }
    return true;
  }

  // net-thread side: parse a line, marshal handleRequest to the GUI thread,
  // wait for the result (with a timeout so a wedged GUI can't hang the socket),
  // and serialize the response. mirrors FurnaceMCP::handleLine, but off-thread.
  String mcpHandleLineMarshalled(MCPWinServer* w, const String& line) {
    if (line.empty()) return "";
    json req;
    try {
      req=json::parse(line);
    } catch (std::exception&) {
      return mcpRPCError(nullptr,-32700,"parse error").dump();
    }

    if (mcpIsLongEngineTool(req)) {
      json resp=w->mcp->handleRequest(req);
      if (resp.is_null()) return "";
      return resp.dump();
    }

    MCPPendingCall* call=new MCPPendingCall();
    call->req=req;
    std::future<json> fut=call->prom.get_future();
    {
      std::lock_guard<std::mutex> lk(w->qMutex);
      w->queue.push_back(call);
      w->hasWork.store(true,std::memory_order_relaxed);
    }
    mcpWakeGUI();

    // the GUI thread (pump) owns 'call' from here on; do not touch it again.
    if (fut.wait_for(std::chrono::seconds(30))!=std::future_status::ready) {
      json id=req.contains("id")?req["id"]:json(nullptr);
      return mcpRPCError(id,-32000,"GUI thread did not service the request within 30s").dump();
    }
    json resp=fut.get();
    if (resp.is_null()) return "";
    return resp.dump();
  }

  // one accepted client, newline-delimited JSON-RPC (mirrors net.cpp).
  void mcpServeClient(MCPWinServer* w, MCPSocket clientSock) {
    std::string pending;
    char buf[4096];
    while (w->running.load()) {
      int got=recv(clientSock,buf,sizeof(buf),0);
      if (got<=0) break;
      pending.append(buf,got);
      size_t nl;
      while ((nl=pending.find('\n'))!=std::string::npos) {
        std::string line=pending.substr(0,nl);
        pending.erase(0,nl+1);
        if (!line.empty() && line.back()=='\r') line.pop_back();
        String resp=mcpHandleLineMarshalled(w,line.c_str());
        if (!resp.empty()) {
          resp+='\n';
          size_t sent=0;
          while (sent<resp.size()) {
            int n=send(clientSock,resp.c_str()+sent,(int)(resp.size()-sent),0);
            if (n<=0) break;
            sent+=n;
          }
        }
      }
    }
  }

  void mcpNetThread(MCPWinServer* w) {
    while (w->running.load()) {
      MCPSocket clientSock=accept(w->listener,NULL,NULL);
      if (clientSock==MCP_INVALID_SOCKET) break;
      if (!w->running.load()) { mcpCloseSocket(clientSock); break; }
      logI("MCP window: client connected.");
      w->client.store(clientSock);
      mcpServeClient(w,clientSock);
      w->client.store(MCP_INVALID_SOCKET);
      mcpCloseSocket(clientSock);
      logI("MCP window: client disconnected.");
    }
  }
}

// GUI-thread pump: drain the queue and fulfill the promises. cheap when idle.
void furnaceMCPWindowPump() {
  MCPWinServer* w=g_win;
  if (w==NULL) return;
  g_pumpThread.store(std::this_thread::get_id());
  if (!w->hasWork.load(std::memory_order_relaxed)) return;

  std::deque<MCPPendingCall*> local;
  {
    std::lock_guard<std::mutex> lk(w->qMutex);
    local.swap(w->queue);
    w->hasWork.store(false,std::memory_order_relaxed);
  }
  for (MCPPendingCall* call: local) {
    json resp;
    if (call->fn) {
      try {
        (*call->fn)();
      } catch (std::exception& ex) {
        resp=json{{"__err",ex.what()}};
      }
      call->prom.set_value(resp);
      delete call;
      continue;
    }
    try {
      resp=w->mcp->handleRequest(call->req);
    } catch (std::exception& ex) {
      json id=call->req.contains("id")?call->req["id"]:json(nullptr);
      resp=mcpRPCError(id,-32603,fmt::sprintf("internal error: %s",ex.what()));
    }
    call->prom.set_value(resp);
    delete call;
  }
}

bool FurnaceMCP::serveWindow(const String& addr) {
  furnaceMCPFixupStdio();
  if (e==NULL) {
    logE("MCP: no engine bound!");
    return false;
  }
  if (g_win!=NULL) {
    logE("MCP: window server already running!");
    return false;
  }

  size_t colon=addr.find_last_of(':');
  if (colon==String::npos) {
    logE("MCP: address must be host:port (got %s)",addr);
    return false;
  }
  String host=addr.substr(0,colon);
  int port=atoi(addr.substr(colon+1).c_str());
  if (port<0 || port>65535) {
    logE("MCP: invalid port in %s",addr);
    return false;
  }

#ifdef _WIN32
  WSADATA wsaData;
  if (WSAStartup(MAKEWORD(2,2),&wsaData)!=0) {
    logE("MCP: WSAStartup failed!");
    return false;
  }
#endif

  MCPSocket listener=socket(AF_INET,SOCK_STREAM,0);
  if (listener==MCP_INVALID_SOCKET) {
    logE("MCP: could not create socket!");
#ifdef _WIN32
    WSACleanup();
#endif
    return false;
  }

  int reuse=1;
  setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,(const char*)&reuse,sizeof(reuse));

  sockaddr_in bindAddr;
  memset(&bindAddr,0,sizeof(bindAddr));
  bindAddr.sin_family=AF_INET;
  bindAddr.sin_port=htons((unsigned short)port);
  if (host.empty() || host=="0.0.0.0") {
    bindAddr.sin_addr.s_addr=INADDR_ANY;
  } else {
    if (inet_pton(AF_INET,host.c_str(),&bindAddr.sin_addr)!=1) {
      logE("MCP: invalid host %s (IPv4 addresses only)",host);
      mcpCloseSocket(listener);
#ifdef _WIN32
      WSACleanup();
#endif
      return false;
    }
  }

  if (bind(listener,(sockaddr*)&bindAddr,sizeof(bindAddr))!=0) {
    logE("MCP: could not bind %s!",addr);
    mcpCloseSocket(listener);
#ifdef _WIN32
    WSACleanup();
#endif
    return false;
  }
  if (listen(listener,1)!=0) {
    logE("MCP: could not listen on %s!",addr);
    mcpCloseSocket(listener);
#ifdef _WIN32
    WSACleanup();
#endif
    return false;
  }

  MCPWinServer* w=new MCPWinServer();
  w->mcp=this;
  w->listener=listener;
  w->running.store(true);
  g_win=w;

  // report the actual bound address (resolves port 0 to the real port)
  sockaddr_in actual;
#ifdef _WIN32
  int actualLen=sizeof(actual);
#else
  socklen_t actualLen=sizeof(actual);
#endif
  if (getsockname(listener,(sockaddr*)&actual,&actualLen)==0) {
    char ipStr[64];
    inet_ntop(AF_INET,&actual.sin_addr,ipStr,sizeof(ipStr));
    printf("furnace-mcp ready %s:%d\n",ipStr,(int)ntohs(actual.sin_port));
    fflush(stdout);
  } else {
    printf("furnace-mcp ready %s\n",addr.c_str());
    fflush(stdout);
  }

  furnaceMCPGUIMarshal=mcpGUIMarshalImpl;
  w->netThread=std::thread(mcpNetThread,w);
  return true;
}

void FurnaceMCP::stopWindow() {
  MCPWinServer* w=g_win;
  if (w==NULL) return;
  furnaceMCPGUIMarshal=NULL;
  w->running.store(false);

  // unblock accept() and any in-flight recv().
  MCPSocket cl=w->client.load();
  if (cl!=MCP_INVALID_SOCKET) mcpCloseSocket(cl);
  if (w->listener!=MCP_INVALID_SOCKET) mcpCloseSocket(w->listener);

  // fail any queued-but-unserviced calls so the net thread's futures return.
  {
    std::lock_guard<std::mutex> lk(w->qMutex);
    for (MCPPendingCall* call: w->queue) {
      if (call->fn) {
        call->prom.set_value(json{{"__err","server shutting down"}});
      } else {
        json id=call->req.contains("id")?call->req["id"]:json(nullptr);
        call->prom.set_value(mcpRPCError(id,-32001,"server shutting down"));
      }
      delete call;
    }
    w->queue.clear();
    w->hasWork.store(false,std::memory_order_relaxed);
  }

  if (w->netThread.joinable()) w->netThread.join();

#ifdef _WIN32
  WSACleanup();
#endif
  g_win=NULL;
  delete w;
  logI("MCP window server stopped.");
}
