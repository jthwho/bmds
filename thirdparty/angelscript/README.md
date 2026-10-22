# AngelScript (vendored)

Andreas Jonsson's scripting library, the language of `angel` (`programs/angel`,
`devplan/scripting.md`). License: zlib (`LICENSE`, and each file's header).

- **Source:** <https://www.angelcode.com/angelscript/>, release 2.38.0,
  `angelscript_2.38.0.zip` (sha256
  `b33b5dbcda10317ef67d628353d83246984ce6fcac102d4dc2aed121eba52e6f`): its `angelscript/include`
  as `include/`, `angelscript/source` as `source/`, and the add-on `add_on/scriptarray`. Line
  endings are kept as released (CRLF).
- **Changes** (zlib's clause 2: altered versions are marked; every change is commented
  `BMDS:`). Patches in `patches/`, applied with `patch -p1` from this directory in this order,
  reproduce this tree from the release:
  1. `bmds-float-literals.diff`: the define `AS_FLOAT_LITERALS`. Decimal literals without a
     suffix are `float`, a `d` suffix makes a `double`, and an integer given to overloads for
     both goes to `float` (the P4's FPU is single precision; `double` costs 4.5 times as much).
  2. `bmds-top-level-statements.diff`: the define `AS_TOP_LEVEL_STATEMENTS`. The first section
     of a module may have statements outside functions; they are compiled, in order, into the
     global function `$top` (`AS_TOP_LEVEL_FUNCTION`), and a non-const global declared among
     them with an initializer is initialized where it stands, when `$top` reaches it.
  3. `bmds-scriptarray.diff`: the add-on without the STL (strings with `snprintf`, a heapsort
     in place of `std::sort` and the quadratic sorts).
  4. `bmds-foreach-items.diff`: a fix. `foreach`'s item variables get a scope of their own
     inside the loop's, as a `for` body's variables have. Released, they lived in the loop's
     scope: destroyed after each time round and then again at the loop's end (a value type such
     as `string` freed twice), and not destroyed by `continue` (constructed over, leaked).
     Found by the guide's lessons (`devplan/scripting.md` §14).

  Without the two defines, the library behaves exactly as released. Both are written to be
  offered upstream.
- **Built** for BMDS with `AS_NO_EXCEPTIONS` (programs have no exceptions); on RV32 the library
  chooses `AS_MAX_PORTABILITY` itself (no native calling convention), so everything registered
  uses `asCALL_GENERIC`. The host tests build it with `AS_MAX_PORTABILITY` as well, so both run
  the same code.
