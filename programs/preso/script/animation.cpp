/**
 * @file      animation.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "animation.h"
#include <math.h>
#include <new>
#include <stdlib.h>

namespace preso {

        float eased(Ease ease, float t) {
                if (t <= 0) return 0;
                if (t >= 1) return 1;
                switch (ease) {
                        case Ease::Linear: return t;
                        case Ease::In: return t * t * t;
                        case Ease::Out: {
                                float u = 1 - t;
                                return 1 - u * u * u;
                        }
                        case Ease::InOut: {
                                float u = -2 * t + 2;
                                return t < 0.5f ? 4 * t * t * t : 1 - u * u * u / 2;
                        }
                        case Ease::Bounce: {
                                // Robert Penner's bounce out.
                                const float n = 7.5625f, d = 2.75f;
                                if (t < 1 / d) return n * t * t;
                                if (t < 2 / d) {
                                        t -= 1.5f / d;
                                        return n * t * t + 0.75f;
                                }
                                if (t < 2.5f / d) {
                                        t -= 2.25f / d;
                                        return n * t * t + 0.9375f;
                                }
                                t -= 2.625f / d;
                                return n * t * t + 0.984375f;
                        }
                }
                return t;
        }

        bool propertyNamed(StringView name, Property *property) {
                struct Named {
                                const char *name;
                                Property    property;
                };
                static constexpr Named names[] = {
                        {"x", Property::X},
                        {"y", Property::Y},
                        {"scale", Property::Scale},
                        {"rotation", Property::Rotation},
                        {"opacity", Property::Opacity},
                        {"lineWidth", Property::LineWidth},
                        {"fillOpacity", Property::FillOpacity},
                        {"width", Property::Width},
                        {"height", Property::Height},
                        {"radius", Property::Radius},
                        {"reveal", Property::Reveal},
                };
                for (const Named &n : names) {
                        if (name.equals(n.name)) {
                                *property = n.property;
                                return true;
                        }
                }
                return false;
        }

        Animation *Animation::make(Shape *target) {
                if (target == nullptr) return nullptr;
                void *p = malloc(sizeof(Animation));
                if (p == nullptr) return nullptr;
                target->addRef();
                return new (p) Animation(target);
        }

        Animation::~Animation() {
                target->release();
                if (other != nullptr) other->release();
        }

        void Animation::release() {
                if (--_refs > 0) return;
                this->~Animation();
                free(this);
                return;
        }

        bool Animation::change(Property property, float to, bool relative, uint16_t idx) {
                if (_count == MaxChanges) return false;
                _changes[_count++] = Change{property, idx, relative, false, false, 0, to, 0, 0};
                return true;
        }

        bool Animation::changeColor(Property property, Color to, uint16_t idx) {
                if (_count == MaxChanges) return false;
                _changes[_count++] = Change{property, idx, false, true, false, 0, 0, 0, to};
                return true;
        }

        void Animation::from(float value) {
                if (_count == 0) return;
                _changes[_count - 1].fromGiven = true;
                _changes[_count - 1].from = value;
                return;
        }

        float Animation::get(const Change &c) const {
                Shape &s = *target;
                switch (c.property) {
                        case Property::X: return s.x;
                        case Property::Y: return s.y;
                        case Property::Scale: return s.scale;
                        case Property::Rotation: return s.rotation;
                        case Property::Opacity: return s.opacity;
                        case Property::LineWidth: return s.lineWidth;
                        case Property::FillOpacity: return s.fillOpacity;
                        case Property::Width: return s.width;
                        case Property::Height: return s.height;
                        case Property::Radius: return s.radius;
                        case Property::Reveal: return s.reveal;
                        case Property::Morph: return s.morph;
                        case Property::BarValue: return c.index < s.barCount ? s.bars[c.index].value : 0;
                        case Property::BarOffset: return c.index < s.barCount ? s.bars[c.index].offset : 0;
                        case Property::StrokeColor:
                        case Property::FillColor:
                        case Property::BarColor: return 0;
                }
                return 0;
        }

        void Animation::set(const Change &c, float v, Color color) {
                Shape &s = *target;
                switch (c.property) {
                        case Property::X: s.x = v; break;
                        case Property::Y: s.y = v; break;
                        case Property::Scale: s.scale = v; break;
                        case Property::Rotation: s.rotation = v; break;
                        case Property::Opacity: s.opacity = v; break;
                        case Property::LineWidth: s.lineWidth = v; break;
                        case Property::FillOpacity: s.fillOpacity = v; break;
                        case Property::Width: s.width = v; break;
                        case Property::Height: s.height = v; break;
                        case Property::Radius: s.radius = v; break;
                        case Property::Reveal: s.reveal = v; break;
                        case Property::Morph: s.morph = v; break;
                        case Property::StrokeColor: s.color = color; break;
                        case Property::FillColor: s.fill = color; break;
                        case Property::BarValue:
                                if (c.index < s.barCount) s.bars[c.index].value = v;
                                break;
                        case Property::BarOffset:
                                if (c.index < s.barCount) s.bars[c.index].offset = v;
                                break;
                        case Property::BarColor:
                                if (c.index < s.barCount) s.bars[c.index].color = color;
                                break;
                }
                return;
        }

        void Animation::begin(Scene &scene) {
                if (_begun) return;
                _begun = true;
                _restoreOpacity = target->opacity;
                _restoreScale = target->scale;
                for (uint32_t i = 0; i < _count; i++) {
                        Change &c = _changes[i];
                        if (c.isColor) {
                                Shape &s = *target;
                                c.fromColor = c.property == Property::StrokeColor ? s.color
                                              : c.property == Property::FillColor ? s.fill
                                              : c.index < s.barCount              ? s.bars[c.index].color
                                                                                  : 0;
                                continue;
                        }
                        float now = get(c);
                        if (c.relative) c.to += now;
                        if (!c.fromGiven) c.from = now;
                }
                if (then == Then::Become && other != nullptr) {
                        if (target->morphTo != nullptr) target->morphTo->release();
                        other->addRef();
                        target->morphTo = other;
                }
                if (adds) (void)scene.add(target);
                apply(0);
                return;
        }

        void Animation::apply(float t) {
                float e = eased(ease, t);
                for (uint32_t i = 0; i < _count; i++) {
                        const Change &c = _changes[i];
                        if (c.isColor) {
                                uint32_t amount = uint32_t(e * 256 + 0.5f);
                                set(c, 0, mixColors(c.fromColor, c.toColor, amount > 256 ? 256 : amount));
                        } else {
                                set(c, c.from + (c.to - c.from) * e, 0);
                        }
                }
                return;
        }

        void Animation::end(Scene &scene) {
                if (_ended) return;
                if (!_begun) begin(scene);
                _ended = true;
                // Its last values exactly (an easing's end may not be 1 to the last bit).
                for (uint32_t i = 0; i < _count; i++) {
                        const Change &c = _changes[i];
                        set(c, c.to, c.toColor);
                }
                switch (then) {
                        case Then::Nothing: break;
                        case Then::Remove:
                                scene.remove(target);
                                target->opacity = _restoreOpacity;
                                target->scale = _restoreScale;
                                break;
                        case Then::Become:
                                if (other != nullptr) (void)target->become(*other);
                                if (target->morphTo != nullptr) target->morphTo->release();
                                target->morphTo = nullptr;
                                target->morph = 0;
                                break;
                        case Then::SwapBars: {
                                Shape &s = *target;
                                if (index < s.barCount && index2 < s.barCount) {
                                        Bar t = s.bars[index];
                                        s.bars[index] = s.bars[index2];
                                        s.bars[index2] = t;
                                        s.bars[index].offset = 0;
                                        s.bars[index2].offset = 0;
                                }
                                break;
                        }
                }
                return;
        }

} // namespace preso
