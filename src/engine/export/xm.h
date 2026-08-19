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

/* .xm - FastTracker II Extended Module.
 *
 * Furnace already reads XM (see fileOps/xm.cpp). This writes it, which is what
 * lets a song made here be played by something that is not Furnace. The N64 is
 * the reason it exists: libdragon ships an XM player (xm64) and a converter
 * (audioconv64), so an XM is a song a Nintendo 64 game can play without anybody
 * writing a player or adopting a private format.
 *
 * WHAT IT REFUSES. XM is a fixed model and Furnace is a wider one, so some
 * songs do not fit. Every case is named - the channel, the pattern, the row,
 * the instrument - and stops the export. Silently dropping a macro or an
 * effect would produce a file that plays differently from what the composer
 * heard, which is the failure this exporter exists to prevent.
 *
 * THE MAPPING IS THE IMPORTER'S, RUN BACKWARDS. Where fileOps/xm.cpp makes a
 * decision (note numbering, sample tuning, envelope shape, tempo), this file
 * inverts that same decision rather than inventing a second one.
 */

#ifndef _EXPORT_XM_H
#define _EXPORT_XM_H

#include "../export.h"

class DivExportXM: public DivROMExport {
  public:
    bool go(DivEngine* eng);
    bool isRunning();
    bool hasFailed();
    void abort();
    void wait();
    DivROMExportProgress getProgress(int index=0);
    ~DivExportXM() {}

  private:
    bool failed=false;
};

#endif
