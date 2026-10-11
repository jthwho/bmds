/**
 * @file      shacrypt.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * SHA-512 and SHA-512 crypt (devplan/network.md §24): FIPS 180-4's vectors,
 * the crypt specification's, and OpenSSL's (`openssl passwd -6`).
 */

#include <doctest.h>

#include <string>
#include <bmds/sha512.h>
#include <bmds/shacrypt.h>

using bmds::Sha512;
namespace ShaCrypt = bmds::ShaCrypt;

namespace {

        std::string hex(const std::string &message, size_t pieces = 1) {
                Sha512       h;
                const size_t step = (message.size() + pieces - 1) / pieces;
                for (size_t at = 0; at < message.size(); at += step)
                        h.update(message.data() + at, std::min(step, message.size() - at));
                uint8_t out[Sha512::DigestBytes];
                h.final(out);
                std::string s;
                char        b[3];
                for (uint8_t v : out) {
                        std::snprintf(b, sizeof(b), "%02x", v);
                        s += b;
                }
                return s;
        }

        std::string crypt(const std::string &password, const std::string &salt, uint32_t rounds) {
                bmds::String out(bmds::MemSpace::Psram);
                REQUIRE(ShaCrypt::hash(bmds::StringView(password.data(), password.size()),
                                       bmds::StringView(salt.data(), salt.size()), rounds, out)
                                .isOk());
                return std::string(out.cStr());
        }

} // namespace

TEST_CASE("Sha512: the standard's vectors, in one piece and in many") {
        CHECK(hex("") == "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
                         "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");
        CHECK(hex("abc") == "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                            "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
        const std::string two =
                "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqr"
                "lmnopqrsmnopqrstnopqrstu";
        const char *const twoDigest = "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018"
                                      "501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909";
        CHECK(hex(two) == twoDigest);
        CHECK(hex(two, 7) == twoDigest);
        CHECK(hex(std::string(1000000, 'a'), 999) ==
              "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973eb"
              "de0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b");
}

TEST_CASE("ShaCrypt: hashes as the specification and OpenSSL make them") {
        // Ours always says its rounds; the hash part is the same as without.
        CHECK(crypt("Hello world!", "saltstring", 5000) ==
              "$6$rounds=5000$saltstring$svn8UoSVapNtMuq1ukKS4tPQd8iKwSMHWjl/"
              "O817G3uBnIFNjnQJuesI68u4OTLiBFdcbYEdFCoEOfaS35"
              "inz1");
        CHECK(crypt("Hello world!", "saltstringsaltstring", 10000) ==
              "$6$rounds=10000$saltstringsaltst$OW1/O6BYHV6BcXZu8QVeXbDWra3Oeqh0sbHbbMCVNSnCM/UrjmM0Dp8vOuZeHBy/"
              "YTBmSK6H9q"
              "s/y3RnOaw5v.");
        CHECK(crypt("Hello world!", "saltstringsaltstring", 1000) ==
              "$6$rounds=1000$saltstringsaltst$Y.wiJpF2BZ6guMz15deos0ztCanM.JnsjeF/"
              "8nuaIh5xtCv52yyDujJKkgLElgDE9Wt4JK2JM5w"
              "E7NH1/y9gX.");
        // Fewer than 1000 rounds are 1000.
        CHECK(crypt("Hello world!", "saltstringsaltstring", 10).rfind("$6$rounds=1000$saltstringsaltst$Y.wiJpF2", 0) ==
              0);
        // A long password (the P sequence over several digests).
        CHECK(crypt(std::string(200, 'p'), "a/b.C9", 5000) ==
              "$6$rounds=5000$a/b.C9$6K6GyCskpR4O44KkxXtwB8i9XYQeNqMrp2tM3Pl2r8YtJUdg8pyjKrP6rSaqC9zAG9hR/JWpDMGa4quC/"
              "Eooo/");
        bmds::String out(bmds::MemSpace::Psram);
        CHECK(ShaCrypt::hash("x", "bad$salt", 5000, out) == bmds::Error::Invalid);
        CHECK(ShaCrypt::hash(std::string(ShaCrypt::MaxPassword + 1, 'x').c_str(), "s", 5000, out) ==
              bmds::Error::Invalid);
}

TEST_CASE("ShaCrypt: settings read, hashes told apart, passwords checked") {
        ShaCrypt::Setting s;
        REQUIRE(ShaCrypt::parse("$6$rounds=7000$abc$xyz", s));
        CHECK(s.rounds == 7000);
        CHECK(s.roundsGiven);
        CHECK(s.salt == "abc");
        CHECK(s.hash == "xyz");
        REQUIRE(ShaCrypt::parse("$6$abc", s));
        CHECK(s.rounds == ShaCrypt::DefaultRounds);
        CHECK_FALSE(s.roundsGiven);
        CHECK(s.hash.isEmpty());
        REQUIRE(ShaCrypt::parse("$6$rounds=5$abc", s));
        CHECK(s.rounds == ShaCrypt::MinRounds);
        REQUIRE(ShaCrypt::parse("$6$rounds=99999999999$abc", s) == false); // more digits than any count
        CHECK_FALSE(ShaCrypt::parse("$5$abc", s));
        CHECK_FALSE(ShaCrypt::parse("$6$rounds=x$abc", s));
        CHECK_FALSE(ShaCrypt::parse("$6$a:b", s));

        const std::string made = crypt("secret", "0123456789abcdef", 1000);
        CHECK(ShaCrypt::isHash(made.c_str()));
        CHECK(ShaCrypt::verify("secret", made.c_str()));
        CHECK_FALSE(ShaCrypt::verify("Secret", made.c_str()));
        CHECK_FALSE(ShaCrypt::verify("", made.c_str()));
        CHECK_FALSE(ShaCrypt::isHash("plain password"));
        CHECK_FALSE(ShaCrypt::isHash("$6$abc$short"));
        CHECK_FALSE(ShaCrypt::isHash((made + "x").c_str()));
        CHECK_FALSE(ShaCrypt::isHash(("$6$rounds=1000$0123456789abcdefg$" + made.substr(made.size() - 86)).c_str()));
        // Without rounds=, 5000 (OpenSSL's form).
        CHECK(ShaCrypt::verify("Hello world!",
                               "$6$saltstring$svn8UoSVapNtMuq1ukKS4tPQd8iKwSMHWjl/O817G3uBnIFNjnQJuesI68"
                               "u4OTLiBFdcbYEdFCoEOfaS35inz1"));
}
