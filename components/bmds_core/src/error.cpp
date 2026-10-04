/**
 * @file      error.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cerrno>
#include <bmds/error.h>
#include <bmds/detail/esperr.h>

BMDS_NAMESPACE_BEGIN

namespace {

        struct CodeText {
                        const char *name;
                        const char *desc;
        };

        // Indexed by Error::Code.
        constexpr CodeText CodeTexts[] = {
                {"Ok", "No error"},
                {"Invalid", "Invalid value or argument"},
                {"NoMem", "Out of memory"},
                {"Timeout", "Operation timed out"},
                {"WouldBlock", "Operation would block"},
                {"Busy", "Resource is busy"},
                {"NotExist", "Does not exist"},
                {"Exists", "Already exists"},
                {"NotSupported", "Operation not supported"},
                {"IoError", "Input/output error"},
                {"NoSpace", "No space left on device"},
                {"Full", "Full"},
                {"Empty", "Empty"},
                {"InvalidState", "Invalid state for operation"},
                {"NotReady", "Not ready"},
                {"OutOfRange", "Out of range"},
                {"BufferTooSmall", "Buffer too small"},
                {"PermissionDenied", "Permission denied"},
                {"NotImplemented", "Not implemented"},
                {"EndOfFile", "End of file"},
                {"Cancelled", "Cancelled"},
                {"CorruptData", "Corrupt data"},
                {"DeviceError", "Device error"},
                {"NotFound", "Not found"},
                {"BadDescriptor", "Bad descriptor"},
                {"TooManyOpen", "Too many open handles"},
                {"IsDir", "Is a directory"},
                {"NotDir", "Not a directory"},
                {"NotEmpty", "Directory not empty"},
                {"ReadOnly", "Read-only"},
                {"Interrupted", "Interrupted"},
                {"LibraryFailure", "Library failure"},
                {"UnsupportedSystemError", "Unmapped system error"},
                {"BrokenPipe", "Broken pipe"},
                {"NoMedia", "No medium"},
                {"NoFilesystem", "No filesystem"},
        };
        static_assert(sizeof(CodeTexts) / sizeof(CodeTexts[0]) == Error::CodeCount,
                      "CodeTexts must have one entry per Error::Code");

        constexpr CodeText UnknownText = {"Unknown", "Unknown error"};

        struct Mapping {
                        int32_t     from;
                        Error::Code to;
        };

        constexpr Mapping EspMap[] = {
                {Detail::EspErr::Ok, Error::Ok},
                {Detail::EspErr::NoMem, Error::NoMem},
                {Detail::EspErr::InvalidArg, Error::Invalid},
                {Detail::EspErr::InvalidState, Error::InvalidState},
                {Detail::EspErr::InvalidSize, Error::Invalid},
                {Detail::EspErr::NotFound, Error::NotExist},
                {Detail::EspErr::NotSupported, Error::NotSupported},
                {Detail::EspErr::Timeout, Error::Timeout},
                {Detail::EspErr::InvalidResponse, Error::DeviceError},
                {Detail::EspErr::InvalidCrc, Error::CorruptData},
                {Detail::EspErr::InvalidVersion, Error::NotSupported},
                {Detail::EspErr::NotFinished, Error::Busy},
                {Detail::EspErr::NotAllowed, Error::PermissionDenied},
        };

        // A table rather than a switch: several errno macros share values on
        // some C libraries (e.g. ENOTSUP == EOPNOTSUPP on glibc but not on
        // newlib), which would be duplicate case labels. First match wins.
        constexpr Mapping ErrnoMap[] = {
                {0, Error::Ok},
                {EINVAL, Error::Invalid},
                {ENOMEM, Error::NoMem},
                {ETIMEDOUT, Error::Timeout},
                {EAGAIN, Error::WouldBlock},
                {EWOULDBLOCK, Error::WouldBlock},
                {EBUSY, Error::Busy},
                {ENOENT, Error::NotExist},
                {EEXIST, Error::Exists},
                {ENOTSUP, Error::NotSupported},
                {EOPNOTSUPP, Error::NotSupported},
                {ENOSYS, Error::NotImplemented},
                {EIO, Error::IoError},
                {ENOSPC, Error::NoSpace},
                {ERANGE, Error::OutOfRange},
                {EACCES, Error::PermissionDenied},
                {EPERM, Error::PermissionDenied},
                {EBADF, Error::BadDescriptor},
                {EMFILE, Error::TooManyOpen},
                {ENFILE, Error::TooManyOpen},
                {EISDIR, Error::IsDir},
                {ENOTDIR, Error::NotDir},
                {ENOTEMPTY, Error::NotEmpty},
                {EROFS, Error::ReadOnly},
                {EINTR, Error::Interrupted},
                {EPIPE, Error::BrokenPipe},
                {ECANCELED, Error::Cancelled},
                {ENODEV, Error::NotExist},
                {ENXIO, Error::NotExist},
        };

        template <typename Table> Error lookup(const Table &table, int32_t value, Error::Code fallback) {
                for (const Mapping &m : table) {
                        if (m.from == value) return m.to;
                }
                return fallback;
        }

        const CodeText &textFor(Error::Code code) {
                return code < Error::CodeCount ? CodeTexts[code] : UnknownText;
        }

} // namespace

Error Error::fromEspErr(int32_t espErr) {
        return lookup(EspMap, espErr, LibraryFailure);
}

Error Error::syserr() {
        return syserr(errno);
}

Error Error::syserr(int errnum) {
        return lookup(ErrnoMap, errnum, UnsupportedSystemError);
}

const char *Error::name() const {
        return textFor(_code).name;
}

const char *Error::desc() const {
        return textFor(_code).desc;
}

BMDS_NAMESPACE_END
