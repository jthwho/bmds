/**
 * @file      abilock.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The ABI lock: the userspace ABI's numbers and layouts, checked at
 * compile time in every firmware build (and the host tests). These
 * assertions are append-only: one that fails means the change broke
 * userspace, not that the assertion is wrong (docs/ABI.md).
 */

#include <stddef.h>
#include <stdint.h>
#include <bmds/errno.h>
#include <bmds/program.h>
#include <bmds/sys.h>

// --- bmds/sys.h -----------------------------------------------------------

_Static_assert(BMDS_SYS_MAGIC == 0x53444d42u, "ABI: BMDS_SYS_MAGIC");
_Static_assert(BMDS_SYS_EXIT == 0, "ABI: slot EXIT");
_Static_assert(BMDS_SYS_READ == 1, "ABI: slot READ");
_Static_assert(BMDS_SYS_WRITE == 2, "ABI: slot WRITE");
_Static_assert(BMDS_SYS_CLOSE == 3, "ABI: slot CLOSE");
_Static_assert(BMDS_SYS_SEEK == 4, "ABI: slot SEEK");
_Static_assert(BMDS_SYS_ISATTY == 5, "ABI: slot ISATTY");
_Static_assert(BMDS_SYS_MEM_ALLOC == 6, "ABI: slot MEM_ALLOC");
_Static_assert(BMDS_SYS_MEM_FREE == 7, "ABI: slot MEM_FREE");
_Static_assert(BMDS_SYS_MEM_REALLOC == 8, "ABI: slot MEM_REALLOC");
_Static_assert(BMDS_SYS_OPEN == 9, "ABI: slot OPEN");
_Static_assert(BMDS_SYS_STAT == 10, "ABI: slot STAT");
_Static_assert(BMDS_SYS_FSTAT == 11, "ABI: slot FSTAT");
_Static_assert(BMDS_SYS_MKDIR == 12, "ABI: slot MKDIR");
_Static_assert(BMDS_SYS_RMDIR == 13, "ABI: slot RMDIR");
_Static_assert(BMDS_SYS_UNLINK == 14, "ABI: slot UNLINK");
_Static_assert(BMDS_SYS_RENAME == 15, "ABI: slot RENAME");
_Static_assert(BMDS_SYS_OPENDIR == 16, "ABI: slot OPENDIR");
_Static_assert(BMDS_SYS_READDIR == 17, "ABI: slot READDIR");
_Static_assert(BMDS_SYS_CLOSEDIR == 18, "ABI: slot CLOSEDIR");
_Static_assert(BMDS_SYS_GETCWD == 19, "ABI: slot GETCWD");
_Static_assert(BMDS_SYS_CHDIR == 20, "ABI: slot CHDIR");
_Static_assert(BMDS_SYS_DUP == 21, "ABI: slot DUP");
_Static_assert(BMDS_SYS_DUP2 == 22, "ABI: slot DUP2");
_Static_assert(BMDS_SYS_CLOCK_GET == 23, "ABI: slot CLOCK_GET");
_Static_assert(BMDS_SYS_SLEEP == 24, "ABI: slot SLEEP");
_Static_assert(BMDS_SYS_INTERRUPTED == 25, "ABI: slot INTERRUPTED");
_Static_assert(BMDS_SYS_TERM_GET == 26, "ABI: slot TERM_GET");
_Static_assert(BMDS_SYS_TERM_SET == 27, "ABI: slot TERM_SET");
_Static_assert(BMDS_SYS_MEM_INFO == 28, "ABI: slot MEM_INFO");
_Static_assert(BMDS_SYS_RANDOM == 29, "ABI: slot RANDOM");
_Static_assert(BMDS_SYS_SET_MTIME == 30, "ABI: slot SET_MTIME");
_Static_assert(BMDS_SYS_FTRUNCATE == 31, "ABI: slot FTRUNCATE");
_Static_assert(BMDS_SYS_SYMLINK == 32, "ABI: slot SYMLINK");
_Static_assert(BMDS_SYS_READLINK == 33, "ABI: slot READLINK");
_Static_assert(BMDS_SYS_LSTAT == 34, "ABI: slot LSTAT");
_Static_assert(BMDS_SYS_PROC_VFORK == 35, "ABI: slot PROC_VFORK");
_Static_assert(BMDS_SYS_PROC_EXEC == 36, "ABI: slot PROC_EXEC");
_Static_assert(BMDS_SYS_PROC_VFORK_EXIT == 37, "ABI: slot PROC_VFORK_EXIT");
_Static_assert(BMDS_SYS_PROC_WAIT == 38, "ABI: slot PROC_WAIT");
_Static_assert(BMDS_SYS_PIPE == 39, "ABI: slot PIPE");
_Static_assert(BMDS_SYS_PROC_ID == 40, "ABI: slot PROC_ID");
_Static_assert(BMDS_SYS_FD_FLAGS == 41, "ABI: slot FD_FLAGS");
_Static_assert(BMDS_SYS_SIG_SETUP == 42, "ABI: slot SIG_SETUP");
_Static_assert(BMDS_SYS_SIG_ACTION == 43, "ABI: slot SIG_ACTION");
_Static_assert(BMDS_SYS_SIG_MASK == 44, "ABI: slot SIG_MASK");
_Static_assert(BMDS_SYS_SIG_PENDING == 45, "ABI: slot SIG_PENDING");
_Static_assert(BMDS_SYS_SIG_TAKE == 46, "ABI: slot SIG_TAKE");
_Static_assert(BMDS_SYS_SIG_SUSPEND == 47, "ABI: slot SIG_SUSPEND");
_Static_assert(BMDS_SYS_PROC_KILL == 48, "ABI: slot PROC_KILL");
_Static_assert(BMDS_SYS_PROC_EXIT_SIGNAL == 49, "ABI: slot PROC_EXIT_SIGNAL");
_Static_assert(BMDS_SYS_IO_POLL == 50, "ABI: slot IO_POLL");
_Static_assert(BMDS_SYS_PROC_GROUP == 51, "ABI: slot PROC_GROUP");
_Static_assert(BMDS_SYS_TERM_FOREGROUND == 52, "ABI: slot TERM_FOREGROUND");
_Static_assert(BMDS_SYS_THREAD_START == 53, "ABI: slot THREAD_START");
_Static_assert(BMDS_SYS_THREAD_EXIT == 54, "ABI: slot THREAD_EXIT");
_Static_assert(BMDS_SYS_FUTEX == 55, "ABI: slot FUTEX");
_Static_assert(BMDS_SYS_THREAD_KILL == 56, "ABI: slot THREAD_KILL");
_Static_assert(BMDS_SYS_THREAD_CLEAR == 57, "ABI: slot THREAD_CLEAR");
_Static_assert(BMDS_SYS_FILE_TYPE == 58, "ABI: slot FILE_TYPE");
_Static_assert(BMDS_FILE_TYPE_NAME == 0x1 && BMDS_FILE_TYPE_CONTENT == 0x2 && BMDS_FILE_TYPE_MAX == 127,
               "ABI: file_type flags");
_Static_assert(BMDS_SYS_SYSCONF_FIND == 59 && BMDS_SYS_SYSCONF_GET == 60 && BMDS_SYS_SYSCONF_INFO == 61,
               "ABI: slots SYSCONF_FIND, _GET, _INFO");
_Static_assert(BMDS_SYS_SYSCONF_SET == 62 && BMDS_SYS_SYSCONF_NEXT == 63 && BMDS_SYS_SYSCONF_DECLARE == 64,
               "ABI: slots SYSCONF_SET, _NEXT, _DECLARE");
_Static_assert(BMDS_SYS_SYSCONF_WATCH == 65, "ABI: slot SYSCONF_WATCH");
_Static_assert(BMDS_SYS_SYNC == 66, "ABI: slot SYNC");
_Static_assert(BMDS_SYS_AUDIO_CONFIG == 67, "ABI: slot AUDIO_CONFIG");
_Static_assert(BMDS_SYS_AUDIO_STATUS == 68, "ABI: slot AUDIO_STATUS");
_Static_assert(BMDS_SYS_AUDIO_CONTROL == 69, "ABI: slot AUDIO_CONTROL");
_Static_assert(BMDS_SYS_SOUND_PLAY == 70, "ABI: slot SOUND_PLAY");
_Static_assert(BMDS_SYS_AUDIO_DECODE == 71 && BMDS_SYS_AUDIO_DECODE_INFO == 72 && BMDS_SYS_AUDIO_DECODE_TAG == 73,
               "ABI: slots AUDIO_DECODE, _INFO, _TAG");
_Static_assert(BMDS_SYS_AUDIO_CYCLE == 74, "ABI: slot AUDIO_CYCLE");
_Static_assert(BMDS_SYS_MIDI_CONFIG == 75, "ABI: slot MIDI_CONFIG");
_Static_assert(BMDS_SYS_MIDI_STATUS == 76, "ABI: slot MIDI_STATUS");
_Static_assert(BMDS_SYS_MIDI_CONTROL == 77, "ABI: slot MIDI_CONTROL");
_Static_assert(BMDS_SYS_AUDIO_SYNTH == 78, "ABI: slot AUDIO_SYNTH");
_Static_assert(BMDS_SYS_AUDIO_SYNTH_INFO == 79, "ABI: slot AUDIO_SYNTH_INFO");
_Static_assert(BMDS_SYS_STATS_LIST == 80 && BMDS_SYS_STATS_READ == 81 && BMDS_SYS_STATS_HISTORY == 82,
               "ABI: slots STATS_LIST, _READ, _HISTORY");
_Static_assert(BMDS_SYS_STATS_PROBLEMS == 83 && BMDS_SYS_STATS_CONTROL == 84 && BMDS_SYS_STATS_STATUS == 85,
               "ABI: slots STATS_PROBLEMS, _CONTROL, _STATUS");
_Static_assert(BMDS_SYS_TASK_LIST == 86 && BMDS_SYS_PROC_INFO == 87, "ABI: slots TASK_LIST, PROC_INFO");
_Static_assert(BMDS_SYS_SYSTEM_INFO == 88, "ABI: slot SYSTEM_INFO");
_Static_assert(BMDS_SYS_NOTIFY_OPEN == 89 && BMDS_SYS_NOTIFY_SEND == 90 && BMDS_SYS_NOTIFY_CLOSE == 91,
               "ABI: slots NOTIFY_OPEN, NOTIFY_SEND, NOTIFY_CLOSE");
_Static_assert(BMDS_SYS_EXPAND == 92, "ABI: slot EXPAND");
_Static_assert(BMDS_SYS_IMAGE_INFO == 93 && BMDS_SYS_IMAGE_DECODE == 94 && BMDS_SYS_IMAGE_ENCODE == 95,
               "ABI: slots IMAGE_INFO, IMAGE_DECODE, IMAGE_ENCODE");
