/**
 * @file      shacrypt.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * SHA-512 crypt, as its specification describes it
 * (https://www.akkadia.org/drepper/SHA-crypt.txt).
 */

#include <cstring>
#include <bmds/sha512.h>
#include <bmds/shacrypt.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr StringView Prefix("$6$");
        constexpr StringView RoundsPrefix("rounds=");
        constexpr char       Chars[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
        constexpr size_t     Digest = Sha512::DigestBytes;

        bool isSaltChar(char c) {
                return c == '.' || c == '/' || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                       (c >= 'a' && c <= 'z');
        }

        // The hash's characters: bytes of the digest taken three at a time, in the
        // specification's order (each triple is low, middle, high), 4 characters each.
        constexpr uint8_t Order[22][3] = {
                {0, 21, 42},  {22, 43, 1},  {44, 2, 23},  {3, 24, 45},  {25, 46, 4},  {47, 5, 26},
                {6, 27, 48},  {28, 49, 7},  {50, 8, 29},  {9, 30, 51},  {31, 52, 10}, {53, 11, 32},
                {12, 33, 54}, {34, 55, 13}, {56, 14, 35}, {15, 36, 57}, {37, 58, 16}, {59, 17, 38},
                {18, 39, 60}, {40, 61, 19}, {62, 20, 41}, {0, 0, 63},
        };

        Error encode(const uint8_t (&c)[Digest], String &out) {
                for (size_t i = 0; i < 22; ++i) {
                        // The last group is one byte: 2 characters.
                        uint32_t v =
                                i < 21 ? uint32_t(c[Order[i][0]]) << 16 | uint32_t(c[Order[i][1]]) << 8 | c[Order[i][2]]
                                       : c[Order[i][2]];
                        for (int n = i < 21 ? 4 : 2; n > 0; --n) {
                                const Error err = out.append(Chars[v & 0x3f]);
                                if (err.isError()) return err;
                                v >>= 6;
                        }
                }
                return Error::Ok;
        }

        // @p length bytes of @p sequence's digest repeated (the P and S sequences).
        void addRepeated(Sha512 &h, const uint8_t (&digest)[Digest], size_t length) {
                for (; length > Digest; length -= Digest) h.update(digest, Digest);
                h.update(digest, length);
                return;
        }

        void wipe(void *p, size_t n) {
                volatile uint8_t *v = static_cast<volatile uint8_t *>(p);
                while (n-- > 0) *v++ = 0;
                return;
        }

} // namespace

bool ShaCrypt::parse(StringView text, Setting &out) {
        out = Setting{};
        if (!text.startsWith(Prefix)) return false;
        StringView rest = text.mid(Prefix.size());
        if (rest.startsWith(RoundsPrefix)) {
                const size_t end = rest.indexOf('$');
                if (end == StringView::NPos) return false;
                const StringView digits = rest.mid(RoundsPrefix.size(), end - RoundsPrefix.size());
                if (digits.isEmpty() || digits.size() > 10) return false;
                uint64_t rounds = 0;
                for (char c : digits) {
                        if (c < '0' || c > '9') return false;
                        rounds = rounds * 10 + static_cast<uint64_t>(c - '0');
                }
                out.rounds = rounds < MinRounds ? MinRounds : rounds > MaxRounds ? MaxRounds : uint32_t(rounds);
                out.roundsGiven = true;
                rest = rest.mid(end + 1);
        }
        size_t saltEnd = rest.indexOf('$');
        if (saltEnd == StringView::NPos) saltEnd = rest.size();
        const StringView salt = rest.left(saltEnd);
        for (char c : salt)
                if (!isSaltChar(c)) return false;
        out.salt = salt;
        if (saltEnd < rest.size()) out.hash = rest.mid(saltEnd + 1);
        return true;
}

bool ShaCrypt::isHash(StringView text) {
        Setting s;
        if (!parse(text, s) || s.salt.size() > MaxSalt || s.hash.size() != HashChars) return false;
        for (char c : s.hash)
                if (!isSaltChar(c)) return false;
        return true;
}

