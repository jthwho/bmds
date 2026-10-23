/**
 * @file      canvassurface.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A canvas's surface (devplan/guiprograms.md §11 3c): the swap chain a local program presents,
 * shown by its canvas.
 */

#include "canvassurface.h"

#include <new>
#include <bmds/guiapplication.h>
#include <bmds/memory.h>
#include <bmds/painter.h>

BMDS_NAMESPACE_BEGIN

Result<FileHandle::Ptr> CanvasSurface::make(uint16_t width, uint16_t height, Format format, uint8_t count,
                                            MemSpace space) {
        if (format != Format::Rgb565 && format != Format::Bgr24) return makeError<FileHandle::Ptr>(Error::Invalid);
        void *memory = Memory::alloc(MemSpace::Psram, sizeof(CanvasSurface));
        if (memory == nullptr) return makeError<FileHandle::Ptr>(Error::NoMem);
        auto *surface = ::new (memory) CanvasSurface();
        Error err = surface->makeBuffers(width, height, format, count, space);
        if (err.isOk()) {
                auto [handle, herr] = FileHandle::open(*surface, FileHandle::Read);
                if (herr.isOk()) return makeResult(std::move(handle));
                err = herr;
        }
        surface->~CanvasSurface();
        Memory::free(surface);
        return makeError<FileHandle::Ptr>(err);
}

CanvasSurface *CanvasSurface::of(FileHandle &handle) {
        // Ours are the only swap chains that aren't program surfaces.
        SwapChain *chain = handle.stream().swapChain();
        return chain != nullptr && handle.stream().programSurface() == nullptr ? static_cast<CanvasSurface *>(chain)
                                                                               : nullptr;
}

void CanvasSurface::ref() {
        _refs.fetch_add(1, std::memory_order_relaxed);
        return;
}

void CanvasSurface::unref() {
        if (_refs.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
        this->~CanvasSurface();
        Memory::free(this);
        return;
}

void CanvasSurface::handleClosed(uint8_t) {
        unref(); // the program's descriptor: the canvas may still show the last present
        return;
}

void CanvasSurface::attach(GuiApplication &app, Changed changed, void *arg) {
        ref();
        Mutex::Locker locker(_lock, WaitForever);
        _app = &app;
        _changed = changed;
        _arg = arg;
        _damage = Rect();
        _posted = false;
        return;
}

void CanvasSurface::detach() {
        GuiApplication *app;
        {
                // No present posts after this: they post under the lock.
                Mutex::Locker locker(_lock, WaitForever);
                app = _app;
                _app = nullptr;
                _changed = nullptr;
                _posted = false;
        }
        if (app == nullptr) return;
        app->cancelPosts(this); // ours, not yet run (this is the loop's thread)
        unref();
        return;
}

Result<uint32_t> CanvasSurface::present(uint8_t index, Span<const Rect> damage) {
        if (index >= _bufferCount) return makeError<uint32_t>(Error::Invalid);
        {
                Mutex::Locker locker(_lock, WaitForever);
                if (_states[index] != State::Program) return makeError<uint32_t>(Error::Invalid);
        }
        writeBack(index); // the painter's DMA reads memory, not the cache (the lock not held)
        Rect changed;
        if (damage.size() == 0) changed = Rect{0, 0, _width, _height};
        for (const Rect &r : damage) changed = changed.united(r);
        bool freed;
        {
                Mutex::Locker locker(_lock, WaitForever);
                freed = takePresentLocked(index);
                if (_app != nullptr) {
                        _damage = _damage.united(changed);
                        if (!_posted) _posted = _app->post(changedDue, this).isOk(); // full: the next present's
                }
        }
        if (freed) _freed.release();
        return makeResult(frames() + 1);
}

void CanvasSurface::changedDue(void *arg) {
        auto   *self = static_cast<CanvasSurface *>(arg);
        Rect    damage;
        Changed changed;
        void   *changedArg;
        {
                Mutex::Locker locker(self->_lock, WaitForever);
                self->_posted = false;
                damage = self->_damage;
                self->_damage = Rect();
                changed = self->_changed;
                changedArg = self->_arg;
        }
        // Attached still: detach() cancels this, on this thread.
        if (changed != nullptr && !damage.isEmpty()) changed(changedArg, damage);
        return;
}

bool CanvasSurface::paint(Painter &painter, int x, int y, uint16_t width, uint16_t height) {
        int8_t index;
        {
                Mutex::Locker locker(_lock, WaitForever);
                index = beginReadLocked();
        }
        if (index >= 0) {
                const uint16_t w = width < _width ? width : _width;
                const uint16_t h = height < _height ? height : _height;
                if (_format == Format::Rgb565)
                        (void)painter.pixels565(x, y, _buffers[index], _stride, w, h);
                else
                        (void)painter.pixels(x, y, _buffers[index], _stride, w, h);
        }
        release(index);
        return index >= 0;
}

SwapChain::Info CanvasSurface::info() {
        Info i;
        {
                Mutex::Locker locker(_lock, WaitForever);
                i.shown = _app != nullptr;
                i.presents = _presents;
                i.dropped = _dropped;
        }
        i.width = _width;
        i.height = _height;
        i.frames = frames();
        return i;
}

Result<size_t> CanvasSurface::read(WritableByteSpan, unsigned int) {
        return makeError<size_t>(Error::NotSupported); // its canvas's events come by the protocol
}

Result<size_t> CanvasSurface::write(ByteSpan, unsigned int) {
        return makeError<size_t>(Error::NotSupported);
}

BMDS_NAMESPACE_END