_Static_assert(BMDS_SYS_SOCKET == 96 && BMDS_SYS_BIND == 97 && BMDS_SYS_LISTEN == 98 && BMDS_SYS_ACCEPT == 99,
               "ABI: slots SOCKET, BIND, LISTEN, ACCEPT");
_Static_assert(BMDS_SYS_CONNECT == 100 && BMDS_SYS_SOCK_NAME == 101 && BMDS_SYS_SENDMSG == 102 &&
                       BMDS_SYS_RECVMSG == 103,
               "ABI: slots CONNECT, SOCK_NAME, SENDMSG, RECVMSG");
_Static_assert(BMDS_SYS_SETSOCKOPT == 104 && BMDS_SYS_GETSOCKOPT == 105 && BMDS_SYS_SHUTDOWN == 106 &&
                       BMDS_SYS_NET_INFO == 107,
               "ABI: slots SETSOCKOPT, GETSOCKOPT, SHUTDOWN, NET_INFO");
_Static_assert(BMDS_SYS_PROC_TIMER == 108, "ABI: slot PROC_TIMER");
_Static_assert(BMDS_SYS_NET_WATCH == 109, "ABI: slot NET_WATCH");
_Static_assert(BMDS_SYS_TERM_CTL == 110, "ABI: slot TERM_CTL");
_Static_assert(BMDS_SYS_FS_INFO == 111, "ABI: slot FS_INFO");
_Static_assert(offsetof(bmds_fs_info_t, size) == 0 && offsetof(bmds_fs_info_t, block_bytes) == 4 &&
                       offsetof(bmds_fs_info_t, total_bytes) == 8 && offsetof(bmds_fs_info_t, free_bytes) == 16 &&
                       offsetof(bmds_fs_info_t, type) == 24 && sizeof(bmds_fs_info_t) == 40,
               "ABI: bmds_fs_info_t");
_Static_assert(BMDS_SYS_CRYPTO == 112, "ABI: slot CRYPTO");
_Static_assert(BMDS_CRYPTO_AES_ECB_ENCRYPT == 1 && BMDS_CRYPTO_AES_ECB_DECRYPT == 2 &&
                       BMDS_CRYPTO_AES_CBC_ENCRYPT == 3 && BMDS_CRYPTO_AES_CBC_DECRYPT == 4 &&
                       BMDS_CRYPTO_AES_CTR == 5 && BMDS_CRYPTO_SHA1 == 16 && BMDS_CRYPTO_SHA256 == 17 &&
                       BMDS_CRYPTO_SHA512 == 18,
               "ABI: crypto's algorithms");
#if UINTPTR_MAX == 0xffffffffu // (pointers inside)
_Static_assert(offsetof(bmds_crypto_op_t, size) == 0 && offsetof(bmds_crypto_op_t, algorithm) == 4 &&
                       offsetof(bmds_crypto_op_t, key) == 8 && offsetof(bmds_crypto_op_t, key_bytes) == 12 &&
                       offsetof(bmds_crypto_op_t, state) == 16 && offsetof(bmds_crypto_op_t, in) == 20 &&
                       offsetof(bmds_crypto_op_t, out) == 24 && offsetof(bmds_crypto_op_t, length) == 28 &&
                       sizeof(bmds_crypto_op_t) == 32,
               "ABI: bmds_crypto_op_t");
#endif
_Static_assert(BMDS_SYS_SCHED == 113, "ABI: slot SCHED");
_Static_assert(BMDS_SCHED_OTHER == 0 && BMDS_SCHED_FIFO == 1 && BMDS_SCHED_RR == 2 && BMDS_SCHED_BATCH == 3 &&
                       BMDS_SCHED_IDLE == 5 && BMDS_SCHED_RESET_ON_FORK == 0x40000000u,
               "ABI: scheduling policies (Linux's)");
_Static_assert(BMDS_SCHED_PRIORITY_MIN == 1 && BMDS_SCHED_PRIORITY_MAX == 99, "ABI: real-time priorities");
_Static_assert(BMDS_SCHED_GET == 0 && BMDS_SCHED_SET == 1 && BMDS_SCHED_YIELD == 2 && BMDS_SCHED_PIN == 3,
               "ABI: sched operations");
_Static_assert(offsetof(bmds_sched_t, size) == 0 && offsetof(bmds_sched_t, policy) == 4 &&
                       offsetof(bmds_sched_t, priority) == 8 && offsetof(bmds_sched_t, cores) == 12 &&
                       offsetof(bmds_sched_t, core) == 16 && offsetof(bmds_sched_t, kernel_priority) == 20 &&
                       offsetof(bmds_sched_t, throttled) == 24 && offsetof(bmds_sched_t, flags) == 28 &&
                       sizeof(bmds_sched_t) == 32,
               "ABI: bmds_sched_t");
_Static_assert(BMDS_SYS_AUDIO_SPEAK == 114, "ABI: slot AUDIO_SPEAK");
_Static_assert(BMDS_SYS_AUDIO_SPEAK_INFO == 115, "ABI: slot AUDIO_SPEAK_INFO");
_Static_assert(BMDS_SYS_PTP_CTL == 116, "ABI: slot PTP_CTL");
_Static_assert(BMDS_SYS_SURFACE_OPEN == 117 && BMDS_SYS_SURFACE_BUFFER == 118 && BMDS_SYS_SURFACE_ACQUIRE == 119 &&
                       BMDS_SYS_SURFACE_PRESENT == 120 && BMDS_SYS_SURFACE_CONTROL == 121,
               "ABI: slots SURFACE_*");
_Static_assert(BMDS_SPEAK_MARKUP == 0x1000u && BMDS_SPEAK_VOICE_MAX == 32, "ABI: speech flags and sizes");
#if UINTPTR_MAX == 0xffffffffu // (a pointer inside)
_Static_assert(offsetof(bmds_speak_config_t, size) == 0 && offsetof(bmds_speak_config_t, flags) == 4 &&
                       offsetof(bmds_speak_config_t, voice) == 8 && offsetof(bmds_speak_config_t, speed) == 12 &&
                       offsetof(bmds_speak_config_t, pitch) == 16 && offsetof(bmds_speak_config_t, volume) == 20 &&
                       sizeof(bmds_speak_config_t) == 24,
               "ABI: bmds_speak_config_t");
#endif
_Static_assert(offsetof(bmds_speak_info_t, size) == 0 && offsetof(bmds_speak_info_t, rate) == 4 &&
                       offsetof(bmds_speak_info_t, channels) == 8 && offsetof(bmds_speak_info_t, format) == 12 &&
                       offsetof(bmds_speak_info_t, frames) == 16 && offsetof(bmds_speak_info_t, speed) == 24 &&
                       offsetof(bmds_speak_info_t, pitch) == 28 && offsetof(bmds_speak_info_t, volume) == 32 &&
                       offsetof(bmds_speak_info_t, flags) == 36 && offsetof(bmds_speak_info_t, voice) == 40 &&
                       sizeof(bmds_speak_info_t) == 72,
               "ABI: bmds_speak_info_t");
_Static_assert(BMDS_TERM_PTY_NUMBER == 0 && BMDS_TERM_PTY_UNLOCK == 1 && BMDS_TERM_SET_SIZE == 2 &&
                       BMDS_TERM_CONTROL == 3 && BMDS_TERM_RELEASE == 4 && BMDS_TERM_PTS_NUMBER == 5,
               "ABI: term_ctl operations");
_Static_assert(BMDS_TIMER_REAL == 0, "ABI: timers (Linux's ITIMER_REAL)");
_Static_assert(offsetof(bmds_timer_t, size) == 0 && offsetof(bmds_timer_t, reserved) == 4 &&
                       offsetof(bmds_timer_t, value_us) == 8 && offsetof(bmds_timer_t, interval_us) == 16 &&
                       sizeof(bmds_timer_t) == 24,
               "ABI: bmds_timer_t");
_Static_assert(BMDS_AF_UNSPEC == 0 && BMDS_AF_INET == 2, "ABI: address families (Linux's)");
_Static_assert(BMDS_SOCK_STREAM == 1 && BMDS_SOCK_DGRAM == 2 && BMDS_SOCK_RAW == 3, "ABI: socket types (Linux's)");
_Static_assert(BMDS_SOCK_CLOEXEC == BMDS_O_CLOEXEC && BMDS_SOCK_DONTWAIT == 0x1, "ABI: socket flags");
_Static_assert(BMDS_MSG_PEEK == 0x2 && BMDS_MSG_CTRUNC == 0x8 && BMDS_MSG_TRUNC == 0x20 && BMDS_MSG_DONTWAIT == 0x40 &&
                       BMDS_MSG_WAITALL == 0x100 && BMDS_MSG_NOSIGNAL == 0x4000 && BMDS_MSG_MORE == 0x8000,
               "ABI: message flags (Linux's)");
_Static_assert(BMDS_SHUT_RD == 0 && BMDS_SHUT_WR == 1 && BMDS_SHUT_RDWR == 2, "ABI: shutdown sides");
_Static_assert(BMDS_SOCK_NAME_LOCAL == 0 && BMDS_SOCK_NAME_PEER == 1, "ABI: sock_name");
_Static_assert(BMDS_MSG_ERRQUEUE == 0x2000, "ABI: MSG_ERRQUEUE (Linux's)");
_Static_assert(BMDS_SOL_BMDS == 0x424d4453 && BMDS_SO_FASTPATH == 1, "ABI: BMDS_SOL_BMDS, BMDS_SO_FASTPATH");
_Static_assert(offsetof(bmds_fastpath_t, packet_us) == 4 && offsetof(bmds_fastpath_t, depth) == 8 &&
                       sizeof(bmds_fastpath_t) == 12,
               "ABI: bmds_fastpath_t");
_Static_assert(BMDS_IOV_MAX == 64, "ABI: BMDS_IOV_MAX");
_Static_assert(BMDS_NET_UP == 0x1 && BMDS_NET_LINK == 0x2 && BMDS_NET_FULL_DUPLEX == 0x4 && BMDS_NET_LOOPBACK == 0x8 &&
                       BMDS_NET_BROADCAST == 0x10 && BMDS_NET_MULTICAST == 0x20,
               "ABI: net_info flags");
_Static_assert(BMDS_NET_MODE_DHCP == 0 && BMDS_NET_MODE_STATIC == 1 && BMDS_NET_MODE_OFF == 2, "ABI: net_info modes");
_Static_assert(BMDS_NET_DHCP_NONE == 0 && BMDS_NET_DHCP_ASKING == 1 && BMDS_NET_DHCP_BOUND == 2,
               "ABI: net_info DHCP states");
_Static_assert(offsetof(bmds_sockaddr_in_t, port) == 2 && offsetof(bmds_sockaddr_in_t, addr) == 4 &&
                       sizeof(bmds_sockaddr_in_t) == 16,
               "ABI: bmds_sockaddr_in_t (Linux's sockaddr_in)");
