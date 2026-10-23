// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

package org.capyps5.emulator

import java.io.File

/**
 * The real-time log option: builds the per-game log file under the logs folder of the main
 * library folder and points the core log at it. The core appends every line to the file as it is
 * written (flushed per line, so a kill by the low-memory killer or a crash still leaves the story
 * behind); one game owns one file and sessions are separated by a header line. The file name is
 * the game title reduced to letters and digits, e.g. "Kena Bridge of Spirits" -> kenabridgeofspirits.log
 */
object RealtimeLog {

    /** Creates the logs folder if needed and points the core at this game's log file. */
    fun start(libraryPath: String, titleName: String) {
        val slug = titleName.filter { it.isLetterOrDigit() }.lowercase().take(48).ifEmpty { "game" }
        val dir = File(libraryPath, "logs")
        if (!runCatching { dir.mkdirs(); dir.isDirectory }.getOrDefault(false)) return
        NativeBridge.nativeSetRealtimeLog(File(dir, "$slug.log").absolutePath)
    }

    /** Closes the live log; the core flushes and restores its previous direction. */
    fun stop() {
        NativeBridge.nativeSetRealtimeLog("")
    }
}
