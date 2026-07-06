#pragma once

namespace moo {

// GUI -> engine control commands, applied between ticks on the render thread.
struct Command {
    enum class Type {
        SetProgram,     // arg = source index
        SetPreview,     // arg = source index
        Cut,
        Auto,
        TbarBegin,
        TbarSet,        // farg = position 0..1
        TbarEnd,
        FadeToBlack,
        SetTransition,  // arg = TransitionType, arg2 = duration ticks, farg = softness
    } type = Type::Cut;
    int arg = 0;
    int arg2 = 0;
    float farg = 0.f;
};

}  // namespace moo