_Static_assert(offsetof(bmds_net_info_t, name) == 4 && offsetof(bmds_net_info_t, index) == 20 &&
                       offsetof(bmds_net_info_t, flags) == 24 && offsetof(bmds_net_info_t, mtu) == 28 &&
                       offsetof(bmds_net_info_t, speed_mbps) == 32 && offsetof(bmds_net_info_t, mac) == 36 &&
                       offsetof(bmds_net_info_t, mode) == 42 && offsetof(bmds_net_info_t, dhcp) == 43,
               "ABI: bmds_net_info_t, to dhcp");
_Static_assert(offsetof(bmds_net_info_t, address) == 44 && offsetof(bmds_net_info_t, netmask) == 48 &&
                       offsetof(bmds_net_info_t, gateway) == 52 && offsetof(bmds_net_info_t, dhcp_server) == 56 &&
                       offsetof(bmds_net_info_t, lease_s) == 60 && offsetof(bmds_net_info_t, lease_left_s) == 64 &&
                       offsetof(bmds_net_info_t, dns) == 68 && offsetof(bmds_net_info_t, reserved) == 76,
               "ABI: bmds_net_info_t, addresses");
_Static_assert(offsetof(bmds_net_info_t, rx_bytes) == 80 && offsetof(bmds_net_info_t, tx_bytes) == 88 &&
                       offsetof(bmds_net_info_t, rx_frames) == 96 && offsetof(bmds_net_info_t, tx_frames) == 100 &&
                       offsetof(bmds_net_info_t, rx_errors) == 104 && offsetof(bmds_net_info_t, tx_errors) == 108 &&
                       offsetof(bmds_net_info_t, rx_dropped) == 112 && offsetof(bmds_net_info_t, tx_dropped) == 116 &&
                       sizeof(bmds_net_info_t) == 120,
               "ABI: bmds_net_info_t, counters");
_Static_assert(BMDS_IMAGE_PNG == 0 && BMDS_IMAGE_JPEG == 1 && BMDS_IMAGE_BMP == 2, "ABI: image codecs");
_Static_assert(BMDS_PIXEL_BGR24 == 0 && BMDS_PIXEL_RGB24 == 1 && BMDS_PIXEL_BGRA32 == 2 && BMDS_PIXEL_RGB565 == 3 &&
                       BMDS_PIXEL_GRAY8 == 4,
               "ABI: pixel formats");
_Static_assert(BMDS_IMAGE_ALPHA == 0x1 && BMDS_IMAGE_PROGRESSIVE == 0x2 && BMDS_IMAGE_HARDWARE == 0x4,
               "ABI: image flags");
_Static_assert(BMDS_IMAGE_DECODER_PNG == 0 && BMDS_IMAGE_DECODER_BMP == 1 && BMDS_IMAGE_DECODER_HARDWARE == 2 &&
                       BMDS_IMAGE_DECODER_SOFTWARE == 3,
               "ABI: image decoders");
_Static_assert(BMDS_IMAGE_ORIENT == 0x1 && BMDS_IMAGE_NO_HARDWARE == 0x2, "ABI: image decode flags");
_Static_assert(BMDS_JPEG_YUV420 == 0 && BMDS_JPEG_YUV422 == 1 && BMDS_JPEG_YUV444 == 2 && BMDS_JPEG_GRAY == 3 &&
                       BMDS_PNG_STORED == 10,
               "ABI: image encoding");
_Static_assert(BMDS_NOTIFY_LOW == 0 && BMDS_NOTIFY_NORMAL == 1 && BMDS_NOTIFY_CRITICAL == 2, "ABI: notify urgency");
_Static_assert(BMDS_NOTIFY_SILENT == 0x1 && BMDS_NOTIFY_TRANSIENT == 0x2, "ABI: notify flags");
_Static_assert(BMDS_NOTIFY_TARGET_NONE == 0 && BMDS_NOTIFY_TARGET_OPEN == 1 && BMDS_NOTIFY_TARGET_SHOW == 2 &&
                       BMDS_NOTIFY_TARGET_LAUNCH == 3 && BMDS_NOTIFY_TARGET_SERVICE == 4,
               "ABI: notify targets");
_Static_assert(BMDS_NOTIFY_DEFAULT_TIMEOUT == 0 && BMDS_NOTIFY_NO_TIMEOUT == -1 && BMDS_NOTIFY_MAX_ACTIONS == 3,
               "ABI: notify timeouts, actions");
_Static_assert(BMDS_NOTIFY_ACTIVATED == 1 && BMDS_NOTIFY_DISMISSED == 2 && BMDS_NOTIFY_EXPIRED == 3,
               "ABI: notify events");
_Static_assert(BMDS_SYSCONF_KEY_MAX == 63 && BMDS_SYSCONF_VALUE_MAX == 1024, "ABI: sysconf lengths");
_Static_assert(BMDS_SYSCONF_TYPE_BOOL == 0 && BMDS_SYSCONF_TYPE_INT == 1 && BMDS_SYSCONF_TYPE_STRING == 2 &&
                       BMDS_SYSCONF_TYPE_COLOR == 3 && BMDS_SYSCONF_TYPE_ENUM == 4 && BMDS_SYSCONF_TYPE_PATH == 5 &&
                       BMDS_SYSCONF_TYPE_PATHS == 6 && BMDS_SYSCONF_TYPE_SECRET == 7,
               "ABI: sysconf types");
_Static_assert(BMDS_SYSCONF_FIELD_VALUE == 0 && BMDS_SYSCONF_FIELD_DEFAULT == 1 && BMDS_SYSCONF_FIELD_SAVED == 2 &&
                       BMDS_SYSCONF_FIELD_KEY == 3 && BMDS_SYSCONF_FIELD_HELP == 4 && BMDS_SYSCONF_FIELD_CHOICES == 5,
               "ABI: sysconf fields");
_Static_assert(BMDS_SYSCONF_OP_SET == 0 && BMDS_SYSCONF_OP_SETDEF == 1 && BMDS_SYSCONF_OP_REVERT == 2 &&
                       BMDS_SYSCONF_OP_SAVE == 3 && BMDS_SYSCONF_ALL == -1,
               "ABI: sysconf operations");
_Static_assert(BMDS_SYSCONF_IS_DEFAULT == 0x1 && BMDS_SYSCONF_IS_SAVED == 0x2 && BMDS_SYSCONF_CHANGED == 0x4 &&
                       BMDS_SYSCONF_RUNTIME == 0x8,
               "ABI: sysconf flags");
_Static_assert(BMDS_SYSCONF_OVERFLOW == 0xffffffffu, "ABI: sysconf watch overflow record");
_Static_assert(BMDS_TYPE_LINK == 4, "ABI: file type LINK");
_Static_assert(BMDS_ABI_VERSION == 1, "ABI: version (raise it with every addition once shipped)");
_Static_assert(BMDS_STDIN == 0 && BMDS_STDOUT == 1 && BMDS_STDERR == 2, "ABI: standard fds");
_Static_assert(BMDS_SEEK_SET == 0 && BMDS_SEEK_CUR == 1 && BMDS_SEEK_END == 2, "ABI: whence");
_Static_assert(BMDS_MEM_FAST == 1 && BMDS_MEM_DMA == 2 && BMDS_MEM_ALIGN == 16, "ABI: memory flags");

_Static_assert(BMDS_PATH_MAX == 1024 && BMDS_NAME_MAX == 765, "ABI: path and name lengths");
_Static_assert(BMDS_O_READ == 0x01 && BMDS_O_WRITE == 0x02 && BMDS_O_CREATE == 0x04, "ABI: open flags");
_Static_assert(BMDS_O_TRUNC == 0x08 && BMDS_O_APPEND == 0x10 && BMDS_O_EXCL == 0x20, "ABI: open flags");
_Static_assert(BMDS_O_CLOEXEC == 0x40 && BMDS_FD_CLOEXEC == 0x1, "ABI: close-on-exec flags");
_Static_assert(BMDS_WAIT_NOHANG == 0x1 && BMDS_PROC_SELF == 0 && BMDS_PROC_PARENT == 1, "ABI: process flags");
_Static_assert(BMDS_WAIT_UNTRACED == 0x2 && BMDS_WAIT_CONTINUED == 0x4, "ABI: wait flags");
_Static_assert(BMDS_TYPE_FILE == 1 && BMDS_TYPE_DIRECTORY == 2 && BMDS_TYPE_DEVICE == 3, "ABI: file types");
_Static_assert(BMDS_ATTR_READONLY == 0x01 && BMDS_ATTR_HIDDEN == 0x02, "ABI: attributes");
_Static_assert(BMDS_ATTR_SYSTEM == 0x04 && BMDS_ATTR_ARCHIVE == 0x20, "ABI: attributes");
_Static_assert(BMDS_CLOCK_REALTIME == 0 && BMDS_CLOCK_MONOTONIC == 1, "ABI: clocks");
_Static_assert(BMDS_TERM_SIGNALS == 0x01 && BMDS_TERM_ONLCR == 0x02 && BMDS_TERM_COLORS == 0x04, "ABI: term flags");
_Static_assert(BMDS_TERM_LOGS == 0x08 && BMDS_TERM_LINE == 0x10, "ABI: term flags");
_Static_assert(BMDS_TERM_ECHO == 0x20 && BMDS_TERM_ICRNL == 0x40, "ABI: term flags");
_Static_assert(BMDS_PROC_JOB == 2, "ABI: proc_id JOB");
_Static_assert(BMDS_PROC_THREAD == 3, "ABI: proc_id THREAD");
_Static_assert(BMDS_FUTEX_WAIT == 0 && BMDS_FUTEX_WAKE == 1 && BMDS_FUTEX_REQUEUE == 2, "ABI: futex operations");
_Static_assert(BMDS_THREAD_WAKE == 0x1 && BMDS_THREAD_STACK_MIN == 8192, "ABI: thread values");
_Static_assert(BMDS_GROUP_GET == 0 && BMDS_GROUP_SET == 1 && BMDS_GROUP_SESSION == 2 && BMDS_GROUP_NEW_SESSION == 3,
               "ABI: proc_group operations");
_Static_assert(BMDS_SIGHUP == 1 && BMDS_SIGINT == 2 && BMDS_SIGQUIT == 3 && BMDS_SIGILL == 4, "ABI: signals 1-4");
_Static_assert(BMDS_SIGTRAP == 5 && BMDS_SIGABRT == 6 && BMDS_SIGBUS == 7 && BMDS_SIGFPE == 8, "ABI: signals 5-8");
_Static_assert(BMDS_SIGKILL == 9 && BMDS_SIGUSR1 == 10 && BMDS_SIGSEGV == 11 && BMDS_SIGUSR2 == 12,
               "ABI: signals 9-12");
