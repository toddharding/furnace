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

// MCP (Model Context Protocol) server: exposes the DivEngine over JSON-RPC 2.0
// (initialize / tools/list / tools/call) so AI-agent harnesses can author,
// drive, inspect and render songs. Binds a DivEngine* like FurnaceCLI does.
// Transports: stdio (--mcp) and TCP (--mcp-tcp <host:port>).

#ifndef _FUR_MCP_H
#define _FUR_MCP_H

#include "../engine/engine.h"

#include <functional>
#include <vector>
#include "nlohmann/json.hpp"

class FurnaceMCP;

// rebind CRT stdio to the process's real std handles (Windows GUI-subsystem
// fix; no-op elsewhere). called by the serve loops and the self-test.
void furnaceMCPFixupStdio();

// one MCP tool: name/description/JSON-Schema for tools/list, and a handler.
// handlers return the tool's result payload (serialized as text content) and
// signal tool-level failure by throwing std::runtime_error (reported as a
// JSON-RPC error to the caller, engine state untouched by convention).
struct FurnaceMCPTool {
  const char* name;
  const char* description;
  nlohmann::json inputSchema;
  std::function<nlohmann::json(FurnaceMCP&, const nlohmann::json&)> handler;
  FurnaceMCPTool(const char* n, const char* d, nlohmann::json s, std::function<nlohmann::json(FurnaceMCP&, const nlohmann::json&)> h):
    name(n), description(d), inputSchema(s), handler(h) {}
};

class FurnaceMCP;

// per-domain tool registration (one translation unit each under src/mcp/).
// each appends its domain's tools via FurnaceMCP::addTool.
void registerPatternTools(FurnaceMCP& m);    // patterns, orders, grooves, speeds
void registerInstrumentTools(FurnaceMCP& m); // instrument CRUD + JSON get/set/update
void registerAssetTools(FurnaceMCP& m);      // wavetables + samples (incl. DSP ops)
void registerSongTools(FurnaceMCP& m);       // metadata, subsongs, systems, channels, mixer, compat flags, config
void registerRenderTools(FurnaceMCP& m);     // render_wav, export_vgm/rom/cmdstream/text
void registerObserveTools(FurnaceMCP& m);    // channel states, registers, oscilloscopes, capture

class FurnaceMCP {
  DivEngine* e;
  std::vector<FurnaceMCPTool> tools;
  void registerCoreTools();

  public:
    DivEngine* engine() { return e; }
    const std::vector<FurnaceMCPTool>& getTools() { return tools; }
    void addTool(FurnaceMCPTool t);

    void bindEngine(DivEngine* eng);

    // handle one JSON-RPC 2.0 request object. returns the response object,
    // or a null json for notifications (no reply shall be sent).
    nlohmann::json handleRequest(const nlohmann::json& req);

    // parse one line as JSON-RPC, handle it, serialize the response.
    // returns the empty string when no reply shall be sent.
    String handleLine(const String& line);

    // serve loops (blocking). serveTcp prints "furnace-mcp ready <host:port>"
    // once listening (with the real port when given port 0).
    bool serveStdio();
    bool serveTcp(const String& addr);

    // in-process self-test against the bound engine. returns 0 on success.
    int selfTest();

    FurnaceMCP();
};

#endif
