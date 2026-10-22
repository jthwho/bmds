/**
 * @file      files.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Files and directories: `File` (opened for reading, writing or appending; lines, all of
 * it, values written as print() writes them), and functions for whole files and
 * directories. What fails raises an exception that says why ("Can't open 'x': No such file
 * or directory"), which a catch block can handle.
 */

#include <dirent.h>
#include <errno.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../../thirdparty/angelscript/add_on/scriptarray/scriptarray.h"
#include "angel.h"

namespace angel {

        namespace {

                // A script's open file: reference counted (asOBJ_REF), closed with its last reference.
                class File {
                        public:
                                File(FILE *f, bool writing) : _file(f), _writing(writing) {}
                                ~File() { close(); }

                                void addRef() { ++_refs; }
                                void release() {
                                        if (--_refs == 0) delete this;
                                }
                                void close() {
                                        if (_file != nullptr) fclose(_file);
                                        _file = nullptr;
                                }
                                FILE *file() const { return _file; }
                                bool  writing() const { return _writing; }

                        private:
                                FILE *_file;
                                bool  _writing;
                                int   _refs = 1;
                };

                const String &arg(asIScriptGeneric *gen, asUINT n) {
                        return *static_cast<const String *>(gen->GetArgAddress(n));
                }
                File *self(asIScriptGeneric *gen) {
                        return static_cast<File *>(gen->GetObject());
                }
                void returnText(asIScriptGeneric *gen, const Text &text) {
                        new (gen->GetAddressOfReturnLocation()) String(text.data(), text.length());
                }

                // The open file for reading or writing, or an exception.
                FILE *readable(File *f) {
                        if (f->file() == nullptr) {
                                raise("The file is closed");
                                return nullptr;
                        }
                        if (f->writing()) {
                                raise("The file was opened for writing, not reading");
                                return nullptr;
                        }
                        return f->file();
                }
                FILE *writable(File *f) {
                        if (f->file() == nullptr) {
                                raise("The file is closed");
                                return nullptr;
                        }
                        if (!f->writing()) {
                                raise("The file was opened for reading, not writing");
                                return nullptr;
                        }
                        return f->file();
                }

                void open(asIScriptGeneric *gen) {
                        const String &path = arg(gen, 0), &mode = arg(gen, 1);
                        const char   *m = nullptr;
                        if (strcmp(mode.cStr(), "r") == 0)
                                m = "rb";
                        else if (strcmp(mode.cStr(), "w") == 0)
                                m = "wb";
                        else if (strcmp(mode.cStr(), "a") == 0)
                                m = "ab";
                        if (m == nullptr) {
                                raise("File mode '%s' isn't \"r\" (read), \"w\" (write) or \"a\" (append)",
                                      mode.cStr());
                                return;
                        }
                        FILE *f = fopen(path.cStr(), m);
                        if (f == nullptr) {
                                raise("Can't open '%s': %s", path.cStr(), strerror(errno));
                                return;
                        }
                        gen->SetReturnAddress(new File(f, m[0] != 'r'));
                }
                void addRef(asIScriptGeneric *gen) {
                        self(gen)->addRef();
                }
                void release(asIScriptGeneric *gen) {
                        self(gen)->release();
                }
                void readLine(asIScriptGeneric *gen) {
                        FILE *f = readable(self(gen));
                        Text  line;
                        if (f != nullptr) {
                                int c;
                                while ((c = fgetc(f)) != EOF && c != '\n') {
                                        const char ch = char(c);
                                        line.append(&ch, 1);
                                }
                                if (line.length() != 0 && line.data()[line.length() - 1] == '\r') {
                                        Text trimmed;
                                        trimmed.append(line.data(), line.length() - 1);
                                        returnText(gen, trimmed);
                                        return;
                                }
                        }
                        returnText(gen, line);
                }
                void readAll(asIScriptGeneric *gen) {
                        FILE *f = readable(self(gen));
                        Text  text;
                        if (f != nullptr) {
                                char   chunk[4096];
                                size_t n;
                                while ((n = fread(chunk, 1, sizeof(chunk), f)) != 0) text.append(chunk, n);
                        }
                        returnText(gen, text);
                }
                void eof(asIScriptGeneric *gen) {
                        FILE *f = readable(self(gen));
                        bool  end = true;
                        if (f != nullptr) {
                                const int c = fgetc(f);
                                if (c != EOF) {
                                        ungetc(c, f);
                                        end = false;
                                }
                        }
                        gen->SetReturnByte(end);
                }
                void writeValue(asIScriptGeneric *gen, bool newline) {
                        FILE *f = writable(self(gen));
                        if (f == nullptr) return;
                        Text text;
                        formatValue(gen->GetEngine(), text, gen->GetArgAddress(0), gen->GetArgTypeId(0));
                        if (newline) text.append("\n");
                        if (fwrite(text.data(), 1, text.length(), f) != text.length())
                                raise("Writing failed: %s", strerror(errno));
                }
                void write(asIScriptGeneric *gen) {
                        writeValue(gen, false);
                }
                void writeLine(asIScriptGeneric *gen) {
                        writeValue(gen, true);
                }
                void close(asIScriptGeneric *gen) {
                        self(gen)->close();
                }
                void isOpen(asIScriptGeneric *gen) {
                        gen->SetReturnByte(self(gen)->file() != nullptr);
                }