_Static_assert(BMDS_SIGPIPE == 13 && BMDS_SIGALRM == 14 && BMDS_SIGTERM == 15 && BMDS_SIGSTKFLT == 16,
               "ABI: signals 13-16");
_Static_assert(BMDS_SIGCHLD == 17 && BMDS_SIGCONT == 18 && BMDS_SIGSTOP == 19 && BMDS_SIGTSTP == 20,
               "ABI: signals 17-20");
_Static_assert(BMDS_SIGTTIN == 21 && BMDS_SIGTTOU == 22 && BMDS_SIGURG == 23 && BMDS_SIGXCPU == 24,
               "ABI: signals 21-24");
_Static_assert(BMDS_SIGXFSZ == 25 && BMDS_SIGVTALRM == 26 && BMDS_SIGPROF == 27 && BMDS_SIGWINCH == 28,
               "ABI: signals 25-28");
_Static_assert(BMDS_SIGIO == 29 && BMDS_SIGPWR == 30 && BMDS_SIGSYS == 31 && BMDS_SIG_COUNT == 32,
               "ABI: signals 29-31");
_Static_assert(BMDS_SIG_DEFAULT == 0 && BMDS_SIG_IGNORE == 1 && BMDS_SIG_CATCH == 2, "ABI: signal actions");
_Static_assert(BMDS_SIG_QUERY == 0xffffffffu, "ABI: signal action query");
_Static_assert(BMDS_SIG_BLOCK == 0 && BMDS_SIG_UNBLOCK == 1 && BMDS_SIG_SETMASK == 2, "ABI: signal mask hows");
_Static_assert(BMDS_SI_USER == 0 && BMDS_SI_KERNEL == 0x80 && BMDS_SIG_TERMINATE == 0x1, "ABI: siginfo values");
_Static_assert(BMDS_POLL_IN == 0x01 && BMDS_POLL_PRI == 0x02 && BMDS_POLL_OUT == 0x04, "ABI: poll events");
_Static_assert(BMDS_POLL_ERR == 0x08 && BMDS_POLL_HUP == 0x10 && BMDS_POLL_NVAL == 0x20, "ABI: poll events");
_Static_assert(BMDS_POLL_MAX == 64, "ABI: poll entries");

_Static_assert(offsetof(bmds_stat_t, size) == 0, "ABI: stat.size");
_Static_assert(offsetof(bmds_stat_t, type) == 4, "ABI: stat.type");
_Static_assert(offsetof(bmds_stat_t, attributes) == 8, "ABI: stat.attributes");
_Static_assert(offsetof(bmds_stat_t, open_flags) == 12, "ABI: stat.open_flags");
_Static_assert(offsetof(bmds_stat_t, file_size) == 16, "ABI: stat.file_size");
_Static_assert(offsetof(bmds_stat_t, modified_ns) == 24, "ABI: stat.modified_ns");
_Static_assert(offsetof(bmds_stat_t, file_id) == 32, "ABI: stat.file_id");
_Static_assert(offsetof(bmds_stat_t, device) == 40, "ABI: stat.device");
_Static_assert(offsetof(bmds_stat_t, reserved) == 44, "ABI: stat.reserved");
_Static_assert(sizeof(bmds_stat_t) == 48, "ABI: bmds_stat_t size");
_Static_assert(sizeof(bmds_term_t) == 16, "ABI: bmds_term_t size");
_Static_assert(offsetof(bmds_term_t, size) == 0, "ABI: term.size");
_Static_assert(offsetof(bmds_term_t, flags) == 4, "ABI: term.flags");
_Static_assert(offsetof(bmds_term_t, columns) == 8, "ABI: term.columns");
_Static_assert(offsetof(bmds_term_t, rows) == 10, "ABI: term.rows");
_Static_assert(offsetof(bmds_term_t, colors) == 12, "ABI: term.colors");
_Static_assert(sizeof(bmds_mem_info_t) == 24, "ABI: bmds_mem_info_t size");
_Static_assert(offsetof(bmds_mem_info_t, size) == 0, "ABI: mem_info.size");
_Static_assert(offsetof(bmds_mem_info_t, total_bytes) == 4, "ABI: mem_info.total_bytes");
_Static_assert(offsetof(bmds_mem_info_t, free_bytes) == 8, "ABI: mem_info.free_bytes");
_Static_assert(offsetof(bmds_mem_info_t, largest_free) == 12, "ABI: mem_info.largest_free");
_Static_assert(offsetof(bmds_mem_info_t, program_bytes) == 16, "ABI: mem_info.program_bytes");
_Static_assert(offsetof(bmds_mem_info_t, program_blocks) == 20, "ABI: mem_info.program_blocks");

_Static_assert(offsetof(bmds_siginfo_t, size) == 0, "ABI: siginfo.size");
_Static_assert(offsetof(bmds_siginfo_t, signo) == 4, "ABI: siginfo.signo");
_Static_assert(offsetof(bmds_siginfo_t, code) == 8, "ABI: siginfo.code");
_Static_assert(offsetof(bmds_siginfo_t, flags) == 12, "ABI: siginfo.flags");
_Static_assert(sizeof(bmds_siginfo_t) == 16, "ABI: bmds_siginfo_t size");

_Static_assert(offsetof(bmds_pollfd_t, size) == 0, "ABI: pollfd.size");
_Static_assert(offsetof(bmds_pollfd_t, fd) == 4, "ABI: pollfd.fd");
_Static_assert(offsetof(bmds_pollfd_t, events) == 8, "ABI: pollfd.events");
_Static_assert(offsetof(bmds_pollfd_t, revents) == 12, "ABI: pollfd.revents");
_Static_assert(sizeof(bmds_pollfd_t) == 16, "ABI: bmds_pollfd_t size");

_Static_assert(offsetof(bmds_sysconf_info_t, size) == 0, "ABI: sysconf_info.size");
_Static_assert(offsetof(bmds_sysconf_info_t, type) == 4, "ABI: sysconf_info.type");
_Static_assert(offsetof(bmds_sysconf_info_t, min) == 8, "ABI: sysconf_info.min");
_Static_assert(offsetof(bmds_sysconf_info_t, max) == 12, "ABI: sysconf_info.max");
_Static_assert(offsetof(bmds_sysconf_info_t, flags) == 16, "ABI: sysconf_info.flags");
_Static_assert(offsetof(bmds_sysconf_info_t, reserved) == 20, "ABI: sysconf_info.reserved");
_Static_assert(sizeof(bmds_sysconf_info_t) == 24, "ABI: bmds_sysconf_info_t size");

#if UINTPTR_MAX == 0xffffffffu
_Static_assert(offsetof(bmds_sysconf_param_t, size) == 0, "ABI: sysconf_param.size");
_Static_assert(offsetof(bmds_sysconf_param_t, type) == 4, "ABI: sysconf_param.type");
_Static_assert(offsetof(bmds_sysconf_param_t, key) == 8, "ABI: sysconf_param.key");
_Static_assert(offsetof(bmds_sysconf_param_t, default_value) == 12, "ABI: sysconf_param.default_value");
_Static_assert(offsetof(bmds_sysconf_param_t, help) == 16, "ABI: sysconf_param.help");
_Static_assert(offsetof(bmds_sysconf_param_t, choices) == 20, "ABI: sysconf_param.choices");
_Static_assert(offsetof(bmds_sysconf_param_t, min) == 24, "ABI: sysconf_param.min");
_Static_assert(offsetof(bmds_sysconf_param_t, max) == 28, "ABI: sysconf_param.max");
_Static_assert(sizeof(bmds_sysconf_param_t) == 32, "ABI: bmds_sysconf_param_t size");
#endif

// The audio structs hold no pointers: the same on the host.
_Static_assert(BMDS_AUDIO_U8 == 0 && BMDS_AUDIO_S8 == 1 && BMDS_AUDIO_S16LE == 2 && BMDS_AUDIO_S16BE == 3 &&
                       BMDS_AUDIO_S24LE == 4 && BMDS_AUDIO_S24_3LE == 5 && BMDS_AUDIO_S32LE == 6 &&
                       BMDS_AUDIO_F32LE == 7 && BMDS_AUDIO_MULAW == 8 && BMDS_AUDIO_ALAW == 9,
               "ABI: audio formats");
_Static_assert(sizeof(bmds_audio_config_t) == 32, "ABI: bmds_audio_config_t size");
_Static_assert(offsetof(bmds_audio_config_t, size) == 0, "ABI: audio_config.size");
_Static_assert(offsetof(bmds_audio_config_t, format) == 4, "ABI: audio_config.format");
_Static_assert(offsetof(bmds_audio_config_t, channels) == 8, "ABI: audio_config.channels");
_Static_assert(offsetof(bmds_audio_config_t, rate) == 12, "ABI: audio_config.rate");
_Static_assert(offsetof(bmds_audio_config_t, ring_frames) == 16, "ABI: audio_config.ring_frames");
_Static_assert(offsetof(bmds_audio_config_t, start_frames) == 20, "ABI: audio_config.start_frames");
_Static_assert(offsetof(bmds_audio_config_t, wake_frames) == 24, "ABI: audio_config.wake_frames");
_Static_assert(offsetof(bmds_audio_config_t, flags) == 28, "ABI: audio_config.flags");
_Static_assert(BMDS_AUDIO_IDLE == 0 && BMDS_AUDIO_WAITING == 1 && BMDS_AUDIO_PLAYING == 2 && BMDS_AUDIO_PAUSED == 3 &&
                       BMDS_AUDIO_DRAINING == 4 && BMDS_AUDIO_MIXER == 5,
               "ABI: audio states");
