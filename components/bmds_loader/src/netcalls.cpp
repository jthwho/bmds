/**
 * @file      netcalls.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Sockets' system calls (devplan/network.md §18): Linux's address layouts,
 * option numbers and flags turned into the kernel's Socket, and the
 * interfaces' state (net_info).
 */

#include <cstring>
#include <bmds/errno.h>
#include <bmds/netstatus.h>
#include <bmds/process.h>
#include <bmds/socket.h>
#include <bmds/sys.h>
#include "running.h"
#include "structcopy.h"

using bmds::ByteSpan;
using bmds::Error;
using bmds::FileHandle;
using bmds::KernelCall;
using bmds::Socket;
using bmds::SocketAddress;
using bmds::Span;
using bmds::StringView;
using bmds::WritableByteSpan;
namespace NetStatus = bmds::NetStatus;
using Option = Socket::Option;

namespace {

        // Linux's numbers, as musl's headers have them.
        constexpr int32_t SolSocket = 1;
        constexpr int32_t IpprotoIp = 0;
        constexpr int32_t IpprotoTcp = 6;

        enum : int32_t {
                SoReuseAddr = 2,
                SoType = 3,
                SoError = 4,
                SoBroadcast = 6,
                SoSndBuf = 7,
                SoRcvBuf = 8,
                SoKeepAlive = 9,
                SoLinger = 13,
                SoRcvTimeoOld = 20,
                SoSndTimeoOld = 21,
                SoBindToDevice = 25,
                SoAcceptConn = 30,
                SoProtocol = 38,
                SoDomain = 39,
                SoRcvTimeoNew = 66,
                SoSndTimeoNew = 67,
        };

        enum : int32_t {
                IpTos = 1,
                IpTtl = 2,
                IpMulticastIf = 32,
                IpMulticastTtl = 33,
                IpMulticastLoop = 34,
                IpAddMembership = 35,
                IpDropMembership = 36,
        };

        enum : int32_t {
                TcpNoDelay = 1,
                TcpMaxSeg = 2,
                TcpKeepIdle = 4,
                TcpKeepIntvl = 5,
                TcpKeepCnt = 6,
        };

        constexpr uint32_t MsgOob = 0x1;
        constexpr uint32_t MsgErrQueue = 0x2000;
        constexpr uint32_t IfNameBytes = 16; // IFNAMSIZ

        uint16_t swap16(uint16_t v) {
                return static_cast<uint16_t>((v >> 8) | (v << 8));
        }

        // The socket behind @p fd, and the handle that keeps it while the call runs.
        struct Target {
                        FileHandle::Ptr handle;
                        Socket         *socket = nullptr;
        };

        int32_t find(KernelCall &call, int32_t fd, Target &t) {
                t.handle = call->handle(fd);
                if (!t.handle) return -BMDS_EBADF;
                t.socket = t.handle->stream().socket();
                return t.socket != nullptr ? 0 : -BMDS_ENOTSOCK;
        }

        int32_t errnoOf(Error err) {
                return bmds::Process::errnoFor(err);
        }

        // An AF_INET address from the program (AF_UNSPEC: zero, where @p unspec allows it).
        int32_t addressIn(const void *addr, uint32_t length, SocketAddress &out, bool unspec) {
                if (addr == nullptr) return -BMDS_EFAULT;
                if (length < sizeof(uint16_t)) return -BMDS_EINVAL;
                uint16_t family;
                std::memcpy(&family, addr, sizeof(family));
                if (family == BMDS_AF_UNSPEC && unspec) {
                        out = SocketAddress{};
                        return 0;
                }
                if (family != BMDS_AF_INET) return -BMDS_EAFNOSUPPORT;
                if (length < sizeof(bmds_sockaddr_in_t)) return -BMDS_EINVAL;
                bmds_sockaddr_in_t in;
                std::memcpy(&in, addr, sizeof(in));
                out.ipv4 = in.addr;
                out.port = swap16(in.port);
                return 0;
        }

