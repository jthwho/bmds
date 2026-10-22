/**
 * @file      textdocument.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <compare>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/list.h>
#include <bmds/memspace.h>
#include <bmds/result.h>
#include <bmds/string.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/** @brief A place in a `TextDocument`: a line and a byte offset in it. */
struct TextPosition {
                uint32_t line = 0;
                uint32_t byte = 0;

                constexpr bool                 operator==(const TextPosition &other) const = default;
                constexpr std::strong_ordering operator<=>(const TextPosition &other) const = default;
};

/** @brief A stretch of a `TextDocument`, `start` to `end` (past it); `start <= end`. */
struct TextRange {
                TextPosition start;
                TextPosition end;

                constexpr bool isEmpty() const { return start == end; }
                constexpr bool contains(TextPosition p) const { return start <= p && p < end; }
                constexpr bool operator==(const TextRange &other) const = default;
                /** @brief The range between @p a and @p b, whichever comes first. */
                static constexpr TextRange between(TextPosition a, TextPosition b) {
                        return a <= b ? TextRange{a, b} : TextRange{b, a};
                }
};

/** @brief A cursor and the other end of its selection (equal: nothing selected). */
struct TextSelection {
                TextPosition cursor;
                TextPosition anchor;

                constexpr bool      hasSelection() const { return cursor != anchor; }
                constexpr TextRange range() const { return TextRange::between(cursor, anchor); }
                constexpr bool      operator==(const TextSelection &other) const = default;
};

/**
 * @brief Text being edited (`devplan/editor.md` §3): lines of UTF-8 bytes, edits that tell
 *        observers what changed, undo and redo in steps, find and replace.
 *
 * The text is an array of lines, each its bytes (no terminator), so edits, drawing and
 * highlighting reach any line at once. A loaded file's bytes stay in one block, which its
 * lines point into until each is first lengthened (it then gets a block of its own): a file
 * costs its size and 16 bytes a line. There is always at least one line;
 * a file ending in a newline has an empty last line, so loading and saving give back the same
 * bytes. What the file was is kept: LF or CR LF for each line (new lines take the document's
 * first), a UTF-8 byte order mark. Bytes that aren't valid UTF-8, NUL among them, are kept as
 * they are. Text inserted with CR LF has it read as a newline.
 *
 * **Undo:** edits between `beginEdit()` and `endEdit()` are one step, which undo takes back
 * whole, putting the selection back as it was before. Typing (`EditKind::Typing`) continues the
 * last step while it is typing too, within a second, with no move between (`breakMerge()`), and
 * until a word starts; deleting a character at a time likewise. The log keeps at most
 * `undoLimit()` bytes of text, the oldest steps going first. The step the file was saved at is
 * marked, so undoing back to it is unmodified again.
 *
 * Each line also keeps 16 bits for its user (`lineState()`: the syntax scanner's state at its
 * end), moved with the line and 0 for new or changed lines.
 *
 * @par Thread Safety
 * Not thread-safe: one owner (the GUI loop's thread).
 */
class TextDocument {
        public:
                /** @brief How a line ends. */
                enum class LineEnding : uint8_t {
                        Lf,   ///< `\n`.
                        CrLf, ///< `\r\n`.
                };

                /** @brief What an edit was, for merging steps. */
                enum class EditKind : uint8_t {
                        Other,  ///< A paste, a replace, an indent: a step of its own.
                        Typing, ///< A character typed (merges with the typing before it).
                        Delete, ///< A character deleted by Backspace or Delete (merges likewise).
                };

                /** @brief Find's choices (bits). */
                enum FindFlags : uint32_t {
                        FindCaseSensitive = 1u << 0, ///< Letters' case must match (ASCII's).
                        FindWholeWord = 1u << 1,     ///< Only where no word character is on either side.
                        FindBackward = 1u << 2,      ///< The last before `from`, not the first at or after.
                        FindWrap = 1u << 3,          ///< Past the end, from the start (backward: the reverse).
                };

                /**
                 * @brief What an edit changed: the lines `firstLine` to `firstLine + removedLines`
                 *        (past) were replaced by `insertedLines` lines from `firstLine`.
                 */
                struct Change {
                                uint32_t firstLine = 0;
                                uint32_t removedLines = 0;
                                uint32_t insertedLines = 0;
                };

                /** @brief Told of every change (an edit, an undo, a load). */
                using Changed = void (*)(void *arg, const Change &change);