_Static_assert(sizeof(bmds_audio_status_t) == 88, "ABI: bmds_audio_status_t size");
_Static_assert(offsetof(bmds_audio_status_t, size) == 0, "ABI: audio_status.size");
_Static_assert(offsetof(bmds_audio_status_t, state) == 4, "ABI: audio_status.state");
_Static_assert(offsetof(bmds_audio_status_t, device) == 8, "ABI: audio_status.device");
_Static_assert(offsetof(bmds_audio_status_t, device_rate) == 12, "ABI: audio_status.device_rate");
_Static_assert(offsetof(bmds_audio_status_t, period_frames) == 16, "ABI: audio_status.period_frames");
_Static_assert(offsetof(bmds_audio_status_t, formats) == 20, "ABI: audio_status.formats");
_Static_assert(offsetof(bmds_audio_status_t, min_rate) == 24, "ABI: audio_status.min_rate");
_Static_assert(offsetof(bmds_audio_status_t, max_rate) == 28, "ABI: audio_status.max_rate");
_Static_assert(offsetof(bmds_audio_status_t, max_channels) == 32, "ABI: audio_status.max_channels");
_Static_assert(offsetof(bmds_audio_status_t, ring_frames) == 36, "ABI: audio_status.ring_frames");
_Static_assert(offsetof(bmds_audio_status_t, written_frames) == 40, "ABI: audio_status.written_frames");
_Static_assert(offsetof(bmds_audio_status_t, played_frames) == 48, "ABI: audio_status.played_frames");
_Static_assert(offsetof(bmds_audio_status_t, timestamp_ns) == 56, "ABI: audio_status.timestamp_ns");
_Static_assert(offsetof(bmds_audio_status_t, queued_frames) == 64, "ABI: audio_status.queued_frames");
_Static_assert(offsetof(bmds_audio_status_t, delay_frames) == 68, "ABI: audio_status.delay_frames");
_Static_assert(offsetof(bmds_audio_status_t, underruns) == 72, "ABI: audio_status.underruns");
_Static_assert(offsetof(bmds_audio_status_t, gain_cdb) == 76, "ABI: audio_status.gain_cdb");
_Static_assert(offsetof(bmds_audio_status_t, late_periods) == 80, "ABI: audio_status.late_periods");
_Static_assert(offsetof(bmds_audio_status_t, limited_frames) == 84, "ABI: audio_status.limited_frames");
_Static_assert(BMDS_AUDIO_START == 1 && BMDS_AUDIO_PAUSE == 2 && BMDS_AUDIO_RESUME == 3 && BMDS_AUDIO_DRAIN == 4 &&
                       BMDS_AUDIO_DROP == 5 && BMDS_AUDIO_GAIN == 6 && BMDS_AUDIO_GAIN_MUTED == -9600,
               "ABI: audio_control ops");
_Static_assert(BMDS_SOUND_BELL == 0 && BMDS_SOUND_CLICK == 1 && BMDS_SOUND_ERROR == 2 && BMDS_SOUND_NOTIFY == 3,
               "ABI: sounds");
_Static_assert(BMDS_CODEC_WAV == 0 && BMDS_CODEC_MP3 == 1 && BMDS_CODEC_VORBIS == 2 && BMDS_CODEC_FLAC == 3,
               "ABI: codecs");
_Static_assert(BMDS_CODEC_MIDI == 4, "ABI: codec MIDI");
_Static_assert(BMDS_DECODE_NATIVE == 0xffffffffu && BMDS_DECODE_UNKNOWN_FRAMES == 0xffffffffffffffffull &&
                       BMDS_DECODE_ESTIMATED == 1,
               "ABI: decode constants");
_Static_assert(sizeof(bmds_decode_info_t) == 64, "ABI: bmds_decode_info_t size");
_Static_assert(offsetof(bmds_decode_info_t, size) == 0, "ABI: decode_info.size");
_Static_assert(offsetof(bmds_decode_info_t, codec) == 4, "ABI: decode_info.codec");
_Static_assert(offsetof(bmds_decode_info_t, name) == 8, "ABI: decode_info.name");
_Static_assert(offsetof(bmds_decode_info_t, format) == 16, "ABI: decode_info.format");
_Static_assert(offsetof(bmds_decode_info_t, channels) == 20, "ABI: decode_info.channels");
_Static_assert(offsetof(bmds_decode_info_t, rate) == 24, "ABI: decode_info.rate");
_Static_assert(offsetof(bmds_decode_info_t, bitrate) == 28, "ABI: decode_info.bitrate");
_Static_assert(offsetof(bmds_decode_info_t, frames) == 32, "ABI: decode_info.frames");
_Static_assert(offsetof(bmds_decode_info_t, flags) == 40, "ABI: decode_info.flags");
_Static_assert(offsetof(bmds_decode_info_t, source_bits) == 44, "ABI: decode_info.source_bits");
_Static_assert(offsetof(bmds_decode_info_t, tags) == 48, "ABI: decode_info.tags");
_Static_assert(offsetof(bmds_decode_info_t, native_format) == 52, "ABI: decode_info.native_format");
_Static_assert(offsetof(bmds_decode_info_t, frame_bytes) == 56, "ABI: decode_info.frame_bytes");
_Static_assert(offsetof(bmds_decode_config_t, size) == 0, "ABI: decode_config.size");
_Static_assert(offsetof(bmds_decode_config_t, format) == 4, "ABI: decode_config.format");
_Static_assert(offsetof(bmds_decode_config_t, flags) == 8, "ABI: decode_config.flags");
_Static_assert(offsetof(bmds_decode_config_t, length) == 12, "ABI: decode_config.length");
_Static_assert(offsetof(bmds_decode_config_t, data) == 16, "ABI: decode_config.data");
_Static_assert(BMDS_AUDIO_REALTIME == 0x1 && BMDS_AUDIO_EXCLUSIVE == 0x2, "ABI: audio_config flags");
_Static_assert(BMDS_AUDIO_MIN_PERIOD == 16 && BMDS_AUDIO_MAX_PERIOD == 480, "ABI: real-time periods");
_Static_assert(offsetof(bmds_audio_cycle_t, size) == 0, "ABI: audio_cycle.size");
_Static_assert(offsetof(bmds_audio_cycle_t, frames) == 4, "ABI: audio_cycle.frames");
_Static_assert(offsetof(bmds_audio_cycle_t, channels) == 8, "ABI: audio_cycle.channels");
_Static_assert(offsetof(bmds_audio_cycle_t, rate) == 12, "ABI: audio_cycle.rate");
_Static_assert(offsetof(bmds_audio_cycle_t, out) == 16, "ABI: audio_cycle.out");

_Static_assert(BMDS_MIDI_EVENTS == 0x1 && BMDS_MIDI_CREATE == 0x2 && BMDS_MIDI_SOURCE == 0x4 &&
                       BMDS_MIDI_DESTINATION == 0x8,
               "ABI: midi flags");
_Static_assert(BMDS_MIDI_NAME_MAX == 32, "ABI: midi name");
_Static_assert(BMDS_MIDI_USB == 0 && BMDS_MIDI_SERIAL == 1 && BMDS_MIDI_VIRTUAL == 2, "ABI: midi kinds");
_Static_assert(BMDS_MIDI_GONE == 0x1, "ABI: midi status flags");
_Static_assert(BMDS_MIDI_CONNECT == 1 && BMDS_MIDI_DISCONNECT == 2 && BMDS_MIDI_FLUSH == 3, "ABI: midi ops");
_Static_assert(offsetof(bmds_midi_config_t, size) == 0, "ABI: midi_config.size");
_Static_assert(offsetof(bmds_midi_config_t, flags) == 4, "ABI: midi_config.flags");
_Static_assert(offsetof(bmds_midi_config_t, event_size) == 8, "ABI: midi_config.event_size");
_Static_assert(offsetof(bmds_midi_config_t, port) == 12, "ABI: midi_config.port");
_Static_assert(offsetof(bmds_midi_config_t, name) == 16, "ABI: midi_config.name");
_Static_assert(sizeof(bmds_midi_config_t) == 48, "ABI: bmds_midi_config_t size");
_Static_assert(offsetof(bmds_midi_event_t, time_ns) == 0, "ABI: midi_event.time_ns");
_Static_assert(offsetof(bmds_midi_event_t, port) == 8, "ABI: midi_event.port");
_Static_assert(offsetof(bmds_midi_event_t, length) == 9, "ABI: midi_event.length");
_Static_assert(offsetof(bmds_midi_event_t, flags) == 10, "ABI: midi_event.flags");
_Static_assert(offsetof(bmds_midi_event_t, data) == 11, "ABI: midi_event.data");
_Static_assert(sizeof(bmds_midi_event_t) == 16, "ABI: bmds_midi_event_t size");
_Static_assert(offsetof(bmds_midi_status_t, size) == 0, "ABI: midi_status.size");
_Static_assert(offsetof(bmds_midi_status_t, port) == 4, "ABI: midi_status.port");
_Static_assert(offsetof(bmds_midi_status_t, kind) == 8, "ABI: midi_status.kind");
_Static_assert(offsetof(bmds_midi_status_t, sides) == 12, "ABI: midi_status.sides");
_Static_assert(offsetof(bmds_midi_status_t, flags) == 16, "ABI: midi_status.flags");
_Static_assert(offsetof(bmds_midi_status_t, dropped) == 20, "ABI: midi_status.dropped");
_Static_assert(offsetof(bmds_midi_status_t, events_in) == 24, "ABI: midi_status.events_in");
_Static_assert(offsetof(bmds_midi_status_t, events_out) == 32, "ABI: midi_status.events_out");
_Static_assert(offsetof(bmds_midi_status_t, parse_errors) == 40, "ABI: midi_status.parse_errors");
_Static_assert(offsetof(bmds_midi_status_t, device_errors) == 44, "ABI: midi_status.device_errors");
_Static_assert(offsetof(bmds_midi_status_t, last_ns) == 48, "ABI: midi_status.last_ns");
_Static_assert(offsetof(bmds_midi_status_t, fd_events) == 56, "ABI: midi_status.fd_events");
_Static_assert(offsetof(bmds_midi_status_t, fd_dropped) == 64, "ABI: midi_status.fd_dropped");
_Static_assert(offsetof(bmds_midi_status_t, reserved) == 68, "ABI: midi_status.reserved");
_Static_assert(offsetof(bmds_midi_status_t, name) == 72, "ABI: midi_status.name");
_Static_assert(sizeof(bmds_midi_status_t) == 104, "ABI: bmds_midi_status_t size");
_Static_assert(BMDS_SYNTH_QUEUE == 1024, "ABI: synth queue");
_Static_assert(offsetof(bmds_synth_event_t, frame) == 0, "ABI: synth_event.frame");
_Static_assert(offsetof(bmds_synth_event_t, length) == 8, "ABI: synth_event.length");
_Static_assert(offsetof(bmds_synth_event_t, flags) == 9, "ABI: synth_event.flags");
_Static_assert(offsetof(bmds_synth_event_t, data) == 10, "ABI: synth_event.data");
_Static_assert(sizeof(bmds_synth_event_t) == 16, "ABI: bmds_synth_event_t size");
_Static_assert(offsetof(bmds_synth_config_t, size) == 0, "ABI: synth_config.size");
_Static_assert(offsetof(bmds_synth_config_t, flags) == 4, "ABI: synth_config.flags");
_Static_assert(offsetof(bmds_synth_config_t, rate) == 8, "ABI: synth_config.rate");
_Static_assert(offsetof(bmds_synth_config_t, channels) == 12, "ABI: synth_config.channels");
_Static_assert(offsetof(bmds_synth_config_t, format) == 16, "ABI: synth_config.format");
_Static_assert(offsetof(bmds_synth_config_t, max_voices) == 20, "ABI: synth_config.max_voices");
_Static_assert(offsetof(bmds_synth_config_t, font) == 24, "ABI: synth_config.font");
_Static_assert(offsetof(bmds_synth_info_t, size) == 0, "ABI: synth_info.size");
_Static_assert(offsetof(bmds_synth_info_t, rate) == 4, "ABI: synth_info.rate");
_Static_assert(offsetof(bmds_synth_info_t, channels) == 8, "ABI: synth_info.channels");
_Static_assert(offsetof(bmds_synth_info_t, format) == 12, "ABI: synth_info.format");
_Static_assert(offsetof(bmds_synth_info_t, frames) == 16, "ABI: synth_info.frames");
_Static_assert(offsetof(bmds_synth_info_t, voices) == 24, "ABI: synth_info.voices");
_Static_assert(offsetof(bmds_synth_info_t, max_voices) == 28, "ABI: synth_info.max_voices");
_Static_assert(offsetof(bmds_synth_info_t, peak_voices) == 32, "ABI: synth_info.peak_voices");
_Static_assert(offsetof(bmds_synth_info_t, notes_at_limit) == 36, "ABI: synth_info.notes_at_limit");
_Static_assert(offsetof(bmds_synth_info_t, queued) == 40, "ABI: synth_info.queued");
_Static_assert(offsetof(bmds_synth_info_t, queue_full) == 44, "ABI: synth_info.queue_full");
_Static_assert(offsetof(bmds_synth_info_t, ignored) == 48, "ABI: synth_info.ignored");
_Static_assert(offsetof(bmds_synth_info_t, presets) == 52, "ABI: synth_info.presets");
_Static_assert(offsetof(bmds_synth_info_t, font_bytes) == 56, "ABI: synth_info.font_bytes");
_Static_assert(offsetof(bmds_synth_info_t, cpu_permille) == 60, "ABI: synth_info.cpu_permille");
_Static_assert(offsetof(bmds_synth_info_t, cpu_permille_max) == 64, "ABI: synth_info.cpu_permille_max");
_Static_assert(offsetof(bmds_synth_info_t, reserved) == 68, "ABI: synth_info.reserved");
_Static_assert(offsetof(bmds_synth_info_t, font) == 72, "ABI: synth_info.font");
_Static_assert(sizeof(bmds_synth_info_t) == 200, "ABI: bmds_synth_info_t size");