        // As much of the address as `*length` bytes hold; `*length` its whole length (Linux's way).
        int32_t addressOut(const SocketAddress &a, void *addr, uint32_t *length) {
                if (addr == nullptr || length == nullptr) return -BMDS_EFAULT;
                bmds_sockaddr_in_t out = {};
                out.family = BMDS_AF_INET;
                out.port = swap16(a.port);
                out.addr = a.ipv4;
                const uint32_t n = *length < sizeof(out) ? *length : sizeof(out);
                std::memcpy(addr, &out, n);
                *length = sizeof(out);
                return 0;
        }

        // An int option's value: four bytes, or one (the multicast options take a byte too).
        int32_t intIn(const void *value, uint32_t length, int32_t &out) {
                if (value == nullptr) return -BMDS_EFAULT;
                if (length >= sizeof(int32_t)) {
                        std::memcpy(&out, value, sizeof(out));
                        return 0;
                }
                if (length == 1) {
                        out = *static_cast<const uint8_t *>(value);
                        return 0;
                }
                return -BMDS_EINVAL;
        }

        int32_t intOut(int32_t v, void *value, uint32_t *length) {
                if (value == nullptr || length == nullptr) return -BMDS_EFAULT;
                if (*length < sizeof(int32_t)) return -BMDS_EINVAL;
                std::memcpy(value, &v, sizeof(v));
                *length = sizeof(v);
                return 0;
        }

        // SO_RCVTIMEO and SO_SNDTIMEO: a timeval of two 64-bit fields (the new ones) or 32-bit.
        int32_t timeoutIn(bool wide, const void *value, uint32_t length, int32_t &ms) {
                if (value == nullptr) return -BMDS_EFAULT;
                int64_t sec;
                int64_t usec;
                if (wide) {
                        int64_t tv[2];
                        if (length < sizeof(tv)) return -BMDS_EINVAL;
                        std::memcpy(tv, value, sizeof(tv));
                        sec = tv[0];
                        usec = tv[1];
                } else {
                        int32_t tv[2];
                        if (length < sizeof(tv)) return -BMDS_EINVAL;
                        std::memcpy(tv, value, sizeof(tv));
                        sec = tv[0];
                        usec = tv[1];
                }
                if (usec < 0 || usec >= 1'000'000) return -BMDS_EINVAL;
                if (sec < 0) sec = usec = 0; // Linux: never times out
                const int64_t total = sec * 1000 + (usec + 999) / 1000;
                ms = total > INT32_MAX ? INT32_MAX : static_cast<int32_t>(total);
                return 0;
        }

        int32_t timeoutOut(bool wide, int32_t ms, void *value, uint32_t *length) {
                if (value == nullptr || length == nullptr) return -BMDS_EFAULT;
                if (wide) {
                        const int64_t tv[2] = {ms / 1000, (ms % 1000) * 1000};
                        if (*length < sizeof(tv)) return -BMDS_EINVAL;
                        std::memcpy(value, tv, sizeof(tv));
                        *length = sizeof(tv);
                } else {
                        const int32_t tv[2] = {ms / 1000, (ms % 1000) * 1000};
                        if (*length < sizeof(tv)) return -BMDS_EINVAL;
                        std::memcpy(value, tv, sizeof(tv));
                        *length = sizeof(tv);
                }
                return 0;
        }

        // The IPv4 address of the interface with index @p index, or 0.
        uint32_t addressOfIndex(int32_t index) {
                NetStatus::Info info;
                for (size_t i = 0; NetStatus::info(i, info).isOk(); ++i) {
                        if (static_cast<int32_t>(info.index) == index) return info.address;
                }
                return 0;
        }

        // ip_mreq (group, interface) or ip_mreqn (group, interface, index).
        int32_t membershipIn(const void *value, uint32_t length, uint32_t &group, uint32_t &interface) {
                if (value == nullptr) return -BMDS_EFAULT;
                uint32_t m[3] = {};
                if (length < 2 * sizeof(uint32_t)) return -BMDS_EINVAL;
                std::memcpy(m, value, length >= sizeof(m) ? sizeof(m) : 2 * sizeof(uint32_t));
                group = m[0];
                interface = m[1];
                if (interface == 0 && length >= sizeof(m) && m[2] != 0)
                        interface = addressOfIndex(static_cast<int32_t>(m[2]));
                return 0;
        }

