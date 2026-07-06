#pragma once

namespace moo {

// GUI -> engine control commands, applied between ticks on the render thread.
struct Command {
    enum class Type { SetProgram, SetPreview, Cut } type = Type::Cut;
    int arg = 0;
};

}  // namespace moo
