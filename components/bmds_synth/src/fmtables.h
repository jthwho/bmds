/**
 * @file      fmtables.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The FM synth's tables (devplan/audio.md §26): the OPL's own log-sin and exponent tables, the
 * eight waveforms made from them (as the chip makes them, at full volume), and the MIDI side's
 * level curve. Generated from the formulas below (the first two match the chip's ROM, as Nuked
 * OPL3 reads it); the waveforms at compile time.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>

BMDS_NAMESPACE_BEGIN

namespace FmTables {

        /** @brief The attenuation that silences (a half wave's silent half): `LogSin[256]`. */
        constexpr uint16_t Silent = 0x1000;

        /**
         * @brief A quarter sine's attenuation in 1/256 octaves (about 0.0235 dB):
         *        `round(-log2(sin((i + 0.5) * pi / 512)) * 256)`, then `Silent`.
         */
        alignas(4) constexpr uint16_t LogSin[257] = {
                2137, 1731, 1543, 1419, 1326, 1252, 1190, 1137, 1091, 1050, 1013, 979, 949, 920, 894, 869, 846, 825,
                804,  785,  767,  749,  732,  717,  701,  687,  672,  659,  646,  633, 621, 609, 598, 587, 576, 566,
                556,  546,  536,  527,  518,  509,  501,  492,  484,  476,  468,  461, 453, 446, 439, 432, 425, 418,
                411,  405,  399,  392,  386,  380,  375,  369,  363,  358,  352,  347, 341, 336, 331, 326, 321, 316,
                311,  307,  302,  297,  293,  289,  284,  280,  276,  271,  267,  263, 259, 255, 251, 248, 244, 240,
                236,  233,  229,  226,  222,  219,  215,  212,  209,  205,  202,  199, 196, 193, 190, 187, 184, 181,
                178,  175,  172,  169,  167,  164,  161,  159,  156,  153,  151,  148, 146, 143, 141, 138, 136, 134,
                131,  129,  127,  125,  122,  120,  118,  116,  114,  112,  110,  108, 106, 104, 102, 100, 98,  96,
                94,   92,   91,   89,   87,   85,   83,   82,   80,   78,   77,   75,  74,  72,  70,  69,  67,  66,
                64,   63,   62,   60,   59,   57,   56,   55,   53,   52,   51,   49,  48,  47,  46,  45,  43,  42,
                41,   40,   39,   38,   37,   36,   35,   34,   33,   32,   31,   30,  29,  28,  27,  26,  25,  24,
                23,   23,   22,   21,   20,   20,   19,   18,   17,   17,   16,   15,  15,  14,  13,  13,  12,  12,
                11,   10,   10,   9,    9,    8,    8,    7,    7,    7,    6,    6,   5,   5,   5,   4,   4,   4,
                3,    3,    3,    2,    2,    2,    2,    1,    1,    1,    1,    1,   1,   1,   0,   0,   0,   0,
                0,    0,    0,    0,    4096,
        };

        /** @brief An attenuation's fraction back to a level: `round(1024 * 2^((255 - i) / 256))`. */
        alignas(4) constexpr uint16_t Exp[256] = {
                2042, 2037, 2031, 2026, 2020, 2015, 2010, 2004, 1999, 1993, 1988, 1983, 1977, 1972, 1966, 1961,
                1956, 1951, 1945, 1940, 1935, 1930, 1924, 1919, 1914, 1909, 1904, 1898, 1893, 1888, 1883, 1878,
                1873, 1868, 1863, 1858, 1853, 1848, 1843, 1838, 1833, 1828, 1823, 1818, 1813, 1808, 1803, 1798,
                1794, 1789, 1784, 1779, 1774, 1769, 1765, 1760, 1755, 1750, 1746, 1741, 1736, 1732, 1727, 1722,
                1717, 1713, 1708, 1704, 1699, 1694, 1690, 1685, 1681, 1676, 1672, 1667, 1663, 1658, 1654, 1649,
                1645, 1640, 1636, 1631, 1627, 1623, 1618, 1614, 1609, 1605, 1601, 1596, 1592, 1588, 1584, 1579,
                1575, 1571, 1566, 1562, 1558, 1554, 1550, 1545, 1541, 1537, 1533, 1529, 1525, 1520, 1516, 1512,
                1508, 1504, 1500, 1496, 1492, 1488, 1484, 1480, 1476, 1472, 1468, 1464, 1460, 1456, 1452, 1448,
                1444, 1440, 1436, 1433, 1429, 1425, 1421, 1417, 1413, 1409, 1406, 1402, 1398, 1394, 1391, 1387,
                1383, 1379, 1376, 1372, 1368, 1364, 1361, 1357, 1353, 1350, 1346, 1342, 1339, 1335, 1332, 1328,
                1324, 1321, 1317, 1314, 1310, 1307, 1303, 1300, 1296, 1292, 1289, 1286, 1282, 1279, 1275, 1272,
                1268, 1265, 1261, 1258, 1255, 1251, 1248, 1244, 1241, 1238, 1234, 1231, 1228, 1224, 1221, 1218,
                1214, 1211, 1208, 1205, 1201, 1198, 1195, 1192, 1188, 1185, 1182, 1179, 1176, 1172, 1169, 1166,
                1163, 1160, 1157, 1154, 1150, 1147, 1144, 1141, 1138, 1135, 1132, 1129, 1126, 1123, 1120, 1117,
                1114, 1111, 1108, 1105, 1102, 1099, 1096, 1093, 1090, 1087, 1084, 1081, 1078, 1075, 1072, 1069,
                1066, 1064, 1061, 1058, 1055, 1052, 1049, 1046, 1044, 1041, 1038, 1035, 1032, 1030, 1027, 1024,
        };

        /**
         * @brief A MIDI level as an attenuation in the envelope's units (0.1875 dB), on
         *        TinySoundFont's curves so both engines answer a song alike: `20 * log10(127 / v)
         *        / 0.1875`, 511 (silent) at most. A velocity's is this; volume and expression
         *        (their product cubed, as TinySoundFont) are three times it each.
         */
        constexpr uint16_t LevelAtt[128] = {
                511, 224, 192, 174, 160, 150, 141, 134, 128, 123, 118, 113, 109, 106, 102, 99, 96, 93, 91, 88, 86, 83,
                81,  79,  77,  75,  73,  72,  70,  68,  67,  65,  64,  62,  61,  60,  58,  57, 56, 55, 54, 52, 51, 50,
                49,  48,  47,  46,  45,  44,  43,  42,  41,  40,  40,  39,  38,  37,  36,  36, 35, 34, 33, 32, 32, 31,
                30,  30,  29,  28,  28,  27,  26,  26,  25,  24,  24,  23,  23,  22,  21,  21, 20, 20, 19, 19, 18, 18,
                17,  16,  16,  15,  15,  14,  14,  13,  13,  12,  12,  12,  11,  11,  10,  10, 9,  9,  8,  8,  8,  7,
                7,   6,   6,   5,   5,   5,   4,   4,   3,   3,   3,   2,   2,   1,   1,   1,  0,  0,
        };

        /** @brief Frames in a waveform's period: the chip's 10-bit phase. */
        constexpr size_t WavePeriod = 1024;

        /**
         * @brief The chip's level for waveform @p w (0 to 7) at phase @p p (10 bits), at full
         *        volume: its log-sin entry through its exponent table, as the chip computes it
         *        (Nuked OPL3's reading); a negative half is the ones' complement, -1 where 0.
         */
        constexpr int32_t chipLevel(unsigned w, uint32_t p) {
                const auto level = [](uint32_t att, int32_t neg) {
                        return int32_t((uint32_t(Exp[att & 0xff]) << 1) >> (att >> 8)) ^ neg;
                };
                const uint32_t quarter = (p ^ (0u - ((p >> 8) & 1))) & 0xff;        // mirrored
                const uint32_t doubled = ((p ^ (0u - ((p >> 7) & 1))) << 1) & 0xff; // twice the speed
                const int32_t  negHalf = -int32_t((p >> 9) & 1);
                switch (w & 7) {
                        case 0: return level(LogSin[quarter], negHalf);                   // sine
                        case 1: return (p & 0x200) != 0 ? 0 : level(LogSin[quarter], 0);  // half sine
                        case 2: return level(LogSin[quarter], 0);                         // absolute sine
                        case 3: return (p & 0x100) != 0 ? 0 : level(LogSin[p & 0xff], 0); // quarter sine
                        case 4:                                                           // alternating sine
                                return (p & 0x200) != 0 ? 0 : level(LogSin[doubled], -int32_t((p >> 8) & 1));
                        case 5: return (p & 0x200) != 0 ? 0 : level(LogSin[doubled], 0); // camel sine
                        case 6: return level(0, negHalf);                                // square
                        default:                                                         // log sawtooth
                                return level(((p ^ uint32_t(negHalf)) & 0x1ff) << 3, negHalf);
                }
        }

        /** @brief The eight waveforms' periods: twice the chip's levels (`chipLevel()`). */
        struct WaveTables {
                        int16_t level[8][WavePeriod];
        };

        constexpr WaveTables makeWaveTables() {
                WaveTables t{};
                for (unsigned w = 0; w < 8; ++w)
                        for (uint32_t p = 0; p < WavePeriod; ++p) t.level[w][p] = int16_t(2 * chipLevel(w, p));
                return t;
        }

        /**
         * @brief The FM synth's waveforms (16 KB of rodata): an operator's level is an entry times
         *        its gain, indexed by its phase's top 10 bits (devplan/audio.md §26, the linear form).
         */
        alignas(4) inline constexpr WaveTables Waves = makeWaveTables();

} // namespace FmTables

BMDS_NAMESPACE_END
