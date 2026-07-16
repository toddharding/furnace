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

// shared helpers for MCP tool implementations. tool handlers signal failure
// by throwing std::runtime_error; these helpers throw with clear messages on
// missing/mistyped arguments.

#ifndef _FUR_MCP_TOOLS_COMMON_H
#define _FUR_MCP_TOOLS_COMMON_H

#include "mcp.h"
#include <stdexcept>

static inline int mcpArgInt(const nlohmann::json& args, const char* name) {
  if (!args.contains(name) || !args[name].is_number_integer()) {
    throw std::runtime_error(fmt::sprintf("missing or non-integer argument: %s",name));
  }
  return args[name].get<int>();
}

static inline String mcpArgStr(const nlohmann::json& args, const char* name) {
  if (!args.contains(name) || !args[name].is_string()) {
    throw std::runtime_error(fmt::sprintf("missing or non-string argument: %s",name));
  }
  return args[name].get<String>();
}

static inline int mcpOptInt(const nlohmann::json& args, const char* name, int def) {
  if (!args.contains(name)) return def;
  if (!args[name].is_number_integer()) {
    throw std::runtime_error(fmt::sprintf("non-integer argument: %s",name));
  }
  return args[name].get<int>();
}

static inline bool mcpOptBool(const nlohmann::json& args, const char* name, bool def) {
  if (!args.contains(name)) return def;
  if (!args[name].is_boolean()) {
    throw std::runtime_error(fmt::sprintf("non-boolean argument: %s",name));
  }
  return args[name].get<bool>();
}

static inline double mcpOptFloat(const nlohmann::json& args, const char* name, double def) {
  if (!args.contains(name)) return def;
  if (!args[name].is_number()) {
    throw std::runtime_error(fmt::sprintf("non-numeric argument: %s",name));
  }
  return args[name].get<double>();
}

static inline String mcpOptStr(const nlohmann::json& args, const char* name, const String& def) {
  if (!args.contains(name)) return def;
  if (!args[name].is_string()) {
    throw std::runtime_error(fmt::sprintf("non-string argument: %s",name));
  }
  return args[name].get<String>();
}

// drain a SafeWriter into a String and dispose of it.
static inline String mcpWriterToString(SafeWriter* w) {
  if (w==NULL) throw std::runtime_error("engine returned no data");
  String out((const char*)w->getFinalBuf(),w->size());
  w->finish();
  delete w;
  return out;
}

#endif
