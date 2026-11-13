// SPDX-License-Identifier: GPL-3.0-or-later
package xt.core

import java.io.ByteArrayOutputStream
import java.util.zip.CRC32
import java.util.zip.Inflater
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
import kotlin.test.*

class Pk3Test {
    private class Mem(val b: ByteArray) : Zip.Source {
        override val size get() = b.size.toLong()
        override fun read(offset: Long, length: Int) = b.copyOfRange(offset.toInt(), (offset + length).toInt().coerceAtMost(b.size))
    }

    private val jvm = object : Pk3.Decoders {
        override fun inflate(data: ByteArray, size: Int): ByteArray? {
            val inf = Inflater(true)
            inf.setInput(data)
            val out = ByteArray(size)
            val n = inf.inflate(out)
            inf.end()
            return out.takeIf { n == size }
        }
        var zstdCalls = 0
        override fun zstd(data: ByteArray, size: Int): ByteArray? { zstdCalls++; return "zstd!".toByteArray().takeIf { it.size == size } }
    }

    private fun pk3(): ByteArray {
        val bo = ByteArrayOutputStream()
        ZipOutputStream(bo).use { z ->
            val jpg = ByteArray(5000) { (it % 251).toByte() }
            val stored = ZipEntry("maps/stormkeep.jpg").apply {
                method = ZipEntry.STORED; size = jpg.size.toLong(); compressedSize = jpg.size.toLong()
                crc = CRC32().apply { update(jpg) }.value
            }
            z.putNextEntry(stored); z.write(jpg); z.closeEntry()
            z.putNextEntry(ZipEntry("maps/stormkeep.mapinfo")); z.write("title Storm Keep\ngametype dm\ngametype ctf\n".repeat(20).toByteArray()); z.closeEntry()
            z.putNextEntry(ZipEntry("maps/stormkeep.bsp")); z.write(ByteArray(100)); z.closeEntry()
        }
        return bo.toByteArray()
    }

    @Test
    fun readsStoredAndDeflatedEntries() {
        val src = Mem(pk3())
        val e = Pk3.entries(src)
        assertEquals(listOf("maps/stormkeep.jpg", "maps/stormkeep.mapinfo", "maps/stormkeep.bsp"), e.map { it.name })
        val jpg = assertNotNull(Pk3.read(src, Pk3.find(e, "MAPS/STORMKEEP.JPG")!!, jvm))
        assertEquals(5000, jpg.size)
        assertEquals(250.toByte(), jpg[250])
        val mi = Pk3.find(e, "maps/stormkeep.mapinfo")!!
        assertEquals(Pk3.DEFLATE, mi.method)
        val text = Pk3.read(src, mi, jvm)!!.decodeToString()
        assertEquals("Storm Keep", MapInfoFile.parse("stormkeep", text).title)
        assertNull(Pk3.read(src, e[0], jvm, limit = 100), "over the limit")
        assertEquals(listOf("stormkeep"), MapCatalog.fromEntries("x", e.map { it.name }).map { it.name })
    }

    @Test
    fun zstdEntriesGoToThePlatformDecoder() {
        // Rewrite the mapinfo's method to 93 (the slim set's zstd) in both headers: the reader must hand it to zstd().
        val b = pk3()
        val src0 = Mem(b)
        val e = Pk3.find(Pk3.entries(src0), "maps/stormkeep.mapinfo")!!
        b[(e.localHeader + 8).toInt()] = 93
        var p = b.size - 22
        while (!(b[p] == 0x50.toByte() && b[p + 1] == 0x4b.toByte() && b[p + 2] == 5.toByte() && b[p + 3] == 6.toByte())) p--
        var cd = (b[p + 16].toInt() and 255) or ((b[p + 17].toInt() and 255) shl 8) or ((b[p + 18].toInt() and 255) shl 16)
        while (b[cd] == 0x50.toByte() && b[cd + 1] == 0x4b.toByte() && b[cd + 2] == 1.toByte()) {
            val nl = (b[cd + 28].toInt() and 255) or ((b[cd + 29].toInt() and 255) shl 8)
            val name = String(b, cd + 46, nl)
            if (name.endsWith(".mapinfo")) b[cd + 10] = 93
            val xl = (b[cd + 30].toInt() and 255) or ((b[cd + 31].toInt() and 255) shl 8)
            val cl = (b[cd + 32].toInt() and 255) or ((b[cd + 33].toInt() and 255) shl 8)
            cd += 46 + nl + xl + cl
        }
        val src = Mem(b)
        val z = Pk3.find(Pk3.entries(src), "maps/stormkeep.mapinfo")!!
        assertEquals(Pk3.ZSTD, z.method)
        Pk3.read(src, z, jvm)
        assertEquals(1, jvm.zstdCalls)
    }
}
