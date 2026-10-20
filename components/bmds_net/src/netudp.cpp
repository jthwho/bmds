/**
 * @file      netudp.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A kernel service's UDP port on lwIP's raw API (bmds/netudp.h).
 */

#include <cstring>
#include <lwip/pbuf.h>
#include <lwip/udp.h>
#include <bmds/memory.h>
#include <bmds/netudp.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t MaxDatagram = 1500; // a datagram in pieces is joined up to this

} // namespace

struct NetUdpCallbacks {
                // Network task, core lock held. Most datagrams are one piece, given in place.
                static void received(void *arg, struct udp_pcb *, struct pbuf *p, const ip_addr_t *from, u16_t port) {
                        auto *self = static_cast<NetUdp *>(arg);
                        if (p->len == p->tot_len) {
                                self->_receive(self->_arg, ip_addr_get_ip4_u32(from), port,
                                               ByteSpan(static_cast<const uint8_t *>(p->payload), p->len));
                        } else {
                                if (self->_scratch == nullptr)
                                        self->_scratch =
                                                static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, MaxDatagram));
                                if (self->_scratch != nullptr) {
                                        const u16_t n = pbuf_copy_partial(p, self->_scratch, MaxDatagram, 0);
                                        self->_receive(self->_arg, ip_addr_get_ip4_u32(from), port,
                                                       ByteSpan(self->_scratch, n));
                                }
                        }
                        (void)pbuf_free(p);
                        return;
                }
};

Error NetUdp::open(uint16_t port, Receive receive, void *arg) {
        if (_pcb != nullptr) return Error::InvalidState;
        struct udp_pcb *pcb = udp_new();
        if (pcb == nullptr) return Error::NoMem;
        const err_t err = udp_bind(pcb, IP_ANY_TYPE, port);
        if (err != ERR_OK) {
                udp_remove(pcb);
                return err == ERR_USE ? Error::Exists : Error::NoMem;
        }
        _receive = receive;
        _arg = arg;
        udp_recv(pcb, &NetUdpCallbacks::received, this);
        _pcb = pcb;
        return Error::Ok;
}

void NetUdp::close() {
        if (_pcb != nullptr) udp_remove(static_cast<struct udp_pcb *>(_pcb));
        _pcb = nullptr;
        Memory::free(_scratch);
        _scratch = nullptr;
        return;
}

Error NetUdp::sendTo(uint32_t address, uint16_t port, ByteSpan data) {
        if (_pcb == nullptr) return Error::InvalidState;
        struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, u16_t(data.size()), PBUF_RAM);
        if (p == nullptr) return Error::NoMem;
        std::memcpy(p->payload, data.data(), data.size());
        ip_addr_t to;
        ip_addr_set_ip4_u32(&to, address);
        const err_t err = udp_sendto(static_cast<struct udp_pcb *>(_pcb), p, &to, port);
        (void)pbuf_free(p);
        if (err == ERR_RTE) return Error::NetworkUnreachable;
        return err == ERR_OK ? Error::Ok : Error::NoMem;
}

BMDS_NAMESPACE_END
