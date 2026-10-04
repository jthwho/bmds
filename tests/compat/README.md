# Compatibility corpus

Programs built with each SDK version, kept **exactly as built**, and what
they must do (`docs/ABI.md`: a new kernel runs old programs unchanged).

- `v1/`: SDK v1 (ABI 1): `hello.elf`, `cat.elf` (the raw system calls) and
  `progtest.elf` (picolibc over every slot), with `cases.txt`.
- **Never rebuild, replace or edit** a program or an expectation here to make
  a test pass: a failure means the kernel broke userspace. A new SDK version
  adds a new directory (`v2/`, ...) built with it.
- The host tests check that every program still passes the loader's format
  check and relocates (`tests/host/loader.cpp`).
- On a board, `tools/bmds-compat` sends each corpus to the RAM disk and runs
  its cases (about 5 s for v1):

  ```
  . ~/esp/esp-idf/export.sh    # pyserial
  tools/bmds-compat            # every tests/compat/v*; -p PORT, -u to print results
  ```

`cases.txt`: per case a line `=== command` (shell syntax, run in the
corpus directory on the board), the expected console output (stdout and
stderr together), then `--- status`.
