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

// MCP transports: newline-delimited JSON-RPC 2.0 over stdio and TCP.
// nothing else in the Furnace tree does sockets, so this file owns the
// whole socket lifecycle (including WSAStartup/WSACleanup on Windows).

#include "mcp.h"
#include "../ta-log.h"

#include <stdio.h>
#include <string.h>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>
#include <fcntl.h>
typedef SOCKET MCPSocket;
#define MCP_INVALID_SOCKET INVALID_SOCKET
#define mcpCloseSocket closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int MCPSocket;
#define MCP_INVALID_SOCKET (-1)
#define mcpCloseSocket close
#endif

// on Windows, Furnace builds as a GUI-subsystem app (WinMain), so the CRT may
// initialize with dead stdio even when the caller redirected the handles.
// rebind the CRT's stdin/stdout/stderr to the process's real std handles so
// the MCP protocol (and its logs) actually flow. no-op elsewhere.
void furnaceMCPFixupStdio() {
#ifdef _WIN32
  // in a GUI-subsystem CRT the standard streams may have no file descriptor
  // at all (_fileno()==-2), so first give each stream a real fd via NUL,
  // then dup the process's actual std handle over it (if one exists).
  HANDLE h;
  int fd;
  if (_fileno(stdin)<0) {
    h=GetStdHandle(STD_INPUT_HANDLE);
    if (h!=NULL && h!=INVALID_HANDLE_VALUE) {
      freopen("NUL","rb",stdin);
      fd=_open_osfhandle((intptr_t)h,_O_RDONLY|_O_BINARY);
      if (fd>=0 && _fileno(stdin)>=0) {
        _dup2(fd,_fileno(stdin));
        _close(fd);
      }
    }
  }
  if (_fileno(stdout)<0) {
    h=GetStdHandle(STD_OUTPUT_HANDLE);
    if (h!=NULL && h!=INVALID_HANDLE_VALUE) {
      freopen("NUL","wb",stdout);
      fd=_open_osfhandle((intptr_t)h,_O_WRONLY|_O_BINARY);
      if (fd>=0 && _fileno(stdout)>=0) {
        _dup2(fd,_fileno(stdout));
        _close(fd);
      }
    }
  }
  if (_fileno(stderr)<0) {
    h=GetStdHandle(STD_ERROR_HANDLE);
    if (h!=NULL && h!=INVALID_HANDLE_VALUE) {
      freopen("NUL","wb",stderr);
      fd=_open_osfhandle((intptr_t)h,_O_WRONLY|_O_BINARY);
      if (fd>=0 && _fileno(stderr)>=0) {
        _dup2(fd,_fileno(stderr));
        _close(fd);
      }
    }
  }
  setvbuf(stdout,NULL,_IONBF,0);
  setvbuf(stderr,NULL,_IONBF,0);
#endif
}

bool FurnaceMCP::serveStdio() {
  furnaceMCPFixupStdio();
  if (e==NULL) {
    logE("MCP: no engine bound!");
    return false;
  }
  // stdout carries the protocol - keep furnace's logging off it.
  logLevel=LOGLEVEL_ERROR;
  std::string line;
  int c;
  while ((c=fgetc(stdin))!=EOF) {
    if (c=='\r') continue;
    if (c=='\n') {
      String resp=handleLine(line.c_str());
      if (!resp.empty()) {
        fwrite(resp.c_str(),1,resp.size(),stdout);
        fputc('\n',stdout);
        fflush(stdout);
      }
      line.clear();
    } else {
      line+=(char)c;
    }
  }
  return true;
}

bool FurnaceMCP::serveTcp(const String& addr) {
  furnaceMCPFixupStdio();
  if (e==NULL) {
    logE("MCP: no engine bound!");
    return false;
  }

  // parse host:port
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

  // accept loop: one client at a time, newline-delimited JSON-RPC.
  bool running=true;
  while (running) {
    MCPSocket client=accept(listener,NULL,NULL);
    if (client==MCP_INVALID_SOCKET) break;
    logI("MCP: client connected.");
    std::string pending;
    char buf[4096];
    while (true) {
      int got=recv(client,buf,sizeof(buf),0);
      if (got<=0) break;
      pending.append(buf,got);
      size_t nl;
      while ((nl=pending.find('\n'))!=std::string::npos) {
        std::string line=pending.substr(0,nl);
        pending.erase(0,nl+1);
        if (!line.empty() && line.back()=='\r') line.pop_back();
        String resp=handleLine(line.c_str());
        if (!resp.empty()) {
          resp+='\n';
          size_t sent=0;
          while (sent<resp.size()) {
            int n=send(client,resp.c_str()+sent,(int)(resp.size()-sent),0);
            if (n<=0) break;
            sent+=n;
          }
        }
      }
    }
    logI("MCP: client disconnected.");
    mcpCloseSocket(client);
  }

  mcpCloseSocket(listener);
#ifdef _WIN32
  WSACleanup();
#endif
  return true;
}
