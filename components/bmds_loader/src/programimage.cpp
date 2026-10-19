/**
 * @file      programimage.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <algorithm>
#include <cstring>
#include <bmds/memory.h>
#include <bmds/program.h>
#include <bmds/programimage.h>
#include <bmds/sys.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using ProgramImage::Problem;

        // ELF32, as much as the format uses (little endian, no padding).
        struct Ehdr {
                        uint8_t  ident[16];
                        uint16_t type;
                        uint16_t machine;
                        uint32_t version;
                        uint32_t entry;
                        uint32_t phoff;
                        uint32_t shoff;
                        uint32_t flags;
                        uint16_t ehsize;
                        uint16_t phentsize;
                        uint16_t phnum;
                        uint16_t shentsize;
                        uint16_t shnum;
                        uint16_t shstrndx;
        };
        struct Phdr {
                        uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
        };
        struct Shdr {
                        uint32_t name, type, flags, addr, offset, size, link, info, addralign, entsize;
        };
        struct Sym {
                        uint32_t name, value, size;
                        uint8_t  info, other;
                        uint16_t shndx;
        };
        struct Rela {
                        uint32_t offset, info;
                        int32_t  addend;
        };
        static_assert(sizeof(Ehdr) == 52 && sizeof(Phdr) == 32 && sizeof(Shdr) == 40);
        static_assert(sizeof(Sym) == 16 && sizeof(Rela) == 12);

        constexpr uint16_t EtExec = 2;
        constexpr uint16_t EmRiscv = 243;
        constexpr uint32_t PtLoad = 1, PtDynamic = 2, PtInterp = 3, PtNote = 4, PtTls = 7;
        constexpr uint32_t ShtSymtab = 2, ShtRela = 4, ShtRel = 9;
        constexpr uint32_t ShfWrite = 0x1, ShfAlloc = 0x2;
        constexpr uint16_t ShnUndef = 0, ShnAbs = 0xfff1;
        constexpr uint32_t EfFloatAbiMask = 0x6, EfFloatAbiSingle = 0x2, EfRve = 0x8;

        constexpr uint32_t MaxPhdrs = 16;
        constexpr uint32_t MaxShdrs = 1024;
        constexpr uint32_t MaxNoteBytes = 512;
        // A symbol table up to this size is read into memory once (one
        // sequential read), instead of a scattered read per relocation.
        constexpr uint32_t MaxCachedSymtabBytes = 512 * 1024;
        // The same for a relocation section.
        constexpr uint32_t MaxCachedRelaBytes = 1024 * 1024;

        // What a relocation needs at load time.
        enum class Action : uint8_t {
                Nothing, // PC-relative, thread-local, label differences, hints
                Word,    // R_RISCV_32: add the base
                Upper,   // R_RISCV_HI20: add the base to the upper immediate
                PcUpper, // R_RISCV_PCREL_HI20: nothing, unless its symbol is absolute
                Refuse,
        };

        Action actionFor(uint32_t type) {
                switch (type) {
                        case 1: return Action::Word;     // R_RISCV_32
                        case 26: return Action::Upper;   // R_RISCV_HI20
                        case 23: return Action::PcUpper; // R_RISCV_PCREL_HI20
                        case 0:                          // NONE
                        case 16:                         // BRANCH
                        case 17:                         // JAL
                        case 18:                         // CALL
                        case 19:                         // CALL_PLT
                        case 24:                         // PCREL_LO12_I
                        case 25:                         // PCREL_LO12_S
                        case 27:                         // LO12_I (the base's low 12 bits are zero)
                        case 28:                         // LO12_S
                        case 29:                         // TPREL_HI20
                        case 30:                         // TPREL_LO12_I
                        case 31:                         // TPREL_LO12_S
                        case 32:                         // TPREL_ADD
                        case 33:                         // ADD8
                        case 34:                         // ADD16
                        case 35:                         // ADD32
                        case 36:                         // ADD64
                        case 37:                         // SUB8
                        case 38:                         // SUB16
                        case 39:                         // SUB32
                        case 40:                         // SUB64
                        case 43:                         // ALIGN
                        case 44:                         // RVC_BRANCH
                        case 45:                         // RVC_JUMP
                        case 51:                         // RELAX
                        case 52:                         // SUB6
                        case 53:                         // SET6
                        case 54:                         // SET8
                        case 55:                         // SET16
                        case 56:                         // SET32
                        case 57:                         // 32_PCREL
                        case 60:                         // SET_ULEB128
                        case 61:                         // SUB_ULEB128
                                return Action::Nothing;
                        default: return Action::Refuse;
                }
        }

        Error fail(Problem &problem, Error err, const char *what) {
                problem.what = what;
                problem.hasValue = false;
                return err;
        }

        Error fail(Problem &problem, Error err, const char *what, uint32_t value) {
                problem.what = what;
                problem.value = value;
                problem.hasValue = true;
                return err;
        }

        // True if [offset, offset + size) lies within [0, limit).
        bool within(uint64_t offset, uint64_t size, uint64_t limit) {
                return offset <= limit && size <= limit - offset;
        }

        template <typename T> Error readStruct(ProgramImage::Source &source, uint32_t offset, T &out) {
                return source.readAt(offset, WritableByteSpan(reinterpret_cast<uint8_t *>(&out), sizeof(T)));
        }

        // Reads and checks the ELF header and program headers; finds the
        // segment and the note.
        Error readHeaders(ProgramImage::Source &source, uint32_t fileSize, Ehdr &eh, ProgramImage::Info &info,
                          Problem &problem) {
                info = ProgramImage::Info();
                if (fileSize < sizeof(Ehdr)) return fail(problem, Error::NotSupported, "not an ELF file (too short)");
                Error err = readStruct(source, 0, eh);
                if (err.isError()) return err;
                if (std::memcmp(eh.ident,
                                "\x7f"
                                "ELF",
                                4) != 0)
                        return fail(problem, Error::NotSupported, "not an ELF file");
                if (eh.ident[4] != 1 || eh.ident[5] != 1 || eh.ident[6] != 1)
                        return fail(problem, Error::NotSupported, "not a 32-bit little-endian ELF file");
                if (eh.machine != EmRiscv) return fail(problem, Error::NotSupported, "not a RISC-V program");
                if (eh.type != EtExec)
                        return fail(problem, Error::NotSupported, "not a BMDS program (ELF type is not EXEC)", eh.type);
                if ((eh.flags & EfFloatAbiMask) != EfFloatAbiSingle || (eh.flags & EfRve) != 0)
                        return fail(problem, Error::NotSupported, "wrong ABI (needs ilp32f)", eh.flags);
                if (eh.phentsize != sizeof(Phdr) || eh.phnum == 0 || eh.phnum > MaxPhdrs ||
                    !within(eh.phoff, uint64_t(eh.phnum) * sizeof(Phdr), fileSize))
                        return fail(problem, Error::Invalid, "bad program headers");
                bool haveLoad = false;
                bool haveNote = false;
                Phdr load{};
                for (uint32_t i = 0; i < eh.phnum; ++i) {
                        Phdr ph;
                        err = readStruct(source, eh.phoff + i * sizeof(Phdr), ph);
                        if (err.isError()) return err;
                        switch (ph.type) {
                                case PtLoad:
                                        if (haveLoad)
                                                return fail(problem, Error::NotSupported,
                                                            "more than one PT_LOAD segment");
                                        haveLoad = true;
                                        load = ph;
                                        break;
                                case PtInterp: return fail(problem, Error::NotSupported, "needs a dynamic linker");
                                case PtDynamic:
                                        return fail(problem, Error::NotSupported,
                                                    "dynamically linked (a PIE or shared object)");
                                case PtTls:
                                        info.tlsOffset = ph.vaddr;
                                        info.tlsSize = ph.memsz;
                                        break;
                                case PtNote: {
                                        if (ph.filesz > MaxNoteBytes || !within(ph.offset, ph.filesz, fileSize))
                                                return fail(problem, Error::Invalid, "bad note segment");
                                        uint8_t notes[MaxNoteBytes];
                                        err = source.readAt(ph.offset, WritableByteSpan(notes, ph.filesz));
                                        if (err.isError()) return err;
                                        // Elf32_Nhdr: namesz, descsz, type; name and desc padded to 4.
                                        for (uint32_t at = 0; at + 12 <= ph.filesz;) {
                                                uint32_t namesz, descsz, type;
                                                std::memcpy(&namesz, notes + at, 4);
                                                std::memcpy(&descsz, notes + at + 4, 4);
                                                std::memcpy(&type, notes + at + 8, 4);
                                                const uint64_t name = at + 12;
                                                const uint64_t desc = name + ((uint64_t(namesz) + 3) & ~3ull);
                                                const uint64_t next = desc + ((uint64_t(descsz) + 3) & ~3ull);
                                                if (next > ph.filesz) return fail(problem, Error::Invalid, "bad note");
                                                if (namesz == 5 && std::memcmp(notes + name, BMDS_NOTE_NAME, 5) == 0 &&
                                                    type == BMDS_NOTE_PROGRAM) {
                                                        bmds_program_note_t note{};
                                                        std::memcpy(&note, notes + desc,
                                                                    descsz < sizeof(note) ? descsz : sizeof(note));
                                                        if (descsz < 8 || note.size < 8 || note.size > descsz)
                                                                return fail(problem, Error::Invalid, "bad BMDS note");
                                                        // Fields past what the program knew about are zero.
                                                        if (note.size < sizeof(note))
                                                                std::memset(reinterpret_cast<uint8_t *>(&note) +
                                                                                    note.size,
                                                                            0, sizeof(note) - note.size);
                                                        info.abiVersion = note.abi_version;
                                                        info.stackSize = note.stack_size;
                                                        if (note.abi_version == 0)
                                                                return fail(problem, Error::Invalid, "bad ABI version",
                                                                            0);
                                                        if (note.abi_version > BMDS_ABI_VERSION)
                                                                return fail(problem, Error::NotSupported,
                                                                            "built for a newer BMDS (ABI version)",
                                                                            note.abi_version);
                                                        if (note.stack_size > BMDS_MAX_STACK_SIZE)
                                                                return fail(problem, Error::NotSupported,
                                                                            "asks for too much stack", note.stack_size);
                                                        if ((note.flags & ~BMDS_PROGRAM_DISPLAY) != 0)
                                                                return fail(problem, Error::NotSupported,
                                                                            "unknown program flags", note.flags);
                                                        info.flags = note.flags;
                                                        haveNote = true;
                                                }
                                                at = static_cast<uint32_t>(next);
                                        }
                                        break;
                                }
                                default: break; // PT_GNU_STACK, PT_RISCV_ATTRIBUTES, PT_PHDR...
                        }
                }
                if (!haveNote) return fail(problem, Error::NotSupported, "not a BMDS program (no BMDS note)");
                if (!haveLoad) return fail(problem, Error::Invalid, "no PT_LOAD segment");
                if (load.vaddr != 0) return fail(problem, Error::NotSupported, "not linked at address 0", load.vaddr);
                if (load.filesz > load.memsz || !within(load.offset, load.filesz, fileSize))
                        return fail(problem, Error::Invalid, "bad PT_LOAD segment");
                if (load.memsz == 0 || load.memsz > ProgramImage::MaxImageBytes)
                        return fail(problem, Error::NotSupported, "image too big", load.memsz);
                if (eh.entry >= load.filesz)
                        return fail(problem, Error::Invalid, "entry point outside the image", eh.entry);
                if (info.tlsSize != 0 && !within(info.tlsOffset, info.tlsSize, load.memsz))
                        return fail(problem, Error::Invalid, "thread-local storage outside the image");
                info.entry = eh.entry;
                info.fileOffset = load.offset;
                info.fileSize = load.filesz;
                info.imageSize = load.memsz;
                return Error::Ok;
        }

        // Adds @p base to the word or upper immediate @p word.
        uint32_t patched(uint32_t word, uint32_t base, bool upper) {
                // lui/auipc: imm[31:12] += base >> 12 (base is 4096-aligned).
                if (upper) return (word & 0xfffu) | (((word & 0xfffff000u) + base) & 0xfffff000u);
                return word + base;
        }

        // Walks the relocations of the allocated sections: checks each one,
        // counts the patches and, given an image, applies them.
        class Relocator {
                public:
                        Relocator(ProgramImage::Source &source, uint32_t fileSize, const Ehdr &eh,
                                  const ProgramImage::Info &info, Problem &problem)
                            : _source(source), _fileSize(fileSize), _eh(eh), _info(info), _problem(problem) {}
                        ~Relocator() {
                                Memory::free(_symbols);
                                Memory::free(_headers);
                        }
                        Relocator(const Relocator &) = delete;
                        Relocator &operator=(const Relocator &) = delete;

                        // @p image null: check and count only. @p record: the patches
                        // are written there, not applied (@p base is then 0). @p classify:
                        // its writableOffset and patchedPages filled in (inspect()).
                        Error run(uint8_t *image, uint32_t base, uint32_t &patches,
                                  ProgramImage::Patch *record = nullptr, ProgramImage::Info *classify = nullptr) {
                                patches = 0;
                                _image = image;
                                _base = base;
                                _record = record;
                                _classify = classify;
                                if (_eh.shentsize != sizeof(Shdr) || _eh.shnum == 0 || _eh.shnum > MaxShdrs ||
                                    !within(_eh.shoff, uint64_t(_eh.shnum) * sizeof(Shdr), _fileSize))
                                        return fail(_problem, Error::NotSupported,
                                                    "no section headers (stripped? relocations are needed)");
                                // All the section headers in one read (each is consulted again).
                                const size_t headerBytes = size_t(_eh.shnum) * sizeof(Shdr);
                                _headers = static_cast<Shdr *>(Memory::alloc(MemSpace::Psram, headerBytes));
                                if (_headers != nullptr) {
                                        Error err = _source.readAt(
                                                _eh.shoff,
                                                WritableByteSpan(reinterpret_cast<uint8_t *>(_headers), headerBytes));
                                        if (err.isError()) return err;
                                }
                                bool any = false;
                                if (_classify != nullptr) {
                                        _classify->writableOffset = _info.imageSize;
                                        std::memset(_classify->patchedPages, 0, sizeof(_classify->patchedPages));
                                }
                                for (uint32_t i = 0; i < _eh.shnum; ++i) {
                                        Shdr  sh;
                                        Error err = section(i, sh);
                                        if (err.isError()) return err;
                                        if (_classify != nullptr && (sh.flags & ShfAlloc) != 0 &&
                                            (sh.flags & ShfWrite) != 0 && sh.size != 0 &&
                                            sh.addr < _classify->writableOffset)
                                                _classify->writableOffset = sh.addr;
                                        if (sh.type != ShtRela && sh.type != ShtRel) continue;
                                        Shdr target;
                                        if (sh.info >= _eh.shnum)
                                                return fail(_problem, Error::Invalid, "bad relocation section");
                                        err = section(sh.info, target);
                                        if (err.isError()) return err;
                                        if ((target.flags & ShfAlloc) == 0) continue; // debug information
                                        if (sh.type == ShtRel)
                                                return fail(_problem, Error::NotSupported,
                                                            "REL relocations (only RELA)");
                                        err = relocateSection(sh, target, patches);
                                        if (err.isError()) return err;
                                        any = true;
                                }
                                // A program with code always has relocations (calls, branches);
                                // none means they weren't kept.
                                if (!any)
                                        return fail(_problem, Error::NotSupported,
                                                    "no relocations (link with --emit-relocs; don't strip)");
                                return Error::Ok;
                        }

                private:
                        Error section(uint32_t index, Shdr &out) {
                                if (_headers != nullptr) {
                                        out = _headers[index];
                                        return Error::Ok;
                                }
                                return readStruct(_source, _eh.shoff + index * sizeof(Shdr), out);
                        }

                        Error relocateSection(const Shdr &rela, const Shdr &target, uint32_t &patches) {
                                if (rela.entsize != sizeof(Rela) || rela.size % sizeof(Rela) != 0 ||
                                    !within(rela.offset, rela.size, _fileSize))
                                        return fail(_problem, Error::Invalid, "bad relocation section");
                                if (!within(target.addr, target.size, _info.imageSize))
                                        return fail(_problem, Error::Invalid, "relocated section outside the image");
                                if (rela.link != _symtabIndex) {
                                        Error err = useSymbolTable(rela.link);
                                        if (err.isError()) return err;
                                }
                                // The whole section in one sequential read when there is memory
                                // for it (small reads from a card cost a command each); else a
                                // chunk at a time.
                                Rela          *rels = nullptr;
                                uint32_t       step = 64;
                                Rela           chunk[64];
                                const uint32_t count = rela.size / sizeof(Rela);
                                if (rela.size <= MaxCachedRelaBytes)
                                        rels = static_cast<Rela *>(Memory::alloc(MemSpace::Psram, rela.size));
                                if (rels != nullptr) step = count;
                                struct Freer {
                                                void *p;
                                                ~Freer() { Memory::free(p); }
                                } freer{rels};
                                Rela *const buffer = rels != nullptr ? rels : chunk;
                                for (uint32_t done = 0; done < count;) {
                                        const uint32_t n = count - done < step ? count - done : step;
                                        Error          err = _source.readAt(rela.offset + done * sizeof(Rela),
                                                                            WritableByteSpan(reinterpret_cast<uint8_t *>(buffer),
                                                                                             n * sizeof(Rela)));
                                        if (err.isError()) return err;
                                        for (uint32_t k = 0; k < n; ++k) {
                                                err = relocation(buffer[k], target, patches);
                                                if (err.isError()) return err;
                                        }
                                        done += n;
                                }
                                return Error::Ok;
                        }

                        // Makes section @p index the symbol table lookups use, reading it
                        // into memory if it is small enough.
                        Error useSymbolTable(uint32_t index) {
                                _symtab = Shdr{};
                                _symtabIndex = index;
                                Memory::free(_symbols);
                                _symbols = nullptr;
                                if (index == 0) return Error::Ok;
                                if (index >= _eh.shnum) return fail(_problem, Error::Invalid, "bad symbol table");
                                Error err = section(index, _symtab);
                                if (err.isError()) return err;
                                if (_symtab.type != ShtSymtab || _symtab.entsize != sizeof(Sym) ||
                                    !within(_symtab.offset, _symtab.size, _fileSize))
                                        return fail(_problem, Error::Invalid, "bad symbol table");
                                if (_symtab.size > MaxCachedSymtabBytes) return Error::Ok;
                                // Without the memory, symbols are read one at a time.
                                _symbols = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, _symtab.size));
                                if (_symbols == nullptr) return Error::Ok;
                                return _source.readAt(_symtab.offset, WritableByteSpan(_symbols, _symtab.size));
                        }

                        // True if relocation @p r's symbol is absolute or undefined: its
                        // value doesn't move with the image.
                        Error isFixed(const Rela &r, bool &fixed) {
                                const uint32_t index = r.info >> 8;
                                if (index == 0 || _symtab.size == 0)
                                        return fail(_problem, Error::Invalid, "relocation without a symbol", r.offset);
                                if (index >= _symtab.size / sizeof(Sym))
                                        return fail(_problem, Error::Invalid, "bad symbol index", index);
                                Sym sym;
                                if (_symbols != nullptr) {
                                        std::memcpy(&sym, _symbols + index * sizeof(Sym), sizeof(Sym));
                                } else {
                                        Error err = readStruct(_source, _symtab.offset + index * sizeof(Sym), sym);
                                        if (err.isError()) return err;
                                }
                                fixed = sym.shndx == ShnAbs || sym.shndx == ShnUndef;
                                return Error::Ok;
                        }

                        Error relocation(const Rela &r, const Shdr &target, uint32_t &patches) {
                                const uint32_t type = r.info & 0xff;
                                const Action   action = actionFor(type);
                                if (action == Action::Refuse)
                                        return fail(_problem, Error::NotSupported, "unsupported relocation type", type);
                                if (action == Action::Nothing) return Error::Ok;
                                // The patched word must be in the file part of the target section.
                                if (r.offset < target.addr ||
                                    !within(r.offset, 4, uint64_t(target.addr) + target.size) ||
                                    !within(r.offset, 4, _info.fileSize))
                                        return fail(_problem, Error::Invalid, "relocation outside its section",
                                                    r.offset);
                                bool  fixed = false;
                                Error err = isFixed(r, fixed);
                                if (err.isError()) return err;
                                uint32_t word = 0;
                                if (_image != nullptr) {
                                        std::memcpy(&word, _image + r.offset, 4);
                                } else if (action == Action::PcUpper && fixed) {
                                        err = _source.readAt(_info.fileOffset + r.offset,
                                                             WritableByteSpan(reinterpret_cast<uint8_t *>(&word), 4));
                                        if (err.isError()) return err;
                                }
                                if (action == Action::PcUpper) {
                                        // An absolute target reached PC-relatively would move; the
                                        // linker turns the auipc into a lui when it can (weak
                                        // symbols at 0), which doesn't.
                                        if (fixed && (word & 0x7f) != 0x37)
                                                return fail(_problem, Error::NotSupported,
                                                            "PC-relative reference to an absolute symbol", r.offset);
                                        return Error::Ok;
                                }
                                if (fixed) return Error::Ok;
                                if (_record != nullptr) {
                                        // inspect() counted them: a file that has more changed.
                                        if (patches == _info.relocations)
                                                return fail(_problem, Error::Invalid, "the file changed while loading");
                                        _record[patches] =
                                                r.offset | (action == Action::Upper ? ProgramImage::PatchUpper : 0);
                                }
                                if (_classify != nullptr) {
                                        // Both pages of a word that straddles two (RVC: 2-aligned).
                                        for (const uint32_t at : {r.offset, r.offset + 3}) {
                                                const uint32_t page = at / ProgramImage::SharePageBytes;
                                                _classify->patchedPages[page / 32] |= 1u << (page % 32);
                                        }
                                }
                                ++patches;
                                if (_image == nullptr || _record != nullptr) return Error::Ok;
                                word = patched(word, _base, action == Action::Upper);
                                std::memcpy(_image + r.offset, &word, 4);
                                return Error::Ok;
                        }

                        ProgramImage::Source     &_source;
                        uint32_t                  _fileSize;
                        const Ehdr               &_eh;
                        const ProgramImage::Info &_info;
                        Problem                  &_problem;
                        Shdr                      _symtab{};
                        uint32_t                  _symtabIndex = 0;
                        uint8_t                  *_symbols = nullptr; // the cached symbol table, if any
                        Shdr                     *_headers = nullptr; // the section headers, if there was memory
                        uint8_t                  *_image = nullptr;
                        uint32_t                  _base = 0;
                        ProgramImage::Patch      *_record = nullptr;
                        ProgramImage::Info       *_classify = nullptr;
        };

        // Rereads the headers of a program inspected as @p expected (the file
        // may have changed since).
        Error recheck(ProgramImage::Source &source, uint32_t fileSize, const ProgramImage::Info &expected, Ehdr &eh,
                      ProgramImage::Info &info, Problem &problem) {
                Error err = readHeaders(source, fileSize, eh, info, problem);
                if (err.isError()) return err;
                if (info.fileOffset != expected.fileOffset || info.fileSize != expected.fileSize ||
                    info.imageSize != expected.imageSize || info.entry != expected.entry)
                        return fail(problem, Error::Invalid, "the file changed while loading");
                return Error::Ok;
        }


} // namespace

Error ProgramImage::MemorySource::readAt(uint32_t offset, WritableByteSpan out) {
        if (!within(offset, out.size(), _bytes.size())) return Error::EndOfFile;
        std::memcpy(out.data(), _bytes.data() + offset, out.size());
        return Error::Ok;
}

Error ProgramImage::inspect(Source &source, uint32_t fileSize, Info &info, Problem &problem) {
        problem = Problem();
        Ehdr  eh;
        Error err = readHeaders(source, fileSize, eh, info, problem);
        if (err.isError()) return err;
        Relocator relocator(source, fileSize, eh, info, problem);
        uint32_t  patches = 0;
        err = relocator.run(nullptr, 0, patches, nullptr, &info);
        info.relocations = patches;
        return err;
}

bool ProgramImage::isSharedPage(const Info &info, uint32_t page) {
        if (page >= pageCount(info.imageSize)) return false;
        const uint64_t end = uint64_t(page + 1) * SharePageBytes;
        if (end > info.writableOffset || end > info.fileSize) return false;
        return (info.patchedPages[page / 32] & (1u << (page % 32))) == 0;
}

uint32_t ProgramImage::sharedPageCount(const Info &info) {
        uint32_t n = 0;
        for (uint32_t p = 0; p < pageCount(info.imageSize); ++p) n += isSharedPage(info, p) ? 1 : 0;
        return n;
}

Error ProgramImage::load(Source &source, uint32_t fileSize, const Info &expected, uint8_t *image, uint32_t base,
                         Problem &problem) {
        problem = Problem();
        if ((base & (BMDS_LOAD_ALIGN - 1)) != 0)
                return fail(problem, Error::Invalid, "load address not 4096-aligned", base);
        // The file may have changed since it was inspected: check it again.
        Ehdr  eh;
        Info  info;
        Error err = recheck(source, fileSize, expected, eh, info, problem);
        if (err.isError()) return err;
        err = source.readAt(info.fileOffset, WritableByteSpan(image, info.fileSize));
        if (err.isError()) return err;
        std::memset(image + info.fileSize, 0, info.imageSize - info.fileSize);
        Relocator relocator(source, fileSize, eh, info, problem);
        uint32_t  patches = 0;
        err = relocator.run(image, base, patches);
        if (err.isOk() && patches != expected.relocations)
                return fail(problem, Error::Invalid, "the file changed while loading");
        return err;
}

Error ProgramImage::read(Source &source, uint32_t fileSize, const Info &expected, uint8_t *bytes, Patch *patches,
                         Problem &problem) {
        problem = Problem();
        Ehdr  eh;
        Info  info;
        Error err = recheck(source, fileSize, expected, eh, info, problem);
        if (err.isError()) return err;
        err = source.readAt(info.fileOffset, WritableByteSpan(bytes, info.fileSize));
        if (err.isError()) return err;
        // load()'s walk at base 0 (no change), writing the patches down.
        info.relocations = expected.relocations;
        Relocator relocator(source, fileSize, eh, info, problem);
        uint32_t  count = 0;
        err = relocator.run(bytes, 0, count, patches);
        if (err.isOk() && count != expected.relocations)
                return fail(problem, Error::Invalid, "the file changed while loading");
        return err;
}

void ProgramImage::place(const Info &info, const uint8_t *bytes, const Patch *patches, uint8_t *image, uint32_t base) {
        std::memcpy(image, bytes, info.fileSize);
        std::memset(image + info.fileSize, 0, info.imageSize - info.fileSize);
        for (uint32_t i = 0; i < info.relocations; ++i) {
                const uint32_t at = patches[i] & ~PatchUpper;
                uint32_t       word;
                std::memcpy(&word, image + at, 4);
                word = patched(word, base, (patches[i] & PatchUpper) != 0);
                std::memcpy(image + at, &word, 4);
        }
        return;
}

void ProgramImage::placePages(const Info &info, const uint8_t *bytes, const Patch *patches, uint8_t *const *pages,
                              uint32_t base) {
        const uint32_t count = pageCount(info.imageSize);
        for (uint32_t p = 0; p < count; ++p) {
                uint8_t *page = pages[p];
                if (page == nullptr) continue;
                const uint32_t start = p * SharePageBytes;
                const uint32_t fromFile = info.fileSize > start ? std::min(info.fileSize - start, SharePageBytes) : 0;
                std::memcpy(page, bytes + start, fromFile);
                std::memset(page + fromFile, 0, SharePageBytes - fromFile); // the bss, and past the image
        }
        // A word may straddle two pages: its bytes are read and written one by one there.
        auto at = [&](uint32_t offset) -> uint8_t & {
                return pages[offset / SharePageBytes][offset % SharePageBytes];
        };
        for (uint32_t i = 0; i < info.relocations; ++i) {
                const uint32_t offset = patches[i] & ~PatchUpper;
                if (pages[offset / SharePageBytes] == nullptr || pages[(offset + 3) / SharePageBytes] == nullptr)
                        continue; // shared: no patch there (isSharedPage())
                uint32_t word = 0;
                for (uint32_t k = 0; k < 4; ++k) word |= uint32_t(at(offset + k)) << (8 * k);
                word = patched(word, base, (patches[i] & PatchUpper) != 0);
                for (uint32_t k = 0; k < 4; ++k) at(offset + k) = uint8_t(word >> (8 * k));
        }
        return;
}

Error ProgramImage::findIcon(Source &source, uint32_t fileSize, uint16_t side, IconRef &out) {
        constexpr uint32_t ShtNote = 7, ShfAlloc = 2, ShdrBytes = 40, MaxSections = 256;
        const auto         u16 = [](const uint8_t *p) {
                return uint16_t(p[0] | p[1] << 8);
        };
        const auto u32 = [](const uint8_t *p) {
                return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
        };
        const auto within = [fileSize](uint32_t offset, uint32_t size) {
                return offset <= fileSize && size <= fileSize - offset;
        };
        uint8_t ehdr[52];
        if (fileSize < sizeof(ehdr)) return Error::NotFound;
        Error err = source.readAt(0, WritableByteSpan(ehdr, sizeof(ehdr)));
        if (err.isError()) return err;
        if (std::memcmp(ehdr,
                        "\x7f"
                        "ELF",
                        4) != 0 ||
            ehdr[4] != 1 || ehdr[5] != 1)
                return Error::NotFound;
        const uint32_t shoff = u32(ehdr + 32);
        const uint16_t shentsize = u16(ehdr + 46);
        const uint16_t shnum = u16(ehdr + 48);
        if (shentsize < ShdrBytes || shnum > MaxSections || !within(shoff, uint32_t(shnum) * shentsize))
                return Error::NotFound;
        IconRef best;
        bool    found = false;
        // Better for @p side: the same size, else the smallest bigger one, else the biggest.
        const auto better = [side](const IconRef &a, const IconRef &b) {
                if (a.width == side) return b.width != side;
                if (b.width == side) return false;
                if ((a.width > side) != (b.width > side)) return a.width > side;
                return a.width > side ? a.width < b.width : a.width > b.width;
        };
        for (uint16_t i = 0; i < shnum; ++i) {
                uint8_t sh[ShdrBytes];
                err = source.readAt(shoff + uint32_t(i) * shentsize, WritableByteSpan(sh, sizeof(sh)));
                if (err.isError()) return err;
                if (u32(sh + 4) != ShtNote || (u32(sh + 8) & ShfAlloc) != 0) continue;
                const uint32_t start = u32(sh + 16);
                const uint32_t size = u32(sh + 20);
                if (!within(start, size)) continue;
                // Its notes: Elf32_Nhdr, the name and the descriptor, each padded to 4.
                for (uint32_t at = 0; size - at >= 12;) {
                        uint8_t nh[12 + 8];
                        err = source.readAt(start + at, WritableByteSpan(nh, 12));
                        if (err.isError()) return err;
                        const uint32_t namesz = u32(nh);
                        const uint32_t descsz = u32(nh + 4);
                        const uint32_t type = u32(nh + 8);
                        const uint32_t desc = at + 12 + ((namesz + 3) & ~3u);
                        if (namesz > 64 || descsz > size || desc > size || descsz > size - desc) break;
                        bool ours = false;
                        if (namesz == 5) {
                                err = source.readAt(start + at + 12, WritableByteSpan(nh + 12, 5));
                                if (err.isError()) return err;
                                ours = std::memcmp(nh + 12, BMDS_NOTE_NAME, 5) == 0;
                        }
                        if (ours && type == BMDS_NOTE_ICON && descsz >= 16) {
                                uint8_t d[16];
                                err = source.readAt(start + desc, WritableByteSpan(d, sizeof(d)));
                                if (err.isError()) return err;
                                IconRef        r;
                                const uint32_t header = u32(d);
                                r.width = u16(d + 4);
                                r.height = u16(d + 6);
                                r.format = u32(d + 8);
                                r.length = u32(d + 12);
                                r.offset = start + desc + header;
                                if (header >= 16 && header <= descsz && r.length <= descsz - header &&
                                    r.format == BMDS_ICON_FORMAT_PNG && r.width != 0 && r.height != 0 &&
                                    (!found || better(r, best))) {
                                        best = r;
                                        found = true;
                                }
                        }
                        at = desc + ((descsz + 3) & ~3u);
                }
        }
        if (!found) return Error::NotFound;
        out = best;
        return Error();
}

BMDS_NAMESPACE_END