_Static_assert(BMDS_COUNTER_UNAVAILABLE == INT64_MIN && BMDS_COUNTER_NO_LEVEL == INT64_MIN, "ABI: counter markers");
_Static_assert(BMDS_COUNTER_NAME_MAX == 47, "ABI: counter name length");
_Static_assert(BMDS_UNIT_COUNT == 0 && BMDS_UNIT_BYTES == 1 && BMDS_UNIT_MICROS == 2 && BMDS_UNIT_PERMILLE == 3,
               "ABI: units 0-3");
_Static_assert(BMDS_UNIT_HZ == 4 && BMDS_UNIT_MILLIHZ == 5 && BMDS_UNIT_PPM == 6 && BMDS_UNIT_FRAMES == 7,
               "ABI: units 4-7");
_Static_assert(BMDS_UNIT_CYCLES == 8 && BMDS_UNIT_DECIBELS == 9 && BMDS_UNIT_NANOS == 10 && BMDS_UNIT_PPB == 11,
               "ABI: units 8-11");
_Static_assert(BMDS_COUNTER_TOTAL == 0 && BMDS_COUNTER_LEVEL == 1 && BMDS_COUNTER_PEAK == 2 &&
                       BMDS_COUNTER_STATE == 3 && BMDS_COUNTER_INFO == 4,
               "ABI: counter kinds");
_Static_assert(BMDS_COUNTER_HISTORY == 0x1 && BMDS_COUNTER_ADJUSTED == 0x2, "ABI: counter flags");
_Static_assert(BMDS_RULE_NONE == 0 && BMDS_RULE_ZERO == 1 && BMDS_RULE_ABOVE == 2 && BMDS_RULE_BELOW == 3 &&
                       BMDS_RULE_DRIFT == 4 && BMDS_RULE_STATE == 5,
               "ABI: rules");
_Static_assert(BMDS_HEALTH_NORMAL == 0 && BMDS_HEALTH_WARNING == 1 && BMDS_HEALTH_CRITICAL == 2 &&
                       BMDS_HEALTH_UNJUDGED == 255,
               "ABI: health");
_Static_assert(BMDS_PROBLEM_OPEN == 0x1 && BMDS_PROBLEM_EARLIER == 0x2, "ABI: problem flags");
_Static_assert(BMDS_STATS_WATCH == 1 && BMDS_STATS_SEEN == 2 && BMDS_STATS_CLEAR == 3, "ABI: stats_control ops");
_Static_assert(BMDS_STATS_CPU_COUNTED == 0x1 && BMDS_STATS_WATCHING == 0x2, "ABI: stats_status flags");
_Static_assert(BMDS_TASK_RUNNING == 0 && BMDS_TASK_READY == 1 && BMDS_TASK_BLOCKED == 2 && BMDS_TASK_SUSPENDED == 3 &&
                       BMDS_TASK_DELETED == 4,
               "ABI: task states");
_Static_assert(BMDS_TASK_MAIN == 0x1 && BMDS_TASK_STACK_WARN == 512 && BMDS_TASK_STACK_CRIT == 256,
               "ABI: task flags and stack levels");
_Static_assert(BMDS_PROC_INFO_NEXT == 0x1, "ABI: proc_info flags");
_Static_assert(BMDS_PROC_RUNNING == 0 && BMDS_PROC_VFORK == 1 && BMDS_PROC_ENDING == 2 && BMDS_PROC_ZOMBIE == 3 &&
                       BMDS_PROC_STOPPED == 4,
               "ABI: process states");

_Static_assert(offsetof(bmds_counter_t, size) == 0, "ABI: counter.size");
_Static_assert(offsetof(bmds_counter_t, id) == 4, "ABI: counter.id");
_Static_assert(offsetof(bmds_counter_t, generation) == 8, "ABI: counter.generation");
_Static_assert(offsetof(bmds_counter_t, unit) == 12, "ABI: counter.unit");
_Static_assert(offsetof(bmds_counter_t, kind) == 16, "ABI: counter.kind");
_Static_assert(offsetof(bmds_counter_t, flags) == 20, "ABI: counter.flags");
_Static_assert(offsetof(bmds_counter_t, rule) == 24, "ABI: counter.rule");
_Static_assert(offsetof(bmds_counter_t, health) == 28, "ABI: counter.health");
_Static_assert(offsetof(bmds_counter_t, hold) == 32, "ABI: counter.hold");
_Static_assert(offsetof(bmds_counter_t, reserved) == 36, "ABI: counter.reserved");
_Static_assert(offsetof(bmds_counter_t, warn) == 40, "ABI: counter.warn");
_Static_assert(offsetof(bmds_counter_t, crit) == 48, "ABI: counter.crit");
_Static_assert(offsetof(bmds_counter_t, name) == 56, "ABI: counter.name");
_Static_assert(offsetof(bmds_counter_t, labels) == 104, "ABI: counter.labels");
_Static_assert(offsetof(bmds_counter_t, help) == 168, "ABI: counter.help");
_Static_assert(sizeof(bmds_counter_t) == 296, "ABI: bmds_counter_t size");
_Static_assert(offsetof(bmds_problem_t, size) == 0, "ABI: problem.size");
_Static_assert(offsetof(bmds_problem_t, id) == 4, "ABI: problem.id");
_Static_assert(offsetof(bmds_problem_t, health) == 8, "ABI: problem.health");
_Static_assert(offsetof(bmds_problem_t, flags) == 12, "ABI: problem.flags");
_Static_assert(offsetof(bmds_problem_t, changed) == 16, "ABI: problem.changed");
_Static_assert(offsetof(bmds_problem_t, start_ns) == 24, "ABI: problem.start_ns");
_Static_assert(offsetof(bmds_problem_t, end_ns) == 32, "ABI: problem.end_ns");
_Static_assert(offsetof(bmds_problem_t, first_value) == 40, "ABI: problem.first_value");
_Static_assert(offsetof(bmds_problem_t, last_value) == 48, "ABI: problem.last_value");
_Static_assert(offsetof(bmds_problem_t, count) == 56, "ABI: problem.count");
_Static_assert(offsetof(bmds_problem_t, name) == 64, "ABI: problem.name");
_Static_assert(sizeof(bmds_problem_t) == 112, "ABI: bmds_problem_t size");
_Static_assert(offsetof(bmds_stats_status_t, size) == 0, "ABI: stats_status.size");
_Static_assert(offsetof(bmds_stats_status_t, generation) == 4, "ABI: stats_status.generation");
_Static_assert(offsetof(bmds_stats_status_t, count) == 8, "ABI: stats_status.count");
_Static_assert(offsetof(bmds_stats_status_t, watchers) == 12, "ABI: stats_status.watchers");
_Static_assert(offsetof(bmds_stats_status_t, interval_ms) == 16, "ABI: stats_status.interval_ms");
_Static_assert(offsetof(bmds_stats_status_t, flags) == 20, "ABI: stats_status.flags");
_Static_assert(offsetof(bmds_stats_status_t, unseen) == 24, "ABI: stats_status.unseen");
_Static_assert(offsetof(bmds_stats_status_t, cores) == 28, "ABI: stats_status.cores");
_Static_assert(offsetof(bmds_stats_status_t, last_change) == 32, "ABI: stats_status.last_change");
_Static_assert(offsetof(bmds_stats_status_t, last_sample_ns) == 40, "ABI: stats_status.last_sample_ns");
_Static_assert(sizeof(bmds_stats_status_t) == 48, "ABI: bmds_stats_status_t size");
_Static_assert(offsetof(bmds_task_info_t, size) == 0, "ABI: task_info.size");
_Static_assert(offsetof(bmds_task_info_t, pid) == 4, "ABI: task_info.pid");
_Static_assert(offsetof(bmds_task_info_t, tid) == 8, "ABI: task_info.tid");
_Static_assert(offsetof(bmds_task_info_t, state) == 12, "ABI: task_info.state");
_Static_assert(offsetof(bmds_task_info_t, priority) == 16, "ABI: task_info.priority");
_Static_assert(offsetof(bmds_task_info_t, base_priority) == 20, "ABI: task_info.base_priority");
_Static_assert(offsetof(bmds_task_info_t, core) == 24, "ABI: task_info.core");
_Static_assert(offsetof(bmds_task_info_t, stack_free) == 28, "ABI: task_info.stack_free");
_Static_assert(offsetof(bmds_task_info_t, cpu_us) == 32, "ABI: task_info.cpu_us");
_Static_assert(offsetof(bmds_task_info_t, number) == 40, "ABI: task_info.number");
_Static_assert(offsetof(bmds_task_info_t, flags) == 44, "ABI: task_info.flags");
_Static_assert(offsetof(bmds_task_info_t, name) == 48, "ABI: task_info.name");
_Static_assert(sizeof(bmds_task_info_t) == 64, "ABI: bmds_task_info_t size");
_Static_assert(offsetof(bmds_proc_info_t, size) == 0, "ABI: proc_info.size");
_Static_assert(offsetof(bmds_proc_info_t, pid) == 4, "ABI: proc_info.pid");
_Static_assert(offsetof(bmds_proc_info_t, parent) == 8, "ABI: proc_info.parent");
_Static_assert(offsetof(bmds_proc_info_t, group) == 12, "ABI: proc_info.group");
_Static_assert(offsetof(bmds_proc_info_t, session) == 16, "ABI: proc_info.session");
_Static_assert(offsetof(bmds_proc_info_t, state) == 20, "ABI: proc_info.state");
_Static_assert(offsetof(bmds_proc_info_t, threads) == 24, "ABI: proc_info.threads");
_Static_assert(offsetof(bmds_proc_info_t, status) == 28, "ABI: proc_info.status");
_Static_assert(offsetof(bmds_proc_info_t, memory) == 32, "ABI: proc_info.memory");
_Static_assert(offsetof(bmds_proc_info_t, cpu_us) == 40, "ABI: proc_info.cpu_us");
_Static_assert(offsetof(bmds_proc_info_t, start_ns) == 48, "ABI: proc_info.start_ns");
_Static_assert(offsetof(bmds_proc_info_t, command) == 56, "ABI: proc_info.command");
_Static_assert(offsetof(bmds_proc_info_t, children_cpu_us) == 104, "ABI: proc_info.children_cpu_us");
_Static_assert(sizeof(bmds_proc_info_t) == 112, "ABI: bmds_proc_info_t size"); /* 104 before 2026-10-10 */