                /** @brief The most observers. */
                static constexpr size_t MaxObservers = 4;
                /** @brief The undo log's default limit in bytes. */
                static constexpr size_t DefaultUndoLimit = 4u * 1024u * 1024u;
                /** @brief How long typing merges into the step before it. */
                static constexpr uint32_t MergeMs = 1000;

                /** @brief An empty document (one empty line, LF), its lines in @p space. */
                explicit TextDocument(MemSpace space = MemSpace::Psram);
                ~TextDocument();
                TextDocument(const TextDocument &) = delete;
                TextDocument &operator=(const TextDocument &) = delete;

                // ---- Loading and saving

                /**
                 * @brief Replaces the text with @p bytes (a file's contents): the line endings
                 *        and the byte order mark found, the undo log emptied, unmodified.
                 * @return `Ok`, or `NoMem` (the document then empty).
                 */
                Error load(StringView bytes);
                /** @brief Returns the bytes `save()` gives. */
                size_t savedSize() const;
                /** @brief Sets @p out to the text as a file holds it. @return `Ok` or `NoMem`. */
                Error save(String &out) const;
                /** @brief Marks the text as saved (unmodified from here). */
                void setSaved();
                /** @brief Returns true if the text differs from what was loaded or last saved. */
                bool isModified() const { return _savedStep != _step; }

                /** @brief Returns the line ending new lines take. */
                LineEnding lineEnding() const { return _ending; }
                /**
                 * @brief Sets the line ending of new lines, and of every line if @p all (the
                 *        text then modified; not an undo step).
                 */
                void setLineEnding(LineEnding ending, bool all);
                /** @brief Returns true if the file starts with a UTF-8 byte order mark. */
                bool hasByteOrderMark() const { return _bom; }
                /** @brief Saves with a byte order mark or without. */
                void setByteOrderMark(bool bom) { _bom = bom; }

                // ---- Reading

                /** @brief Returns the line count (at least 1). */
                uint32_t lineCount() const { return _count; }
                /** @brief Returns line @p index's bytes (no line ending); empty past the end. */
                StringView line(uint32_t index) const;
                /** @brief Returns how line @p index ends. */
                LineEnding lineEndingOf(uint32_t index) const;
                /** @brief Returns the bytes of text (lines and their newlines, one byte each). */
                size_t length() const { return _length; }
                /** @brief Returns the position past the last byte. */
                TextPosition end() const;
                /** @brief Returns @p p moved into the text (a line past the end: the end; a byte past a line's end: its end). */
                TextPosition clamp(TextPosition p) const;
                /** @brief Sets @p out to the text in @p range, lines joined by `\n`. @return `Ok` or `NoMem`. */
                Error text(TextRange range, String &out) const;
                /** @brief Returns the byte at @p p (`\n` at a line's end, 0 at the document's end). */
                char byteAt(TextPosition p) const;

                /** @brief Returns line @p index's state (its user's; 0 when new or changed). */
                uint16_t lineState(uint32_t index) const;
                /** @brief Sets line @p index's state. */
                void setLineState(uint32_t index, uint16_t state);

                // ---- Moving through the text (positions on character boundaries)

                /** @brief Returns the position after the character at @p p (the next line's start at a line's end). */
                TextPosition nextCharacter(TextPosition p) const;
                /** @brief Returns the position before the character before @p p. */
                TextPosition previousCharacter(TextPosition p) const;
                /** @brief Returns the start of the next word after @p p (or the line's end, then the next line's start). */
                TextPosition nextWord(TextPosition p) const;
                /** @brief Returns the start of the word before @p p. */
                TextPosition previousWord(TextPosition p) const;
                /** @brief Returns the word around @p p (letters, digits, `_`, any non-ASCII), or an empty range at @p p. */
                TextRange wordAt(TextPosition p) const;
                /** @brief Returns true for a byte of a word: an ASCII letter, digit or `_`, or any non-ASCII byte. */
                static bool isWordByte(char c);
                /**
                 * @brief Returns the columns @p codepoint is drawn in: 2 for a wide character, 0
                 *        for a combining mark, 1 for the rest (controls and the replacement too).
                 *        A tab is the caller's.
                 */
                static uint32_t characterColumns(uint32_t codepoint);

