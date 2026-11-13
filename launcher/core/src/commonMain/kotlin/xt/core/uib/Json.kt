// SPDX-License-Identifier: GPL-3.0-or-later
package xt.core.uib

/**
 * A small JSON reader and writer for the UI bridge messages: objects become
 * LinkedHashMap<String, Any?>, arrays List<Any?>, numbers Long (when whole and
 * in range) or Double, plus String, Boolean and null. No reflection and no
 * library, so the loader binary stays small and starts fast.
 */
object Json {
    class Error(message: String) : Exception(message)

    fun parse(text: String): Any? {
        val p = Parser(text)
        p.ws()
        val v = p.value(0)
        p.ws()
        if (p.i != text.length) throw Error("trailing text at ${p.i}")
        return v
    }

    /** An object, or null when [text] is not a JSON object. */
    @Suppress("UNCHECKED_CAST")
    fun obj(text: String): Map<String, Any?>? = runCatching { parse(text) as? Map<String, Any?> }.getOrNull()

    private class Parser(val s: String) {
        var i = 0

        fun ws() {
            while (i < s.length && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) i++
        }

        fun value(depth: Int): Any? {
            if (depth > 64) throw Error("nested too deep")
            if (i >= s.length) throw Error("unexpected end")
            return when (val c = s[i]) {
                '{' -> obj(depth)
                '[' -> arr(depth)
                '"' -> str()
                't' -> lit("true", true)
                'f' -> lit("false", false)
                'n' -> lit("null", null)
                else -> if (c == '-' || c in '0'..'9') num() else throw Error("unexpected '$c' at $i")
            }
        }

        fun lit(word: String, v: Any?): Any? {
            if (!s.startsWith(word, i)) throw Error("bad literal at $i")
            i += word.length
            return v
        }

        fun obj(depth: Int): Map<String, Any?> {
            val m = LinkedHashMap<String, Any?>()
            i++
            ws()
            if (i < s.length && s[i] == '}') { i++; return m }
            while (true) {
                ws()
                if (i >= s.length || s[i] != '"') throw Error("expected a key at $i")
                val k = str()
                ws()
                if (i >= s.length || s[i] != ':') throw Error("expected ':' at $i")
                i++
                ws()
                m[k] = value(depth + 1)
                ws()
                if (i >= s.length) throw Error("unexpected end")
                if (s[i] == ',') { i++; continue }
                if (s[i] == '}') { i++; return m }
                throw Error("expected ',' or '}' at $i")
            }
        }

        fun arr(depth: Int): List<Any?> {
            val l = ArrayList<Any?>()
            i++
            ws()
            if (i < s.length && s[i] == ']') { i++; return l }
            while (true) {
                ws()
                l += value(depth + 1)
                ws()
                if (i >= s.length) throw Error("unexpected end")
                if (s[i] == ',') { i++; continue }
                if (s[i] == ']') { i++; return l }
                throw Error("expected ',' or ']' at $i")
            }
        }

        fun str(): String {
            val sb = StringBuilder()
            i++
            while (true) {
                if (i >= s.length) throw Error("unterminated string")
                val c = s[i++]
                when {
                    c == '"' -> return sb.toString()
                    c == '\\' -> {
                        if (i >= s.length) throw Error("bad escape")
                        when (val e = s[i++]) {
                            '"', '\\', '/' -> sb.append(e)
                            'b' -> sb.append('\b')
                            'f' -> sb.append('\u000c')
                            'n' -> sb.append('\n')
                            'r' -> sb.append('\r')
                            't' -> sb.append('\t')
                            'u' -> {
                                if (i + 4 > s.length) throw Error("bad \\u escape")
                                sb.append(s.substring(i, i + 4).toInt(16).toChar())
                                i += 4
                            }
                            else -> throw Error("bad escape \\$e")
                        }
                    }
                    else -> sb.append(c)
                }
            }
        }

        fun num(): Any {
            val start = i
            if (s[i] == '-') i++
            while (i < s.length && (s[i] in '0'..'9' || s[i] in ".eE+-")) i++
            val t = s.substring(start, i)
            if ('.' !in t && 'e' !in t && 'E' !in t) t.toLongOrNull()?.let { return it }
            return t.toDoubleOrNull() ?: throw Error("bad number '$t'")
        }
    }

    fun write(v: Any?): String = StringBuilder().also { write(it, v) }.toString()

    private fun write(sb: StringBuilder, v: Any?) {
        when (v) {
            null -> sb.append("null")
            is Boolean -> sb.append(v)
            is Int, is Long -> sb.append(v)
            is Number -> {
                val d = v.toDouble()
                if (d.isNaN() || d.isInfinite()) sb.append("null")
                else if (d == kotlin.math.floor(d) && kotlin.math.abs(d) < 1e15) sb.append(d.toLong()) else sb.append(d)
            }
            is String -> quote(sb, v)
            is Map<*, *> -> {
                sb.append('{')
                var first = true
                for ((k, x) in v) {
                    if (!first) sb.append(',')
                    first = false
                    quote(sb, k.toString())
                    sb.append(':')
                    write(sb, x)
                }
                sb.append('}')
            }
            is Iterable<*> -> {
                sb.append('[')
                var first = true
                for (x in v) {
                    if (!first) sb.append(',')
                    first = false
                    write(sb, x)
                }
                sb.append(']')
            }
            else -> quote(sb, v.toString())
        }
    }

    private fun quote(sb: StringBuilder, s: String) {
        sb.append('"')
        for (c in s) when {
            c == '"' -> sb.append("\\\"")
            c == '\\' -> sb.append("\\\\")
            c == '\n' -> sb.append("\\n")
            c == '\r' -> sb.append("\\r")
            c == '\t' -> sb.append("\\t")
            c.code < 32 -> sb.append("\\u").append(c.code.toString(16).padStart(4, '0'))
            else -> sb.append(c)
        }
        sb.append('"')
    }
}

/** Typed reads from a parsed object, tolerant of what the other side sends. */
fun Map<String, Any?>.str(k: String): String? = when (val v = this[k]) {
    null -> null
    is String -> v
    is Number, is Boolean -> v.toString()
    else -> null
}

fun Map<String, Any?>.num(k: String): Double? = when (val v = this[k]) {
    is Number -> v.toDouble()
    is String -> v.toDoubleOrNull()
    is Boolean -> if (v) 1.0 else 0.0
    else -> null
}

fun Map<String, Any?>.int(k: String): Int? = num(k)?.toInt()

fun Map<String, Any?>.bool(k: String): Boolean? = when (val v = this[k]) {
    is Boolean -> v
    is Number -> v.toDouble() != 0.0
    is String -> v == "1" || v.equals("true", true)
    else -> null
}

@Suppress("UNCHECKED_CAST")
fun Map<String, Any?>.obj(k: String): Map<String, Any?>? = this[k] as? Map<String, Any?>

fun Map<String, Any?>.list(k: String): List<Any?>? = this[k] as? List<Any?>
