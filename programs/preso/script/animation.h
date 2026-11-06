/**
 * @file      animation.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * An animation of a scene's object (devplan/preso.md §7.4): properties changed from what they
 * are when it begins to where it ends, along an easing. play() runs some together; the region
 * gives each its progress a frame, or its end at once (fast-forward, Next).
 */

#ifndef PRESO_SCRIPT_ANIMATION_H
#define PRESO_SCRIPT_ANIMATION_H

#include "scene.h"

namespace preso {

        /** @brief How an animation's progress goes from 0 to 1. Scripts' Ease: append only. */
        enum class Ease : uint8_t {
                Linear,
                In,    ///< Slow, then fast.
                Out,   ///< Fast, then slow.
                InOut, ///< Slow at both ends (the default).
                Bounce,       ///< Lands and bounces, as a ball.
                ThereAndBack, ///< To its end and back to where it began (indicate()).
                Elastic,      ///< Past its end and back, as a spring.
                Wiggle,       ///< Either way of its end and back to where it began, a few times (wiggle()).
        };

        float eased(Ease ease, float t);
        /** @brief Whether @p ease ends where it began (an animation of it ends at its start). */
        bool returns(Ease ease);

        /** @brief What an animation changes. */
        enum class Property : uint8_t {
                X,
                Y,
                Scale,
                Rotation,
                Opacity,
                LineWidth,
                FillOpacity,
                Width,
                Height,
                Radius,
                Reveal,
                StrokeColor,
                FillColor,
                Morph,
                BarValue,  ///< A bar's (index).
                BarOffset, ///< A bar's place (index), in bars.
                BarColor,  ///< A bar's (index).
                CornerRadius,
                Start, ///< An arc's or sector's, degrees.
                Sweep,
                InnerRadius,
                Bend,  ///< A line's, degrees.
                Value, ///< A number's (its text follows).
                Dash,
                HeadSize,
                Along, ///< Where along `other`'s path (0 to 1): moveAlong(); `index` 1: turned to face its way.
                Dim,   ///< Code: how much the lines out of focus are dimmed.
        };

        /** @brief The property scripts write as @p name (tween()); false if there is none. */
        bool propertyNamed(StringView name, Property *property);

        /**
         * @brief Changes to an object over a play.
         *
         * @par Thread Safety
         * Not thread-safe: the core's thread.
         */
        class Animation {
                public:
                        static constexpr uint32_t MaxChanges = 4;

                        enum class Then : uint8_t {
                                Nothing,
                                Remove,   ///< Out of the scene at its end, its opacity and scale as they were.
                                Become,   ///< The target takes `other`'s look (a transform).
                                SwapBars, ///< Bars `index` and `index2` change places at its end.
                                SwapChildren, ///< Its children `index` and `index2` change places in its order.
                                Focus,        ///< Code: `mask` its lines in focus (as it begins, if any; at its end).
                                NewCode, ///< Code: `other`'s text as it begins, hidden (its lines move) until its end.
                        };

                        static Animation *make(Shape *target);
                        void              addRef() { _refs++; }
                        void              release();

                        /** @brief A change to @p to (@p relative: by @p to from where it begins). */
                        bool change(Property property, float to, bool relative = false, uint16_t index = 0);
                        bool changeColor(Property property, Color to, uint16_t index = 0);
                        /** @brief Begins from @p from, not from what it is then. */
                        void from(float value);

                        Shape   *target;
                        Shape   *other = nullptr; ///< Then::Become's; Along's path.
                        /** @brief Animations played with it, on its progress (a composite's parts). */
                        Animation *with = nullptr;
                        /** @brief Plays @p a with this (a reference taken); false if @p a is null. */
                        bool     join(Animation *a);
                        Ease     ease = Ease::InOut;
                        float    delay = 0;     ///< Seconds after the play begins.
                        float    duration = -1; ///< Seconds; < 0: the play's.
                        bool     adds = false;  ///< Into the scene as it begins.
                        Then     then = Then::Nothing;
                        uint16_t index = 0; ///< SwapBars'.
                        uint16_t index2 = 0;
                        uint64_t mask = 0; ///< Then::Focus's.

                        /** @brief Begins: values captured, added to @p scene if it brings its target in. */
                        void begin(Scene &scene);
                        /** @brief At @p t (0 to 1, before easing). */
                        void apply(float t);
                        /** @brief Ends: its final values, and what it does then. */
                        void end(Scene &scene);
                        bool hasBegun() const { return _begun; }
                        bool hasEnded() const { return _ended; }

                private:
                        struct Change {
                                        Property property;
                                        uint16_t index;
                                        bool     relative;
                                        bool     isColor;
                                        bool     fromGiven;
                                        float    from;
                                        float    to;
                                        Color    fromColor;
                                        Color    toColor;
                        };

                        explicit Animation(Shape *t) : target(t) {}
                        ~Animation();
                        float get(const Change &c) const;
                        void  set(const Change &c, float v, Color color);

                        Change   _changes[MaxChanges];
                        uint32_t _count = 0;
                        float    _restoreOpacity = 1;
                        float    _restoreScale = 1;
                        bool     _begun = false;
                        bool     _ended = false;
                        uint32_t _refs = 1;
        };

} // namespace preso

#endif // PRESO_SCRIPT_ANIMATION_H