_Static_assert(offsetof(bmds_system_info_t, size) == 0, "ABI: system_info.size");
_Static_assert(offsetof(bmds_system_info_t, cores) == 4, "ABI: system_info.cores");
_Static_assert(offsetof(bmds_system_info_t, cpu_mhz) == 8, "ABI: system_info.cpu_mhz");
_Static_assert(offsetof(bmds_system_info_t, chip_revision) == 12, "ABI: system_info.chip_revision");
_Static_assert(offsetof(bmds_system_info_t, serial) == 16, "ABI: system_info.serial");
_Static_assert(offsetof(bmds_system_info_t, hostname) == 48, "ABI: system_info.hostname");
_Static_assert(offsetof(bmds_system_info_t, board) == 112, "ABI: system_info.board");
_Static_assert(offsetof(bmds_system_info_t, version) == 144, "ABI: system_info.version");
#if UINTPTR_MAX == 0xffffffffu // (pointers inside)
_Static_assert(offsetof(bmds_image_decode_t, format) == 4 && offsetof(bmds_image_decode_t, flags) == 8 &&
                       offsetof(bmds_image_decode_t, background) == 12 &&
                       offsetof(bmds_image_decode_t, min_width) == 16 &&
                       offsetof(bmds_image_decode_t, min_height) == 20 &&
                       offsetof(bmds_image_decode_t, box_width) == 24 &&
                       offsetof(bmds_image_decode_t, box_height) == 28 && offsetof(bmds_image_decode_t, out) == 32 &&
                       offsetof(bmds_image_decode_t, out_size) == 36 && offsetof(bmds_image_decode_t, stride) == 40 &&
                       offsetof(bmds_image_decode_t, data) == 44 && offsetof(bmds_image_decode_t, length) == 48 &&
                       sizeof(bmds_image_decode_t) == 52,
               "ABI: bmds_image_decode_t");
#endif
_Static_assert(offsetof(bmds_image_info_t, codec) == 4 && offsetof(bmds_image_info_t, width) == 8 &&
                       offsetof(bmds_image_info_t, height) == 12 && offsetof(bmds_image_info_t, flags) == 16 &&
                       offsetof(bmds_image_info_t, orientation) == 20 &&
                       offsetof(bmds_image_info_t, decode_width) == 24 &&
                       offsetof(bmds_image_info_t, decode_height) == 28 && offsetof(bmds_image_info_t, decoder) == 32 &&
                       offsetof(bmds_image_info_t, shift) == 36 && offsetof(bmds_image_info_t, align) == 40 &&
                       offsetof(bmds_image_info_t, stride) == 44 && offsetof(bmds_image_info_t, bytes) == 48 &&
                       sizeof(bmds_image_info_t) == 52,
               "ABI: bmds_image_info_t");
#if UINTPTR_MAX == 0xffffffffu // (pointers inside)
_Static_assert(offsetof(bmds_image_encode_t, codec) == 4 && offsetof(bmds_image_encode_t, format) == 8 &&
                       offsetof(bmds_image_encode_t, width) == 12 && offsetof(bmds_image_encode_t, height) == 16 &&
                       offsetof(bmds_image_encode_t, stride) == 20 && offsetof(bmds_image_encode_t, pixels) == 24 &&
                       offsetof(bmds_image_encode_t, quality) == 28 && offsetof(bmds_image_encode_t, sampling) == 32 &&
                       offsetof(bmds_image_encode_t, level) == 36 && sizeof(bmds_image_encode_t) == 40,
               "ABI: bmds_image_encode_t");
#endif
_Static_assert(sizeof(bmds_system_info_t) == 160, "ABI: bmds_system_info_t size");

_Static_assert(offsetof(bmds_notification_t, size) == 0, "ABI: notification.size");
_Static_assert(offsetof(bmds_notification_t, replaces) == 4, "ABI: notification.replaces");
_Static_assert(offsetof(bmds_notification_t, urgency) == 8, "ABI: notification.urgency");
_Static_assert(offsetof(bmds_notification_t, flags) == 12, "ABI: notification.flags");
_Static_assert(offsetof(bmds_notification_t, timeout_ms) == 16, "ABI: notification.timeout_ms");
_Static_assert(offsetof(bmds_notification_t, target) == 20, "ABI: notification.target");
_Static_assert(offsetof(bmds_notification_t, action_count) == 24, "ABI: notification.action_count");
_Static_assert(offsetof(bmds_notification_t, app) == 28, "ABI: notification.app");
_Static_assert(offsetof(bmds_notification_t, icon) == 60, "ABI: notification.icon");
_Static_assert(offsetof(bmds_notification_t, title) == 124, "ABI: notification.title");
_Static_assert(offsetof(bmds_notification_t, body) == 188, "ABI: notification.body");
_Static_assert(offsetof(bmds_notification_t, target_text) == 444, "ABI: notification.target_text");
_Static_assert(offsetof(bmds_notification_t, message) == 636, "ABI: notification.message");
_Static_assert(offsetof(bmds_notification_t, actions) == 700, "ABI: notification.actions");
_Static_assert(sizeof(bmds_notification_t) == 772, "ABI: bmds_notification_t size");
_Static_assert(offsetof(bmds_notify_event_t, id) == 4 && offsetof(bmds_notify_event_t, event) == 8 &&
                       offsetof(bmds_notify_event_t, action) == 12,
               "ABI: notify_event fields");
_Static_assert(sizeof(bmds_notify_event_t) == 16, "ABI: bmds_notify_event_t size");

#if UINTPTR_MAX == 0xffffffffu
_Static_assert(offsetof(bmds_thread_t, size) == 0, "ABI: thread.size");
_Static_assert(offsetof(bmds_thread_t, entry) == 4, "ABI: thread.entry");
_Static_assert(offsetof(bmds_thread_t, arg) == 8, "ABI: thread.arg");
_Static_assert(offsetof(bmds_thread_t, tls) == 12, "ABI: thread.tls");
_Static_assert(offsetof(bmds_thread_t, stack_size) == 16, "ABI: thread.stack_size");
_Static_assert(offsetof(bmds_thread_t, hint) == 20, "ABI: thread.hint");
_Static_assert(offsetof(bmds_thread_t, clear) == 24, "ABI: thread.clear");
_Static_assert(offsetof(bmds_thread_t, tid) == 28, "ABI: thread.tid");
_Static_assert(sizeof(bmds_thread_t) == 32, "ABI: bmds_thread_t size");
_Static_assert(sizeof(bmds_decode_config_t) == 20, "ABI: bmds_decode_config_t size");
_Static_assert(offsetof(bmds_audio_cycle_t, in) == 20, "ABI: audio_cycle.in");
_Static_assert(offsetof(bmds_audio_cycle_t, position) == 24, "ABI: audio_cycle.position");
_Static_assert(offsetof(bmds_audio_cycle_t, deadline_ns) == 32, "ABI: audio_cycle.deadline_ns");
_Static_assert(offsetof(bmds_audio_cycle_t, play_ns) == 40, "ABI: audio_cycle.play_ns");
_Static_assert(offsetof(bmds_audio_cycle_t, skipped) == 48, "ABI: audio_cycle.skipped");
_Static_assert(offsetof(bmds_audio_cycle_t, late) == 52, "ABI: audio_cycle.late");
_Static_assert(offsetof(bmds_audio_cycle_t, throttled) == 56, "ABI: audio_cycle.throttled");
_Static_assert(offsetof(bmds_audio_cycle_t, flags) == 60, "ABI: audio_cycle.flags");
_Static_assert(sizeof(bmds_audio_cycle_t) == 64, "ABI: bmds_audio_cycle_t size");
_Static_assert(sizeof(bmds_synth_config_t) == 28, "ABI: bmds_synth_config_t size");
_Static_assert(offsetof(bmds_iovec_t, length) == 4 && sizeof(bmds_iovec_t) == 8, "ABI: bmds_iovec_t");
_Static_assert(offsetof(bmds_msghdr_t, name) == 4 && offsetof(bmds_msghdr_t, name_length) == 8 &&
                       offsetof(bmds_msghdr_t, iov) == 12 && offsetof(bmds_msghdr_t, iov_count) == 16 &&
                       offsetof(bmds_msghdr_t, control) == 20 && offsetof(bmds_msghdr_t, control_length) == 24 &&
                       offsetof(bmds_msghdr_t, flags) == 28 && sizeof(bmds_msghdr_t) == 32,
               "ABI: bmds_msghdr_t");
#endif

_Static_assert(offsetof(bmds_sys_t, magic) == 0, "ABI: bmds_sys_t.magic");
_Static_assert(offsetof(bmds_sys_t, abi_version) == 4, "ABI: bmds_sys_t.abi_version");
_Static_assert(offsetof(bmds_sys_t, count) == 8, "ABI: bmds_sys_t.count");
_Static_assert(offsetof(bmds_sys_t, reserved) == 12, "ABI: bmds_sys_t.reserved");
_Static_assert(offsetof(bmds_sys_t, slot) == 16, "ABI: bmds_sys_t.slot");
#if UINTPTR_MAX == 0xffffffffu
_Static_assert(sizeof(bmds_sys_fn_t) == 4, "ABI: slot size");
#endif

// --- bmds/program.h -------------------------------------------------------