        int32_t optionErrno(Error err) {
                switch (err.code()) {
                        case Error::NotSupported: return -BMDS_ENOPROTOOPT;
                        case Error::Exists: return -BMDS_EADDRINUSE;
                        case Error::Full: return -BMDS_ENOBUFS;
                        case Error::NotExist: return -BMDS_EADDRNOTAVAIL;
                        default: return errnoOf(err);
                }
        }

        int32_t setOption(Socket &s, Option option, const void *value, uint32_t length) {
                int32_t v;
                if (const int32_t r = intIn(value, length, v); r < 0) return r;
                return optionErrno(s.setOption(option, v));
        }

        int32_t getOption(Socket &s, Option option, void *value, uint32_t *length) {
                auto [v, err] = s.option(option);
                if (err.isError()) return optionErrno(err);
                return intOut(v, value, length);
        }

        // The pieces of @p m (at most BMDS_IOV_MAX), their total held to what a result can say.
        template <typename Piece, typename Base>
        int32_t piecesOf(const bmds_msghdr_t &m, Piece (&pieces)[BMDS_IOV_MAX], size_t &count) {
                if (m.iov_count > BMDS_IOV_MAX) return -BMDS_EMSGSIZE;
                if (m.iov_count > 0 && m.iov == nullptr) return -BMDS_EFAULT;
                uint64_t total = 0;
                count = 0;
                for (uint32_t i = 0; i < m.iov_count; ++i) {
                        uint32_t n = m.iov[i].length;
                        if (n == 0) continue;
                        if (m.iov[i].base == nullptr) return -BMDS_EFAULT;
                        if (total + n > INT32_MAX) n = static_cast<uint32_t>(INT32_MAX - total);
                        total += n;
                        pieces[count++] = Piece(static_cast<Base *>(m.iov[i].base), n);
                        if (total == INT32_MAX) break;
                }
                return 0;
        }

} // namespace