                // ---- Whole files and directories

                void fileExists(asIScriptGeneric *gen) {
                        struct stat st;
                        gen->SetReturnByte(stat(arg(gen, 0).cStr(), &st) == 0);
                }
                void isDirectory(asIScriptGeneric *gen) {
                        struct stat st;
                        gen->SetReturnByte(stat(arg(gen, 0).cStr(), &st) == 0 && S_ISDIR(st.st_mode));
                }
                void removeFile(asIScriptGeneric *gen) {
                        const char *path = arg(gen, 0).cStr();
                        if (remove(path) != 0) raise("Can't remove '%s': %s", path, strerror(errno));
                }
                void renameFile(asIScriptGeneric *gen) {
                        const char *from = arg(gen, 0).cStr(), *to = arg(gen, 1).cStr();
                        if (rename(from, to) != 0) raise("Can't rename '%s' to '%s': %s", from, to, strerror(errno));
                }
                void readFile(asIScriptGeneric *gen) {
                        const char *path = arg(gen, 0).cStr();
                        Text        text;
                        FILE       *f = fopen(path, "rb");
                        if (f == nullptr) {
                                raise("Can't open '%s': %s", path, strerror(errno));
                                returnText(gen, text);
                                return;
                        }
                        char   chunk[4096];
                        size_t n;
                        while ((n = fread(chunk, 1, sizeof(chunk), f)) != 0) text.append(chunk, n);
                        fclose(f);
                        returnText(gen, text);
                }
                void putFile(asIScriptGeneric *gen, const char *mode) {
                        const char   *path = arg(gen, 0).cStr();
                        const String &content = arg(gen, 1);
                        FILE         *f = fopen(path, mode);
                        if (f == nullptr) {
                                raise("Can't open '%s': %s", path, strerror(errno));
                                return;
                        }
                        const bool wrote = fwrite(content.data(), 1, content.length(), f) == content.length();
                        if (fclose(f) != 0 || !wrote) raise("Writing '%s' failed: %s", path, strerror(errno));
                }
                void writeFile(asIScriptGeneric *gen) {
                        putFile(gen, "wb");
                }
                void appendFile(asIScriptGeneric *gen) {
                        putFile(gen, "ab");
                }
                int compareNames(const void *a, const void *b) {
                        return strcmp(*static_cast<char *const *>(a), *static_cast<char *const *>(b));
                }
                void listDirectory(asIScriptGeneric *gen) {
                        const char *path = arg(gen, 0).cStr();
                        DIR        *dir = opendir(path);
                        if (dir == nullptr) {
                                raise("Can't list '%s': %s", path, strerror(errno));
                                return;
                        }
                        char         **names = nullptr;
                        size_t         count = 0, capacity = 0;
                        struct dirent *e;
                        while ((e = readdir(dir)) != nullptr) {
                                if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
                                if (count == capacity) {
                                        capacity = capacity != 0 ? capacity * 2 : 32;
                                        char **grown = static_cast<char **>(realloc(names, capacity * sizeof(char *)));
                                        if (grown == nullptr) break;
                                        names = grown;
                                }
                                names[count] = strdup(e->d_name);
                                if (names[count] != nullptr) ++count;
                        }
                        closedir(dir);
                        qsort(names, count, sizeof(char *), compareNames);
                        CScriptArray *array = CScriptArray::Create(stringArrayType(gen->GetEngine()));
                        for (size_t i = 0; i < count; ++i) {
                                if (array != nullptr) {
                                        String name(names[i]);
                                        array->InsertLast(&name);
                                }
                                free(names[i]);
                        }
                        free(names);
                        gen->SetReturnAddress(array);
                }
                void makeDirectory(asIScriptGeneric *gen) {
                        const char *path = arg(gen, 0).cStr();
                        if (mkdir(path, 0777) != 0) raise("Can't make the directory '%s': %s", path, strerror(errno));
                }
                void currentDirectory(asIScriptGeneric *gen) {
                        char buffer[1024];
                        Text text;
                        if (getcwd(buffer, sizeof(buffer)) != nullptr) text.append(buffer);
                        returnText(gen, text);
                }
                void changeDirectory(asIScriptGeneric *gen) {
                        const char *path = arg(gen, 0).cStr();
                        if (chdir(path) != 0) raise("Can't go to '%s': %s", path, strerror(errno));
                }

        } // namespace