_Static_assert(BMDS_NOTE_PROGRAM == 1, "ABI: note type");
_Static_assert(sizeof(BMDS_NOTE_NAME) == 5, "ABI: note name");
_Static_assert(BMDS_DEFAULT_STACK_SIZE == 32768, "ABI: default stack");
_Static_assert(BMDS_MAX_STACK_SIZE == 1048576, "ABI: largest stack");
_Static_assert(BMDS_LOAD_ALIGN == 4096, "ABI: load alignment");
_Static_assert(sizeof(bmds_program_note_t) == 16, "ABI: bmds_program_note_t size");
_Static_assert(offsetof(bmds_program_note_t, size) == 0, "ABI: note.size");
_Static_assert(offsetof(bmds_program_note_t, abi_version) == 4, "ABI: note.abi_version");
_Static_assert(offsetof(bmds_program_note_t, stack_size) == 8, "ABI: note.stack_size");
_Static_assert(offsetof(bmds_program_note_t, flags) == 12, "ABI: note.flags");
_Static_assert(BMDS_NOTE_ICON == 2 && BMDS_ICON_FORMAT_PNG == 1, "ABI: icon note type and format");
_Static_assert(sizeof(bmds_icon_note_t) == 16, "ABI: bmds_icon_note_t size");
_Static_assert(offsetof(bmds_icon_note_t, size) == 0, "ABI: icon.size");
_Static_assert(offsetof(bmds_icon_note_t, width) == 4, "ABI: icon.width");
_Static_assert(offsetof(bmds_icon_note_t, height) == 6, "ABI: icon.height");
_Static_assert(offsetof(bmds_icon_note_t, format) == 8, "ABI: icon.format");
_Static_assert(offsetof(bmds_icon_note_t, length) == 12, "ABI: icon.length");

// --- bmds/errno.h ---------------------------------------------------------

_Static_assert(BMDS_EPERM == 1 && BMDS_ENOENT == 2 && BMDS_ESRCH == 3 && BMDS_EINTR == 4 && BMDS_EIO == 5,
               "ABI: errno 1-5");
_Static_assert(BMDS_E2BIG == 7 && BMDS_ENOEXEC == 8 && BMDS_EBADF == 9 && BMDS_ECHILD == 10, "ABI: errno 7-10");
_Static_assert(BMDS_EAGAIN == 11 && BMDS_ENOMEM == 12, "ABI: errno 11-12");
_Static_assert(BMDS_EACCES == 13 && BMDS_EFAULT == 14 && BMDS_EBUSY == 16 && BMDS_EEXIST == 17, "ABI: errno 13-17");
_Static_assert(BMDS_EXDEV == 18 && BMDS_ENODEV == 19 && BMDS_ENOTDIR == 20 && BMDS_EISDIR == 21, "ABI: errno 18-21");
_Static_assert(BMDS_EINVAL == 22 && BMDS_ENFILE == 23 && BMDS_EMFILE == 24 && BMDS_ENOTTY == 25, "ABI: errno 22-25");
_Static_assert(BMDS_EFBIG == 27 && BMDS_ENOSPC == 28 && BMDS_ESPIPE == 29 && BMDS_EROFS == 30, "ABI: errno 27-30");
_Static_assert(BMDS_EPIPE == 32 && BMDS_ERANGE == 34 && BMDS_ENAMETOOLONG == 36 && BMDS_ENOSYS == 38,
               "ABI: errno 32-38");
_Static_assert(BMDS_ENOTEMPTY == 39 && BMDS_ELOOP == 40 && BMDS_EOVERFLOW == 75 && BMDS_EILSEQ == 84,
               "ABI: errno 39-84");
_Static_assert(BMDS_ENOTSUP == 95 && BMDS_ETIMEDOUT == 110 && BMDS_ENOMEDIUM == 123, "ABI: errno 95-123");
_Static_assert(BMDS_ECANCELED == 125, "ABI: errno 125 (Linux's values throughout)");
_Static_assert(BMDS_ENOTSOCK == 88 && BMDS_EDESTADDRREQ == 89 && BMDS_EMSGSIZE == 90 && BMDS_EPROTOTYPE == 91,
               "ABI: errno 88-91");
_Static_assert(BMDS_ENOPROTOOPT == 92 && BMDS_EPROTONOSUPPORT == 93 && BMDS_EAFNOSUPPORT == 97, "ABI: errno 92-97");
_Static_assert(BMDS_EADDRINUSE == 98 && BMDS_EADDRNOTAVAIL == 99 && BMDS_ENETDOWN == 100 && BMDS_ENETUNREACH == 101,
               "ABI: errno 98-101");
_Static_assert(BMDS_ECONNABORTED == 103 && BMDS_ECONNRESET == 104 && BMDS_ENOBUFS == 105 && BMDS_EISCONN == 106,
               "ABI: errno 103-106");
_Static_assert(BMDS_ENOTCONN == 107 && BMDS_ECONNREFUSED == 111 && BMDS_EHOSTUNREACH == 113, "ABI: errno 107-113");
_Static_assert(BMDS_EALREADY == 114 && BMDS_EINPROGRESS == 115, "ABI: errno 114-115");

/* PTP clocks and TAI (devplan/network.md §31e). */
_Static_assert(BMDS_CLOCK_TAI == 2, "ABI: clock TAI");
_Static_assert(BMDS_PTP_TIME == 0 && BMDS_PTP_CAPS == 1 && BMDS_PTP_OFFSET == 2 && BMDS_PTP_SET == 3 &&
                       BMDS_PTP_STEP == 4 && BMDS_PTP_GET_FREQ == 5 && BMDS_PTP_SET_FREQ == 6 &&
                       BMDS_PTP_CAP_PPS == 1 && BMDS_PTP_MAX_SAMPLES == 25,
               "ABI: ptp_ctl");
_Static_assert(offsetof(bmds_ptp_caps_t, size) == 0 && offsetof(bmds_ptp_caps_t, max_adj_ppb) == 4 &&
                       offsetof(bmds_ptp_caps_t, flags) == 8 && offsetof(bmds_ptp_caps_t, pps_pin) == 12 &&
                       offsetof(bmds_ptp_caps_t, index) == 16 && sizeof(bmds_ptp_caps_t) == 24,
               "ABI: bmds_ptp_caps_t");
_Static_assert(offsetof(bmds_ptp_offset_t, clock) == 4 && offsetof(bmds_ptp_offset_t, count) == 8 &&
                       offsetof(bmds_ptp_offset_t, samples) == 16 && sizeof(bmds_ptp_offset_t) == 16 + 25 * 24,
               "ABI: bmds_ptp_offset_t");

/* Surfaces (devplan/games.md §11). */
_Static_assert(BMDS_PIXEL_L8 == 5, "ABI: pixel format L8");
_Static_assert(BMDS_SURFACE_FULL_SCREEN == 1 && BMDS_SURFACE_WHOLE == 2 && BMDS_SURFACE_FAST == 4,
               "ABI: surface flags");
_Static_assert(BMDS_SURFACE_PALETTE == 1 && BMDS_SURFACE_TITLE == 2 && BMDS_SURFACE_POINTER == 3 &&
                       BMDS_SURFACE_WAIT == 4 && BMDS_SURFACE_INFO == 5,
               "ABI: surface_control ops");
_Static_assert(BMDS_POINTER_NORMAL == 0 && BMDS_POINTER_HIDDEN == 1 && BMDS_POINTER_RELATIVE == 2,
               "ABI: pointer modes");
_Static_assert(BMDS_EVENT_KEY == 1 && BMDS_EVENT_POINTER == 2 && BMDS_EVENT_WHEEL == 3 && BMDS_EVENT_ACTIVE == 4 &&
                       BMDS_EVENT_PRESENTED == 5 && BMDS_EVENT_CLOSE == 6 && BMDS_EVENT_ACTION == 7 &&
                       BMDS_EVENT_PAD == 8,
               "ABI: event types");
_Static_assert(BMDS_KEY_DOWN == 1 && BMDS_KEY_UP == 2 && BMDS_KEY_REPEAT == 4, "ABI: key flags");
#if UINTPTR_MAX == 0xffffffffu // (pointers inside)
_Static_assert(offsetof(bmds_surface_config_t, width) == 4 && offsetof(bmds_surface_config_t, height) == 8 &&
                       offsetof(bmds_surface_config_t, format) == 12 &&
                       offsetof(bmds_surface_config_t, buffers) == 16 && offsetof(bmds_surface_config_t, flags) == 20 &&
                       offsetof(bmds_surface_config_t, aspect_x) == 24 &&
                       offsetof(bmds_surface_config_t, aspect_y) == 28 &&
                       offsetof(bmds_surface_config_t, title) == 32 && sizeof(bmds_surface_config_t) == 36,
               "ABI: bmds_surface_config_t");
_Static_assert(offsetof(bmds_surface_buffer_t, index) == 4 && offsetof(bmds_surface_buffer_t, pixels) == 8 &&
                       offsetof(bmds_surface_buffer_t, stride) == 12 && sizeof(bmds_surface_buffer_t) == 16,
               "ABI: bmds_surface_buffer_t");
_Static_assert(offsetof(bmds_rect_t, y) == 4 && offsetof(bmds_rect_t, width) == 8 &&
                       offsetof(bmds_rect_t, height) == 12 && sizeof(bmds_rect_t) == 16,
               "ABI: bmds_rect_t");
_Static_assert(offsetof(bmds_surface_present_t, index) == 4 && offsetof(bmds_surface_present_t, damage) == 8 &&
                       offsetof(bmds_surface_present_t, damage_count) == 12 && sizeof(bmds_surface_present_t) == 16,
               "ABI: bmds_surface_present_t");
_Static_assert(offsetof(bmds_surface_palette_t, first) == 4 && offsetof(bmds_surface_palette_t, count) == 8 &&
                       offsetof(bmds_surface_palette_t, rgb) == 12 && sizeof(bmds_surface_palette_t) == 16,
               "ABI: bmds_surface_palette_t");
#endif
_Static_assert(offsetof(bmds_surface_wait_t, frame) == 4 && offsetof(bmds_surface_wait_t, timeout_ms) == 8 &&
                       sizeof(bmds_surface_wait_t) == 12,
               "ABI: bmds_surface_wait_t");
_Static_assert(offsetof(bmds_surface_info_t, full_screen) == 4 && offsetof(bmds_surface_info_t, scale_x16) == 16 &&
                       offsetof(bmds_surface_info_t, x) == 24 && offsetof(bmds_surface_info_t, width) == 32 &&
                       offsetof(bmds_surface_info_t, frames) == 40 && offsetof(bmds_surface_info_t, dropped) == 48 &&
                       sizeof(bmds_surface_info_t) == 52,
               "ABI: bmds_surface_info_t");
_Static_assert(offsetof(bmds_surface_event_t, flags) == 4 && offsetof(bmds_surface_event_t, time_ns) == 8 &&
                       offsetof(bmds_surface_event_t, data) == 16 && sizeof(bmds_surface_event_t) == 32,
               "ABI: bmds_surface_event_t");