                /**
                 * @brief Returns the column @p byte of line @p index is drawn at: tabs to the next
                 *        multiple of @p tabWidth, wide characters two columns.
                 */
                uint32_t columnOf(uint32_t index, uint32_t byte, uint32_t tabWidth) const;
                /** @brief Returns the byte of line @p index nearest column @p column (the line's end past it). */
                uint32_t byteAtColumn(uint32_t index, uint32_t column, uint32_t tabWidth) const;

                // ---- Editing

                /**
                 * @brief Inserts @p text at @p at.
                 * @return The position after it, or `NoMem` (nothing inserted).
                 */
                Result<TextPosition> insert(TextPosition at, StringView text);
                /** @brief Removes @p range. @return `Ok` or `NoMem` (nothing removed). */
                Error remove(TextRange range);
                /** @brief Replaces @p range with @p text. @return The position after the text, or `NoMem`. */
                Result<TextPosition> replace(TextRange range, StringView text);

                // ---- Undo and redo

                /**
                 * @brief Starts a step: the edits until `endEdit()` are one, undone together, the
                 *        selection then back at @p before. A step of kind `Typing` or `Delete`
                 *        continues the last if that is of the same kind, its last edit less
                 *        than `MergeMs` before @p nowMs, nothing broke it, it isn't the saved
                 *        step, and (typing) @p typed doesn't start a word after something else.
                 */
                void beginEdit(const TextSelection &before, EditKind kind = EditKind::Other, uint32_t nowMs = 0,
                               StringView typed = StringView());
                /** @brief Ends the step begun, the selection then @p after (what redo puts back). */
                void endEdit(const TextSelection &after);
                /** @brief The next typing starts a step of its own (a move, a click). */
                void breakMerge() { _mergeable = false; }
                /** @brief Returns true if there is a step to undo. */
                bool canUndo() const { return _step > 0; }
                /** @brief Returns true if there is a step to redo. */
                bool canRedo() const { return _step < _steps.size(); }
                /**
                 * @brief Undoes the last step.
                 * @return The selection before it, or `Empty` (nothing to undo) or `NoMem` (the
                 *         log then emptied: the text is as it was after the step's part undone).
                 */
                Result<TextSelection> undo();
                /** @brief Redoes the step undone last. @return The selection after it, or as `undo()`. */
                Result<TextSelection> redo();
                /** @brief Empties the undo log (the text stays). */
                void clearUndo();
                /** @brief Returns the undo log's limit in bytes. */
                size_t undoLimit() const { return _undoLimit; }
                /** @brief Sets the undo log's limit (old steps dropped to fit). */
                void setUndoLimit(size_t bytes);
                /** @brief Returns the text the undo log holds, in bytes. */
                size_t undoBytes() const { return _undoBytes; }

                // ---- Find and replace

                /**
                 * @brief Finds @p needle (no newline in it) from @p from with @p flags.
                 * @return Its range, or `NotFound` (or `Invalid` for an empty needle or one with a newline).
                 */
                Result<TextRange> find(StringView needle, TextPosition from, uint32_t flags) const;
                /**
                 * @brief Replaces every @p needle with @p replacement (as one undo step if the
                 *        caller began one), from the start.
                 * @return How many, or `Invalid` (as `find()`) or `NoMem` (those done stay).
                 */
                Result<size_t> replaceAll(StringView needle, StringView replacement, uint32_t flags);

                // ---- Observers

                /** @brief Adds an observer told of every change. @return `Ok` or `Full`. */
                Error addObserver(Changed fn, void *arg);
                /** @brief Removes an observer added with the same @p fn and @p arg. */
                void removeObserver(Changed fn, void *arg);

        private:
                // A line's bytes: a block of its own (capacity > 0), or a part of the loaded
                // file's (capacity 0) until it first grows.
                struct Line {
                                char    *data = nullptr;
                                uint32_t length = 0;
                                uint32_t capacity = 0;
                                uint16_t state = 0;
                                bool     crlf = false;
                };
                struct Edit {
                                TextPosition at;
                                String       removed{MemSpace::Psram};
                                String       inserted{MemSpace::Psram};
                };
                struct Step {
                                TextSelection before;
                                TextSelection after;
                                List<Edit>    edits{MemSpace::Psram};
                                EditKind      kind = EditKind::Other;
                                uint32_t      lastMs = 0;
                                size_t        bytes = 0;
                };
                struct Observer {
                                Changed fn = nullptr;
                                void   *arg = nullptr;
                };

