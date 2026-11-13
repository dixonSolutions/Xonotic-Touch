// SPDX-License-Identifier: GPL-3.0-or-later
package xt.core.fetch

/** CRC-32 as zip uses it (IEEE 802.3, reflected), slicing by 4. */
class Crc32 {
    private var crc = 0.inv()

    fun update(b: ByteArray, off: Int = 0, len: Int = b.size) {
        var c = crc
        var i = off
        val end = off + len
        while (end - i >= 4) {
            c = c xor ((b[i].toInt() and 0xff) or ((b[i + 1].toInt() and 0xff) shl 8) or
                ((b[i + 2].toInt() and 0xff) shl 16) or ((b[i + 3].toInt() and 0xff) shl 24))
            c = T3[c and 0xff] xor T2[(c ushr 8) and 0xff] xor T1[(c ushr 16) and 0xff] xor T0[c ushr 24]
            i += 4
        }
        while (i < end) {
            c = T0[(c xor b[i].toInt()) and 0xff] xor (c ushr 8)
            i++
        }
        crc = c
    }

    val value: Long get() = crc.inv().toLong() and 0xFFFFFFFFL

    private companion object {
        val T0 = IntArray(256) { n ->
            var c = n
            repeat(8) { c = if (c and 1 != 0) (c ushr 1) xor 0xEDB88320.toInt() else c ushr 1 }
            c
        }
        val T1 = IntArray(256) { (T0[it] ushr 8) xor T0[T0[it] and 0xff] }
        val T2 = IntArray(256) { (T1[it] ushr 8) xor T0[T1[it] and 0xff] }
        val T3 = IntArray(256) { (T2[it] ushr 8) xor T0[T2[it] and 0xff] }
    }
}