extern "C" {

int32_t bmds_k_socket(uint32_t family, uint32_t type, uint32_t protocol, uint32_t flags) {
        KernelCall call;
        if ((flags & ~BMDS_SOCK_CLOEXEC) != 0) return -BMDS_EINVAL;
        if (family != BMDS_AF_INET) return -BMDS_EAFNOSUPPORT;
        Socket::Type t;
        switch (type) {
                case BMDS_SOCK_STREAM: t = Socket::Type::Stream; break;
                case BMDS_SOCK_DGRAM: t = Socket::Type::Datagram; break;
                case BMDS_SOCK_RAW: t = Socket::Type::Raw; break;
                default: return -BMDS_EINVAL;
        }
        if (protocol > 255) return -BMDS_EPROTONOSUPPORT;
        auto [handle, err] = Socket::create(t, static_cast<uint8_t>(protocol));
        if (err == Error::NotSupported) return -BMDS_EPROTONOSUPPORT;
        if (err.isError()) return errnoOf(err);
        return call->addHandle(std::move(handle), flags & BMDS_SOCK_CLOEXEC);
}

int32_t bmds_k_bind(int32_t fd, const void *addr, uint32_t length) {
        KernelCall    call;
        Target        t;
        SocketAddress a;
        if (const int32_t r = find(call, fd, t); r < 0) return r;
        if (const int32_t r = addressIn(addr, length, a, false); r < 0) return r;
        return errnoOf(t.socket->bind(a));
}

int32_t bmds_k_listen(int32_t fd, int32_t backlog) {
        KernelCall call;
        Target     t;
        if (const int32_t r = find(call, fd, t); r < 0) return r;
        return errnoOf(t.socket->listen(backlog));
}

int32_t bmds_k_accept(int32_t fd, void *addr, uint32_t *length, uint32_t flags) {
        KernelCall call;
        Target     t;
        if ((flags & ~(BMDS_SOCK_CLOEXEC | BMDS_SOCK_DONTWAIT)) != 0) return -BMDS_EINVAL;
        if (const int32_t r = find(call, fd, t); r < 0) return r;
        if (addr != nullptr && length == nullptr) return -BMDS_EFAULT;
        SocketAddress peer;
        auto [handle, err] = t.socket->accept(&peer, (flags & BMDS_SOCK_DONTWAIT) ? uint32_t(Socket::DontWait) : 0u);
        if (err == Error::WouldBlock) return -BMDS_EAGAIN;
        if (err.isError()) return errnoOf(err);
        const int32_t newFd = call->addHandle(std::move(handle), flags & BMDS_SOCK_CLOEXEC);
        if (newFd >= 0 && addr != nullptr) (void)addressOut(peer, addr, length);
        return newFd;
}

int32_t bmds_k_connect(int32_t fd, const void *addr, uint32_t length, uint32_t flags) {
        KernelCall    call;
        Target        t;
        SocketAddress a;
        if ((flags & ~BMDS_SOCK_DONTWAIT) != 0) return -BMDS_EINVAL;
        if (const int32_t r = find(call, fd, t); r < 0) return r;
        if (const int32_t r = addressIn(addr, length, a, true); r < 0) return r;
        return errnoOf(t.socket->connect(a, (flags & BMDS_SOCK_DONTWAIT) ? uint32_t(Socket::DontWait) : 0u));
}

int32_t bmds_k_sock_name(int32_t fd, uint32_t which, void *addr, uint32_t *length) {
        KernelCall call;
        Target     t;
        if (const int32_t r = find(call, fd, t); r < 0) return r;
        switch (which) {
                case BMDS_SOCK_NAME_LOCAL: return addressOut(t.socket->address(), addr, length);
                case BMDS_SOCK_NAME_PEER: {
                        auto [peer, err] = t.socket->peer();
                        if (err.isError()) return errnoOf(err);
                        return addressOut(peer, addr, length);
                }
                default: return -BMDS_EINVAL;
        }
}

int32_t bmds_k_sendmsg(int32_t fd, const bmds_msghdr_t *msg, uint32_t flags) {
        KernelCall    call;
        Target        t;
        bmds_msghdr_t m;
        if (const int32_t r = bmds::detail::copyIn(msg, m); r < 0) return r;
        if (const int32_t r = find(call, fd, t); r < 0) return r;
        if (flags & MsgOob) return -BMDS_ENOTSUP;
        ByteSpan pieces[BMDS_IOV_MAX];
        size_t   count = 0;
        if (const int32_t r = piecesOf<ByteSpan, const uint8_t>(m, pieces, count); r < 0) return r;
        SocketAddress  to;
        SocketAddress *toPtr = nullptr;
        if (m.name != nullptr && m.name_length > 0) {
                if (const int32_t r = addressIn(m.name, m.name_length, to, false); r < 0) return r;
                toPtr = &to;
        }
        uint32_t f = 0;
        if (flags & BMDS_MSG_DONTWAIT) f |= Socket::DontWait;
        if (flags & BMDS_MSG_MORE) f |= Socket::More;
        auto [n, err] = t.socket->send(Span<const ByteSpan>(pieces, count), toPtr, f);
        if (err.isError() && n == 0) {
                if (err == Error::BrokenPipe && (flags & BMDS_MSG_NOSIGNAL) == 0) {
                        bmds::Signals &s = call->signals();
                        (void)s.postTo(s.current(), BMDS_SIGPIPE, BMDS_SI_KERNEL);
                }
                if (err == Error::NotConnected && t.socket->type() != Socket::Type::Stream) return -BMDS_EDESTADDRREQ;
                return errnoOf(err);
        }
        return static_cast<int32_t>(n);
}

int32_t bmds_k_recvmsg(int32_t fd, bmds_msghdr_t *msg, uint32_t flags) {
        KernelCall    call;
        Target        t;
        bmds_msghdr_t m;
        if (const int32_t r = bmds::detail::copyIn(msg, m); r < 0) return r;
        if (const int32_t r = find(call, fd, t); r < 0) return r;
        if (flags & MsgOob) return -BMDS_EINVAL;
        if (flags & MsgErrQueue) return -BMDS_EAGAIN; // no error queue
        WritableByteSpan pieces[BMDS_IOV_MAX];
        size_t           count = 0;
        if (const int32_t r = piecesOf<WritableByteSpan, uint8_t>(m, pieces, count); r < 0) return r;
        uint32_t f = 0;
        if (flags & BMDS_MSG_DONTWAIT) f |= Socket::DontWait;
        if (flags & BMDS_MSG_PEEK) f |= Socket::Peek;
        if (flags & BMDS_MSG_WAITALL) f |= Socket::WaitAll;
        auto [got, err] = t.socket->receive(Span<const WritableByteSpan>(pieces, count), f);
        const bool datagram = t.socket->type() != Socket::Type::Stream;
        if (err == Error::EndOfFile) {
                got = Socket::Received{};
        } else if (err.isError()) {
                return errnoOf(err);
        }
        if (m.name != nullptr && datagram && err.isOk()) {
                (void)addressOut(got.from, m.name, &m.name_length);
        } else {
                m.name_length = 0;
        }
        m.control_length = 0;
        m.flags = got.length > got.bytes ? BMDS_MSG_TRUNC : 0;
        if (const int32_t r = bmds::detail::copyOut(m, msg); r < 0) return r;
        const size_t n = datagram && (flags & BMDS_MSG_TRUNC) ? got.length : got.bytes;
        return static_cast<int32_t>(n);
}

int32_t bmds_k_setsockopt(int32_t fd, int32_t level, int32_t name, const void *value, uint32_t length) {
        KernelCall call;
        Target     t;
        if (const int32_t r = find(call, fd, t); r < 0) return r;
        Socket &s = *t.socket;
        if (level == SolSocket) {
                switch (name) {
                        case SoReuseAddr: return setOption(s, Option::ReuseAddress, value, length);
                        case SoKeepAlive: return setOption(s, Option::KeepAlive, value, length);
                        case SoBroadcast: return setOption(s, Option::Broadcast, value, length);
                        case SoRcvBuf: return setOption(s, Option::ReceiveBuffer, value, length);
                        case SoSndBuf: return setOption(s, Option::SendBuffer, value, length);
                        case SoRcvTimeoOld:
                        case SoRcvTimeoNew:
                        case SoSndTimeoOld:
                        case SoSndTimeoNew: {
                                int32_t    ms;
                                const bool wide = name == SoRcvTimeoNew || name == SoSndTimeoNew;
                                if (const int32_t r = timeoutIn(wide, value, length, ms); r < 0) return r;
                                const bool receive = name == SoRcvTimeoOld || name == SoRcvTimeoNew;
                                return optionErrno(
                                        s.setOption(receive ? Option::ReceiveTimeoutMs : Option::SendTimeoutMs, ms));
                        }
                        case SoLinger: {
                                int32_t l[2];
                                if (value == nullptr) return -BMDS_EFAULT;
                                if (length < sizeof(l)) return -BMDS_EINVAL;
                                std::memcpy(l, value, sizeof(l));
                                return optionErrno(s.setOption(Option::Linger, l[0] != 0 ? (l[1] < 0 ? 0 : l[1]) : -1));
                        }
                        case SoBindToDevice: {
                                if (value == nullptr && length > 0) return -BMDS_EFAULT;
                                const uint32_t n = length < IfNameBytes ? length : IfNameBytes;
                                const auto    *text = static_cast<const char *>(value);
                                const Error    err = s.bindToDevice(StringView(text, n > 0 ? strnlen(text, n) : 0));
                                return err == Error::NotExist ? -BMDS_ENODEV : errnoOf(err);
                        }
                        default: return -BMDS_ENOPROTOOPT;
                }
        }
        if (level == IpprotoIp) {
                switch (name) {
                        case IpTtl: return setOption(s, Option::Ttl, value, length);
                        case IpTos: return setOption(s, Option::Tos, value, length);
                        case IpMulticastTtl: return setOption(s, Option::MulticastTtl, value, length);
                        case IpMulticastLoop: return setOption(s, Option::MulticastLoop, value, length);
                        case IpMulticastIf: {
                                if (value == nullptr) return -BMDS_EFAULT;
                                uint32_t interface;
                                if (length >= 3 * sizeof(uint32_t)) {
                                        uint32_t group;
                                        if (const int32_t r = membershipIn(value, length, group, interface); r < 0)
                                                return r;
                                } else if (length >= sizeof(uint32_t)) {
                                        std::memcpy(&interface, value, sizeof(interface));
                                } else {
                                        return -BMDS_EINVAL;
                                }
                                return optionErrno(s.setMulticastInterface(interface));
                        }
                        case IpAddMembership:
                        case IpDropMembership: {
                                uint32_t group;
                                uint32_t interface;
                                if (const int32_t r = membershipIn(value, length, group, interface); r < 0) return r;
                                return optionErrno(name == IpAddMembership ? s.joinGroup(group, interface)
                                                                           : s.leaveGroup(group, interface));
                        }
                        default: return -BMDS_ENOPROTOOPT;
                }
        }
        if (level == IpprotoTcp) {
                switch (name) {
                        case TcpNoDelay: return setOption(s, Option::NoDelay, value, length);
                        case TcpKeepIdle: return setOption(s, Option::KeepIdle, value, length);
                        case TcpKeepIntvl: return setOption(s, Option::KeepInterval, value, length);
                        case TcpKeepCnt: return setOption(s, Option::KeepCount, value, length);
                        case TcpMaxSeg: return s.type() == Socket::Type::Stream ? 0 : -BMDS_ENOPROTOOPT; // kept
                        default: return -BMDS_ENOPROTOOPT;
                }
        }
        return -BMDS_ENOPROTOOPT;
}

int32_t bmds_k_getsockopt(int32_t fd, int32_t level, int32_t name, void *value, uint32_t *length) {
        KernelCall call;
        Target     t;
        if (const int32_t r = find(call, fd, t); r < 0) return r;
        Socket &s = *t.socket;
        if (level == SolSocket) {
                switch (name) {
                        case SoReuseAddr: return getOption(s, Option::ReuseAddress, value, length);
                        case SoKeepAlive: return getOption(s, Option::KeepAlive, value, length);
                        case SoBroadcast: return getOption(s, Option::Broadcast, value, length);
                        case SoRcvBuf: return getOption(s, Option::ReceiveBuffer, value, length);
                        case SoSndBuf: return getOption(s, Option::SendBuffer, value, length);
                        case SoAcceptConn: return getOption(s, Option::Listening, value, length);
                        case SoError: return intOut(-errnoOf(s.takeError()), value, length);
                        case SoType: {
                                const Socket::Type type = s.type();
                                return intOut(type == Socket::Type::Stream     ? BMDS_SOCK_STREAM
                                              : type == Socket::Type::Datagram ? BMDS_SOCK_DGRAM
                                                                               : BMDS_SOCK_RAW,
                                              value, length);
                        }
                        case SoDomain: return intOut(BMDS_AF_INET, value, length);
                        case SoProtocol: return intOut(s.protocol(), value, length);
                        case SoRcvTimeoOld:
                        case SoRcvTimeoNew:
                        case SoSndTimeoOld:
                        case SoSndTimeoNew: {
                                const bool receive = name == SoRcvTimeoOld || name == SoRcvTimeoNew;
                                auto [ms, err] = s.option(receive ? Option::ReceiveTimeoutMs : Option::SendTimeoutMs);
                                if (err.isError()) return optionErrno(err);
                                return timeoutOut(name == SoRcvTimeoNew || name == SoSndTimeoNew, ms, value, length);
                        }
                        case SoLinger: {
                                auto [l, err] = s.option(Option::Linger);
                                if (err.isError()) return optionErrno(err);
                                const int32_t out[2] = {l >= 0 ? 1 : 0, l >= 0 ? l : 0};
                                if (value == nullptr || length == nullptr) return -BMDS_EFAULT;
                                if (*length < sizeof(out)) return -BMDS_EINVAL;
                                std::memcpy(value, out, sizeof(out));
                                *length = sizeof(out);
                                return 0;
                        }
                        default: return -BMDS_ENOPROTOOPT;
                }
        }
        if (level == IpprotoIp) {
                switch (name) {
                        case IpTtl: return getOption(s, Option::Ttl, value, length);
                        case IpTos: return getOption(s, Option::Tos, value, length);
                        case IpMulticastTtl: return getOption(s, Option::MulticastTtl, value, length);
                        case IpMulticastLoop: return getOption(s, Option::MulticastLoop, value, length);
                        case IpMulticastIf: {
                                if (s.type() == Socket::Type::Stream) return -BMDS_ENOPROTOOPT;
                                const uint32_t interface = s.multicastInterface();
                                if (value == nullptr || length == nullptr) return -BMDS_EFAULT;
                                if (*length < sizeof(interface)) return -BMDS_EINVAL;
                                std::memcpy(value, &interface, sizeof(interface));
                                *length = sizeof(interface);
                                return 0;
                        }
                        default: return -BMDS_ENOPROTOOPT;
                }
        }
        if (level == IpprotoTcp) {
                switch (name) {
                        case TcpNoDelay: return getOption(s, Option::NoDelay, value, length);
                        case TcpKeepIdle: return getOption(s, Option::KeepIdle, value, length);
                        case TcpKeepIntvl: return getOption(s, Option::KeepInterval, value, length);
                        case TcpKeepCnt: return getOption(s, Option::KeepCount, value, length);
                        case TcpMaxSeg: return getOption(s, Option::MaxSegment, value, length);
                        default: return -BMDS_ENOPROTOOPT;
                }
        }
        return -BMDS_ENOPROTOOPT;
}

int32_t bmds_k_shutdown(int32_t fd, uint32_t how) {
        KernelCall call;
        Target     t;
        if (const int32_t r = find(call, fd, t); r < 0) return r;
        if (how > BMDS_SHUT_RDWR) return -BMDS_EINVAL;
        return errnoOf(t.socket->shutdown(how != BMDS_SHUT_WR, how != BMDS_SHUT_RD));
}

int32_t bmds_k_net_info(uint32_t index, bmds_net_info_t *info) {
        KernelCall      call;
        bmds_net_info_t out;
        if (const int32_t r = bmds::detail::copyIn(info, out); r < 0) return r;
        NetStatus::Info k;
        if (NetStatus::info(index, k).isError()) return -BMDS_ENODEV;
        out = bmds_net_info_t{};
        static_assert(sizeof(out.name) == sizeof(k.name), "the ABI's interface name");
        std::memcpy(out.name, k.name, sizeof(out.name));
        out.index = k.index;
        out.flags = (k.up ? BMDS_NET_UP : 0) | (k.link ? BMDS_NET_LINK : 0) |
                    (k.fullDuplex ? BMDS_NET_FULL_DUPLEX : 0) | (k.loopback ? BMDS_NET_LOOPBACK : 0) |
                    (k.broadcast ? BMDS_NET_BROADCAST : 0) | (k.multicast ? BMDS_NET_MULTICAST : 0);
        out.mtu = k.mtu;
        out.speed_mbps = k.speedMbps;
        std::memcpy(out.mac, k.mac, sizeof(out.mac));
        static_assert(BMDS_NET_MODE_DHCP == uint8_t(NetStatus::Mode::Dhcp) &&
                              BMDS_NET_MODE_STATIC == uint8_t(NetStatus::Mode::Static) &&
                              BMDS_NET_MODE_OFF == uint8_t(NetStatus::Mode::Off),
                      "the ABI's modes");
        static_assert(BMDS_NET_DHCP_NONE == uint8_t(NetStatus::Dhcp::None) &&
                              BMDS_NET_DHCP_ASKING == uint8_t(NetStatus::Dhcp::Asking) &&
                              BMDS_NET_DHCP_BOUND == uint8_t(NetStatus::Dhcp::Bound),
                      "the ABI's DHCP states");
        out.mode = static_cast<uint8_t>(k.mode);
        out.dhcp = static_cast<uint8_t>(k.dhcp);
        out.address = k.address;
        out.netmask = k.netmask;
        out.gateway = k.gateway;
        out.dhcp_server = k.dhcpServer;
        out.lease_s = k.leaseSeconds;
        out.lease_left_s = k.leaseLeftSeconds;
        out.dns[0] = k.dns[0];
        out.dns[1] = k.dns[1];
        out.rx_bytes = k.rxBytes;
        out.tx_bytes = k.txBytes;
        out.rx_frames = k.rxFrames;
        out.tx_frames = k.txFrames;
        out.rx_errors = k.rxErrors;
        out.tx_errors = k.txErrors;
        out.rx_dropped = k.rxDropped;
        out.tx_dropped = k.txDropped;
        return bmds::detail::copyOut(out, info);
}

} // extern "C"
