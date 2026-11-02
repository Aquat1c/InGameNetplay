#pragma once

#include "imgui.h"

// Never infer ImGui context ownership: explicitly select this backend's.
class EfzScopedImGuiContext {
public:
    explicit EfzScopedImGuiContext(ImGuiContext* context)
        : previous_(ImGui::GetCurrentContext()) { ImGui::SetCurrentContext(context); }
    ~EfzScopedImGuiContext() { ImGui::SetCurrentContext(previous_); }
    EfzScopedImGuiContext(const EfzScopedImGuiContext&) = delete;
    EfzScopedImGuiContext& operator=(const EfzScopedImGuiContext&) = delete;
    void ForgetDestroyed(ImGuiContext* context) {
        if (previous_ == context) previous_ = nullptr;
    }
private:
    ImGuiContext* previous_;
};