Error ShaCrypt::hash(StringView password, StringView salt, uint32_t rounds, String &out) {
        if (password.size() > MaxPassword) return Error::Invalid;
        for (char c : salt)
                if (!isSaltChar(c)) return Error::Invalid;
        if (salt.size() > MaxSalt) salt = salt.left(MaxSalt);
        rounds = rounds < MinRounds ? MinRounds : rounds > MaxRounds ? MaxRounds : rounds;
        const uint8_t *p = reinterpret_cast<const uint8_t *>(password.data());
        const uint8_t *s = reinterpret_cast<const uint8_t *>(salt.data());
        const size_t   pl = password.size();
        const size_t   sl = salt.size();

        // B: password, salt, password.
        uint8_t b[Digest];
        {
                Sha512 h;
                h.update(p, pl);
                h.update(s, sl);
                h.update(p, pl);
                h.final(b);
        }
        // A: password, salt, B for as many bytes as the password, then per bit of its length.
        uint8_t a[Digest];
        {
                Sha512 h;
                h.update(p, pl);
                h.update(s, sl);
                addRepeated(h, b, pl);
                for (size_t n = pl; n > 0; n >>= 1) {
                        if ((n & 1) != 0)
                                h.update(b, Digest);
                        else
                                h.update(p, pl);
                }
                h.final(a);
        }
        // The P sequence: the password repeated, hashed, as long as the password.
        uint8_t dp[Digest];
        {
                Sha512 h;
                for (size_t i = 0; i < pl; ++i) h.update(p, pl);
                h.final(dp);
        }
        // The S sequence: the salt 16 + A[0] times, hashed, as long as the salt.
        uint8_t ds[Digest];
        {
                Sha512 h;
                for (size_t i = 0; i < 16u + a[0]; ++i) h.update(s, sl);
                h.final(ds);
        }
        // The P and S sequences as bytes, for the rounds (the password is at most 256).
        uint8_t pseq[MaxPassword];
        for (size_t i = 0; i < pl; ++i) pseq[i] = dp[i % Digest];
        uint8_t sseq[MaxSalt];
        for (size_t i = 0; i < sl; ++i) sseq[i] = ds[i];
        uint8_t c[Digest];
        std::memcpy(c, a, Digest);
        for (uint32_t r = 0; r < rounds; ++r) {
                Sha512 h;
                if ((r & 1) != 0)
                        h.update(pseq, pl);
                else
                        h.update(c, Digest);
                if (r % 3 != 0) h.update(sseq, sl);
                if (r % 7 != 0) h.update(pseq, pl);
                if ((r & 1) != 0)
                        h.update(c, Digest);
                else
                        h.update(pseq, pl);
                h.final(c);
        }
        Error err = out.append(Prefix);
        if (err.isOk()) err = out.appendf("rounds=%u$", static_cast<unsigned>(rounds));
        if (err.isOk()) err = out.append(salt);
        if (err.isOk()) err = out.append('$');
        if (err.isOk()) err = encode(c, out);
        wipe(b, sizeof(b));
        wipe(a, sizeof(a));
        wipe(dp, sizeof(dp));
        wipe(pseq, sizeof(pseq));
        wipe(c, sizeof(c));
        return err;
}

bool ShaCrypt::verify(StringView password, StringView hash) {
        Setting s;
        if (!isHash(hash) || !parse(hash, s)) return false;
        String made(MemSpace::Psram);
        if (ShaCrypt::hash(password, s.salt, s.rounds, made).isError()) return false;
        // Without rounds= the hash means 5000; ours always says it: compare the hashes alone.
        const StringView mine = made.view().right(HashChars);
        uint8_t          diff = 0;
        for (size_t i = 0; i < HashChars; ++i) diff |= static_cast<uint8_t>(mine[i] ^ s.hash[i]);
        return diff == 0;
}

BMDS_NAMESPACE_END
