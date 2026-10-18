# JSON: a pull reader and a streaming writer in `bmds_core`

Status: **built** (2026-10-17, with `bench.md` step 1; agreed with the user the same day). The
system's JSON, for every component that needs it: first the bench's results files.

## 1. Goal

- **Native JSON in the firmware** (the user's): one reader and one writer that every component
  uses, rather than a library per use.
- **Memory-efficient** (the user's): constant memory, no heap of their own, documents of any size
  read and written as streams, never held whole.

## 2. Why our own (checked 2026-10-17)

- **cJSON** (IDF's): a heap allocation for every value and every string, and every number a
  `double`. The P4's FPU is single precision: `double` arithmetic is software.
- **jsmn:** no allocation, but the whole document in memory and a token array sized before;
  strings and numbers left undecoded.
- **ArduinoJson:** heavy templates; its current version allocates from the heap.
- **sheredom's json.h:** one allocation, but still a whole tree, and `double`s.

RFC 8259's grammar is small: a reader and a writer are about 2-3 KB of code together.

## 3. Design

### 3a. `JsonWriter`

- Writes through a sink (`Error (*)(void *arg, StringView bytes)`), or appends to a `String`,
  from a 128-byte buffer of its own (flushed when full and by `finish()`).
- `beginObject()`, `endObject()`, `beginArray()`, `endArray()`, `key()`, `value()` (strings,
  any integer exactly, `double` (shortest of `%.15g` and `%.17g` that reads back the same;
  NaN and the infinities as `null`), `bool`), `null()`, and `member(key, value)` for both.
- Strings escaped as RFC 8259 requires (`"`, `\`, controls; `\u00XX` for those without a short
  form); the bytes otherwise as given (UTF-8).
- Pretty-printed with an indent (`setIndent()`: spaces a level; 0, the default, is compact).
- Errors are sticky: the first (the sink's, or a misuse: a value where a key belongs, an end
  that doesn't match, deeper than `MaxDepth`) stops the writing; `finish()` returns it, or
  `Invalid` if a container is left open.
- Nesting kept a bit a level in a `uint64_t`: `MaxDepth` 64.

### 3b. `JsonReader`

- A pull parser: `next()` returns a token: `BeginObject`, `EndObject`, `BeginArray`, `EndArray`,
  `Key`, `String`, `Number`, `True`, `False`, `Null`, `End` (the document is done, nothing but
  whitespace after it), or `Error` (`error()`, `line()`, `column()`).
- Reads a whole text (`StringView`) or a stream (a source,
  `Result<size_t> (*)(void *arg, char *buf, size_t bytes)`, 0 at the end, into the caller's
  buffer): a document never has to fit in RAM.
- Keys and strings decoded (escapes, surrogate pairs into UTF-8) into the caller's scratch
  buffer, `text()` valid until the next call; a string longer than the scratch is
  `BufferTooSmall`, never a reallocation. A number's text as written, in the same buffer.
- Numbers: `toInt64()` and `toUint64()` exact (`OutOfRange` past the type, `Invalid` with a
  fraction or exponent), `toDouble()` and `toFloat()` when asked for.
- `skip()`: after a `BeginObject` or `BeginArray`, reads to its end (to pass over what a reader
  doesn't know: files gain fields, `bench.md` §5).
- **Strict:** whitespace is RFC 8259's four; no byte-order mark, comments, trailing commas, single
  quotes, leading zeros or bare `NaN`; strings must be valid UTF-8 (no overlong forms, no
  surrogates, nothing past U+10FFFF), with no raw control characters and no lone surrogate
  escapes; nesting deeper than `MaxDepth` (64) is `OutOfRange`.
- State: the nesting bits, a state, the position (line and column), the buffers' pointers: a few
  dozen bytes.

## 4. Tests (`tests/host/json.cpp`)

- **JSONTestSuite** (`thirdparty/jsontestsuite`, MIT): every `y_` document read to `End`, every
  `n_` one an `Error`, each `i_` one's outcome pinned; each read whole and again a byte at a time
  through a source, with the same outcome.
- Tokens and decoded values (escapes, surrogate pairs, every number form, integer limits).
- The writer: escaping, numbers, indenting, misuse errors, a sink's error; what it writes read
  back by the reader to the same tokens.

## 5. Later

- A tree (`JsonValue`) for small documents read whole, in one arena, if a use wants one.
- Programs: through the firmware's runtime library (`programs.md` §11) when it exists.
