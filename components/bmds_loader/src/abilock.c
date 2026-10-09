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
_Static_assert(BMDS_SYSCONF_KEY_MAX == 63 && BMDS_SYSCONF_VALUE_MAX == 1024, "ABI: sysconf lengths");
_Static_assert(BMDS_SYSCONF_TYPE_BOOL == 0 && BMDS_SYSCONF_TYPE_INT == 1 && BMDS_SYSCONF_TYPE_STRING == 2 &&
                       BMDS_SYSCONF_TYPE_COLOR == 3 && BMDS_SYSCONF_TYPE_ENUM == 4 && BMDS_SYSCONF_TYPE_PATH == 5 &&
                       BMDS_SYSCONF_TYPE_PATHS == 6,
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
_Static_assert(BMDS_EPIPE == 32 && BMDS_ERANGE == 34 && BMDS_ENAMETOOLONG == 36 && BMDS_ENOSYS == 38, "ABI: errno 32-38");
_Static_assert(BMDS_ENOTEMPTY == 39 && BMDS_ELOOP == 40 && BMDS_EOVERFLOW == 75 && BMDS_EILSEQ == 84, "ABI: errno 39-84");
_Static_assert(BMDS_ENOTSUP == 95 && BMDS_ETIMEDOUT == 110 && BMDS_ENOMEDIUM == 123, "ABI: errno 95-123");
_Static_assert(BMDS_ECANCELED == 125, "ABI: errno 125 (Linux's values throughout)");
