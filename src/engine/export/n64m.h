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

/* .n64m - A SONG A NINTENDO 64 CAN PLAY.
 *
 * WHY NOT A REGISTER DUMP, which is what most exports here are. There are no
 * registers: the N64's audio is software, so a dump would be a recording of
 * this program's own mixer and the console would have to reproduce it sample
 * for sample. And why not a rendered stream, which is what several real N64
 * titles shipped: a two-minute stereo stream at 32 kHz is 15 MB, and the whole
 * cartridge is 64.
 *
 * So this is the song: the orders, the patterns, the instruments and the PCM,
 * in the smallest arrangement a player can walk. The player is the same code on
 * the console and on a desktop - one mixer, above the platform seam - which is
 * the only way the two can be checked against each other at all.
 *
 * THE LAYOUT, all big-endian because that is the machine that reads it:
 *
 *   header    magic "UFN6", version, rate, voices, tick rate, speed, rows,
 *             orders, patterns, instruments, samples, loop order, name
 *   orders    orderLen * voices, u16 each: which pattern each voice plays
 *   patterns  patCount * rows * 5 bytes: note, instrument, volume, fx, fxval
 *   instrums  8 bytes each: sample, base note, volume, and the 16.16 step that
 *             plays that sample at that note. THE STEP IS EXPORTED rather than
 *             derived, so the console is in tune with what the composer heard
 *             instead of with a second implementation of Furnace's pitch table.
 *   samples   12 bytes each: frames, loop start, loop end
 *   pcm       s16 big-endian, one block per sample in order
 *
 * WHAT IT REFUSES: any effect the player does not implement. A module that
 * silently dropped an effect would play differently on the console than in the
 * tracker, which is the one thing an export like this exists to prevent - so
 * the export names the pattern, the row and the effect and stops.
 */

#include "../export.h"

class DivExportN64M: public DivROMExport {
  public:
    bool go(DivEngine* eng);
    bool isRunning();
    bool hasFailed();
    void abort();
    void wait();
    DivROMExportProgress getProgress(int index=0);
    ~DivExportN64M() {}

  private:
    bool failed=false;
};
