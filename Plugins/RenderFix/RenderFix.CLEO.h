#pragma once

// RenderFix's CLEO-side graphics guard. The module is deliberately independent
// from GTA-side performance patches so either domain can be disabled separately.
namespace RenderFixCleo
{
    void Configure(bool enabled, bool renderStateGuard);
    void OnFrameBegin();
    void OnFrameEnd();
}