        void registerFiles(asIScriptEngine *engine) {
                check(engine->RegisterObjectType("File", 0, asOBJ_REF), "File");
                behaviour(engine, "File", asBEHAVE_FACTORY,
                          "File@ f(const string &in path, const string &in mode = \"r\")", open,
                          "Opens a file: mode \"r\" to read, \"w\" to write (from empty), \"a\" to add to its end.");
                behaviour(engine, "File", asBEHAVE_ADDREF, "void f()", addRef, nullptr);
                behaviour(engine, "File", asBEHAVE_RELEASE, "void f()", release, nullptr);
                method(engine, "File", "string readLine()", readLine, "The next line, without its newline.");
                method(engine, "File", "string readAll()", readAll, "All the rest of the file.");
                method(engine, "File", "bool eof()", eof, "Nothing more to read.");
                method(engine, "File", "void write(const ?&in)", write, "Writes a value, as print() shows it.");
                method(engine, "File", "void writeLine(const ?&in)", writeLine, "Writes a value, then a newline.");
                method(engine, "File", "void close()", close, "Closes the file (also done when it's no longer used).");
                method(engine, "File", "bool isOpen() const", isOpen, "The file is open (not closed yet).");

                def(engine, "bool fileExists(const string &in path)", fileExists,
                    "There is a file or directory there.");
                def(engine, "bool isDirectory(const string &in path)", isDirectory, "There is a directory there.");
                def(engine, "void removeFile(const string &in path)", removeFile,
                    "Deletes a file (or an empty directory).");
                def(engine, "void renameFile(const string &in from, const string &in to)", renameFile,
                    "Renames (or moves) a file.");
                def(engine, "string readFile(const string &in path)", readFile, "All of a file, as a string.");
                def(engine, "void writeFile(const string &in path, const string &in content)", writeFile,
                    "Makes a file holding the content (replacing one that's there).");
                def(engine, "void appendFile(const string &in path, const string &in content)", appendFile,
                    "Adds the content to a file's end (making it if there's none).");
                def(engine, "array<string>@ listDirectory(const string &in path = \".\")", listDirectory,
                    "The names in a directory, sorted.");
                def(engine, "void makeDirectory(const string &in path)", makeDirectory, "Makes a directory.");
                def(engine, "string currentDirectory()", currentDirectory, "The directory relative paths start from.");
                def(engine, "void changeDirectory(const string &in path)", changeDirectory,
                    "Changes the current directory.");
                return;
        }

} // namespace angel
