// SPDX-License-Identifier: GPL-3.0-or-later
@file:OptIn(ExperimentalForeignApi::class)

package xt.ui

import kotlinx.cinterop.*
import platform.posix.*
import xt.core.LaunchPlan
import xt.core.Zip

/** Plain POSIX file and process helpers; nothing here touches GTK. */
object Sys {
    fun env(): Map<String, String> {
        val out = HashMap<String, String>()
        var p = __environ ?: return out
        while (true) {
            val s = p.pointed.value?.toKString() ?: break
            val eq = s.indexOf('=')
            if (eq > 0) out[s.substring(0, eq)] = s.substring(eq + 1)
            p = (p + 1)!!
        }
        return out
    }

    fun exists(path: String) = access(path, F_OK) == 0

    fun isDir(path: String): Boolean = memScoped {
        val st = alloc<stat>()
        stat(path, st.ptr) == 0 && (st.st_mode.toInt() and S_IFMT) == S_IFDIR
    }

    fun size(path: String): Long = memScoped {
        val st = alloc<stat>()
        if (stat(path, st.ptr) == 0) st.st_size else -1L
    }

    fun exeDir(): String? = memScoped {
        val buf = allocArray<ByteVar>(4096)
        val n = readlink("/proc/self/exe", buf, 4095u)
        if (n <= 0) return null
        buf[n] = 0
        val p = buf.toKString()
        p.substring(0, p.lastIndexOf('/').coerceAtLeast(1))
    }

    fun list(dir: String): List<String> {
        val d = opendir(dir) ?: return emptyList()
        val out = mutableListOf<String>()
        while (true) {
            val e = readdir(d) ?: break
            val n = e.pointed.d_name.toKString()
            if (n != "." && n != "..") out += n
        }
        closedir(d)
        return out.sorted()
    }

    fun readText(path: String, limit: Int = 1 shl 20): String? {
        val f = fopen(path, "rb") ?: return null
        try {
            val out = ByteArray(limit)
            var total = 0
            out.usePinned { pin ->
                while (total < limit) {
                    val n = fread(pin.addressOf(total), 1u, (limit - total).toULong(), f).toInt()
                    if (n <= 0) break
                    total += n
                }
            }
            return out.decodeToString(0, total)
        } finally {
            fclose(f)
        }
    }

    /** Writes through a temp file and rename, so a crash never leaves half a file. */
    fun writeText(path: String, text: String): Boolean {
        mkdirs(path.substring(0, path.lastIndexOf('/').coerceAtLeast(1)))
        val tmp = "$path.tmp"
        val f = fopen(tmp, "wb") ?: return false
        val bytes = text.encodeToByteArray()
        val ok = bytes.isEmpty() || bytes.usePinned { fwrite(it.addressOf(0), 1u, bytes.size.toULong(), f) == bytes.size.toULong() }
        if (fclose(f) != 0 || !ok) return false
        return rename(tmp, path) == 0
    }

    fun mkdirs(dir: String) {
        var cur = ""
        for (part in dir.split('/')) {
            if (part.isEmpty()) { cur = if (cur.isEmpty()) "" else cur; continue }
            cur += "/$part"
            if (!exists(cur)) mkdir(cur, 0x1edu) // 0755
        }
    }

    /** A pk3 opened for Zip.entryNames; reads happen with pread, no full load. */
    class FileSource(private val fd: Int, override val size: Long) : Zip.Source {
        override fun read(offset: Long, length: Int): ByteArray {
            val out = ByteArray(length)
            if (length == 0) return out
            var got = 0
            out.usePinned { pin ->
                while (got < length) {
                    val n = pread(fd, pin.addressOf(got), (length - got).toULong(), offset + got).toInt()
                    if (n <= 0) break
                    got += n
                }
            }
            return out
        }
    }

    fun <T> withZip(path: String, f: (Zip.Source) -> T): T? {
        val fd = open(path, O_RDONLY)
        if (fd < 0) return null
        try {
            val sz = memScoped { val st = alloc<stat>(); if (fstat(fd, st.ptr) == 0) st.st_size else -1L }
            if (sz <= 0) return null
            return f(FileSource(fd, sz))
        } finally {
            close(fd)
        }
    }

    /**
     * Replaces this process with the game. The launcher's memory is gone the
     * moment the game starts, and the Flatpak instance keeps its PID.
     * Returns only on failure, with the reason.
     */
    fun exec(plan: LaunchPlan): String {
        for ((k, v) in plan.env) setenv(k, v, 1)
        plan.cwd?.let { chdir(it) }
        memScoped {
            val argv = allocArrayOf((listOf(plan.program) + plan.args).map { it.cstr.ptr } + listOf(null))
            execv(plan.program, argv)
        }
        return strerror(errno)?.toKString() ?: "exec failed"
    }
}