                Error                reserveLines(uint32_t count, bool exact = false);
                Error                fitLine(Line &line, size_t length); // capacity for length bytes
                void                 freeLine(Line &line);
                void                 freeAll();
                Error                resetToEmpty();
                Result<TextPosition> rawInsert(TextPosition at, StringView text);
                Error                rawRemove(TextRange range);
                Error                record(TextPosition at, TextRange removed, StringView inserted);
                void                 trimUndo();
                void                 dropRedo();
                void                 notify(const Change &change);
                TextPosition         endOfInsert(TextPosition at, StringView text) const;
                bool                 matchesAt(TextPosition p, StringView needle, uint32_t flags) const;

                MemSpace   _space;
                char      *_loaded = nullptr; // the file's bytes, which unedited lines are parts of
                Line      *_lines = nullptr;
                uint32_t   _count = 0;
                uint32_t   _capacity = 0;
                size_t     _length = 0; // bytes, newlines one each
                LineEnding _ending = LineEnding::Lf;
                bool       _bom = false;
                List<Step> _steps{MemSpace::Psram};
                size_t     _step = 0;      // steps done: undo takes _steps[_step - 1]
                size_t     _savedStep = 0; // the step the text was saved at (SIZE_MAX: gone from the log)
                size_t     _undoLimit = DefaultUndoLimit;
                size_t     _undoBytes = 0;
                uint32_t   _depth = 0;         // beginEdit()s not yet ended (only the outermost counts)
                bool       _mergeable = false; // the last step may take more typing
                bool       _replaying = false; // undo or redo: edits aren't recorded
                char       _lastTyped = 0;
                Observer   _observers[MaxObservers];
};

/**
 * @brief A cursor in a `TextDocument` and its selection, moved as editors move them
 *        (`devplan/editor.md` §3): by character, word, line, page and document, Up and Down
 *        keeping the column wanted.
 *
 * Columns are as drawn (`TextDocument::columnOf()`), so its tab width must be the view's.
 *
 * @par Thread Safety
 * As its document's.
 */
class TextCursor {
        public:
                explicit TextCursor(const TextDocument &document) : _document(&document) {}

                /** @brief Returns the cursor and the selection's other end. */
                const TextSelection &selection() const { return _selection; }
                /** @brief Returns the cursor's position. */
                TextPosition position() const { return _selection.cursor; }
                /** @brief Returns the selection's other end. */
                TextPosition anchor() const { return _selection.anchor; }
                /** @brief Returns true if something is selected. */
                bool hasSelection() const { return _selection.hasSelection(); }
                /** @brief Returns the selected range (empty at the cursor). */
                TextRange range() const { return _selection.range(); }

                /** @brief Returns the tab width columns are counted with. */
                uint32_t tabWidth() const { return _tabWidth; }
                /** @brief Sets the tab width (1 at least). */
                void setTabWidth(uint32_t width) {
                        _tabWidth = width != 0 ? width : 1;
                        return;
                }

                /** @brief Puts the cursor at @p p (clamped); the anchor too unless @p select. */
                void setPosition(TextPosition p, bool select = false);
                /** @brief Sets the cursor and anchor (clamped). */
                void setSelection(const TextSelection &selection);
                /** @brief Moves both into the document after it changed. */
                void clamp();

                void moveLeft(bool select);
                void moveRight(bool select);
                void moveWordLeft(bool select);
                void moveWordRight(bool select);
                void moveUp(bool select, uint32_t lines = 1);
                void moveDown(bool select, uint32_t lines = 1);
                /** @brief To the line's first non-blank, or its start if already there (or if @p smart is false). */
                void moveLineStart(bool select, bool smart = true);
                void moveLineEnd(bool select);
                void moveDocumentStart(bool select);
                void moveDocumentEnd(bool select);

                /** @brief Selects all of the text, the cursor at the end. */
                void selectAll();
                /** @brief Selects the word at the cursor. */
                void selectWord();
                /** @brief Selects the cursor's line and its newline. */
                void selectLine();

        private:
                void horizontal(TextPosition to, bool select); // forgets the wanted column
                void vertical(uint32_t line, bool select);

                const TextDocument *_document;
                TextSelection       _selection;
                uint32_t            _tabWidth = 8;
                int64_t             _wantColumn = -1; // Up and Down's; -1: from the cursor
};

BMDS_NAMESPACE_END
