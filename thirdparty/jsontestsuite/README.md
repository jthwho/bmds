# JSONTestSuite (vendored, tests only)

Nicolas Seriot's JSON parsing tests, <https://github.com/nst/JSONTestSuite>, used by the host
tests of `bmds_core`'s `JsonReader` (`tests/host/json.cpp`, `devplan/json.md`). License:
`LICENSE` (MIT).

- **Version:** master, commit `1ef36fa01286573e846ac449e8683f8833c5b26a` (2024-11-22).
- **Vendored:** `test_parsing/` unchanged: `y_` documents a parser must accept, `n_` ones it must
  reject, `i_` ones where either is allowed (the tests record what `JsonReader` does with each).
- **Not vendored:** `test_transform/`, the parsers, the results and the article.

To update: copy `test_parsing/` and `LICENSE` from the new commit and run the host tests.
