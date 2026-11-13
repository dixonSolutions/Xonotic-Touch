package io.github.dixonsolutions.godnotic.fe

/**
 * A small JSON reader and writer for the front-end bridge: objects become
 * [Map]s (insertion ordered), arrays [List]s, numbers [Double]s. Plain Kotlin
 * with no Android classes, so the UI model built on it runs in JVM unit tests
 * (android.jar's org.json is a stub there).
 */
object Json {
    class ParseError(msg: String) : RuntimeException(msg)

    fun parse(text: String): Any? {
        val p = Parser(text)
        p.ws()
        val v = p.value()
        p.ws()
        if (p.i != text.length) throw ParseError("trailing data at ${p.i}")
        return v
    }

    fun parseOrNull(text: String?): Any? = try { if (text == null) null else parse(text) } catch (e: ParseError) { null }

    private class Parser(val s: String) {
        var i = 0
        fun ws() { while (i < s.length && s[i].isWhitespace()) i++ }
        fun value(): Any? {
            if (i >= s.length) throw ParseError("unexpected end")
            return when (val c = s[i]) {
                '{' -> obj()
                '[' -> arr()
                '"' -> str()
                't' -> lit("true", true)
                'f' -> lit("false", false)
                'n' -> lit("null", null)
                else -> if (c == '-' || c.isDigit()) num() else throw ParseError("unexpected '$c' at $i")
            }
        }
        fun lit(word: String, v: Any?): Any? {
            if (!s.startsWith(word, i)) throw ParseError("bad literal at $i")
            i += word.length
            return v
        }
        fun num(): Double {
            val start = i
            if (s[i] == '-') i++
            while (i < s.length && (s[i].isDigit() || s[i] == '.' || s[i] == 'e' || s[i] == 'E' || s[i] == '+' || s[i] == '-')) i++
            return s.substring(start, i).toDoubleOrNull() ?: throw ParseError("bad number at $start")
        }
        fun str(): String {
            i++ // opening quote
            val b = StringBuilder()
            while (true) {
                if (i >= s.length) throw ParseError("unterminated string")
                val c = s[i++]
                when (c) {
                    '"' -> return b.toString()
                    '\\' -> {
                        if (i >= s.length) throw ParseError("bad escape")
                        when (val e = s[i++]) {
                            'n' -> b.append('\n'); 't' -> b.append('\t'); 'r' -> b.append('\r')
                            'b' -> b.append('\b'); 'f' -> b.append('\u000c')
                            'u' -> {
                                if (i + 4 > s.length) throw ParseError("bad \\u escape")
                                b.append(s.substring(i, i + 4).toInt(16).toChar()); i += 4
                            }
                            else -> b.append(e)
                        }
                    }
                    else -> b.append(c)
                }
            }
        }
        fun arr(): List<Any?> {
            i++
            val out = ArrayList<Any?>()
            ws()
            if (i < s.length && s[i] == ']') { i++; return out }
            while (true) {
                ws(); out.add(value()); ws()
                if (i >= s.length) throw ParseError("unterminated array")
                when (s[i++]) { ',' -> continue; ']' -> return out; else -> throw ParseError("expected , or ] at ${i - 1}") }
            }
        }
        fun obj(): Map<String, Any?> {
            i++
            val out = LinkedHashMap<String, Any?>()
            ws()
            if (i < s.length && s[i] == '}') { i++; return out }
            while (true) {
                ws()
                if (i >= s.length || s[i] != '"') throw ParseError("expected key at $i")
                val k = str(); ws()
                if (i >= s.length || s[i++] != ':') throw ParseError("expected : at ${i - 1}")
                ws(); out[k] = value(); ws()
                if (i >= s.length) throw ParseError("unterminated object")
                when (s[i++]) { ',' -> continue; '}' -> return out; else -> throw ParseError("expected , or } at ${i - 1}") }
            }
        }
    }

    fun write(v: Any?): String = StringBuilder().also { write(it, v) }.toString()

    private fun write(b: StringBuilder, v: Any?) {
        when (v) {
            null -> b.append("null")
            is Boolean -> b.append(v)
            is Number -> {
                val d = v.toDouble()
                if (d == Math.floor(d) && !d.isInfinite() && Math.abs(d) < 1e15) b.append(d.toLong()) else b.append(d)
            }
            is String -> {
                b.append('"')
                for (c in v) when {
                    c == '"' -> b.append("\\\""); c == '\\' -> b.append("\\\\")
                    c == '\n' -> b.append("\\n"); c == '\t' -> b.append("\\t"); c == '\r' -> b.append("\\r")
                    c < ' ' -> b.append(String.format("\\u%04x", c.code))
                    else -> b.append(c)
                }
                b.append('"')
            }
            is Map<*, *> -> {
                b.append('{')
                var first = true
                for ((k, x) in v) { if (!first) b.append(','); first = false; write(b, k.toString()); b.append(':'); write(b, x) }
                b.append('}')
            }
            is Iterable<*> -> {
                b.append('[')
                var first = true
                for (x in v) { if (!first) b.append(','); first = false; write(b, x) }
                b.append(']')
            }
            else -> write(b, v.toString())
        }
    }
}

// Lenient accessors: the engine's JSON is the contract, but a missing or
// mistyped field degrades to a default rather than a crash.
@Suppress("UNCHECKED_CAST")
fun Any?.obj(): Map<String, Any?> = this as? Map<String, Any?> ?: emptyMap()
fun Any?.list(): List<Any?> = this as? List<Any?> ?: emptyList()
fun Map<String, Any?>.str(key: String, def: String = ""): String = when (val v = this[key]) {
    null -> def
    is String -> v
    is Double -> if (v == Math.floor(v) && !v.isInfinite()) v.toLong().toString() else v.toString()
    else -> v.toString()
}
fun Map<String, Any?>.num(key: String, def: Double = 0.0): Double = when (val v = this[key]) {
    is Number -> v.toDouble()
    is String -> v.toDoubleOrNull() ?: def
    is Boolean -> if (v) 1.0 else 0.0
    else -> def
}
fun Map<String, Any?>.int(key: String, def: Int = 0): Int = num(key, def.toDouble()).toInt()
fun Map<String, Any?>.bool(key: String, def: Boolean = false): Boolean = when (val v = this[key]) {
    is Boolean -> v
    is Number -> v.toDouble() != 0.0
    is String -> v == "1" || v.equals("true", true)
    else -> def
}
