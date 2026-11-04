std::mutex g_trainingHandoffPublicationMutex;

class TrainingHandoff {
public:
    explicit TrainingHandoff(bool needed = true) : publication_(g_trainingHandoffPublicationMutex, std::defer_lock) {
        if (!needed) { ready_ = true; return; }
        HMODULE module = GetModuleHandleA("efz_training_mode.dll");
        if (!module) { ready_ = true; return; }
        auto resolve = [module](const char* plain, const char* decorated) {
            FARPROC result = GetProcAddress(module, plain);
            return result ? result : GetProcAddress(module, decorated);
        };
        auto prepare = reinterpret_cast<uint32_t (__cdecl*)()>(resolve(
            "EFZ_TM_PrepareNetplayV1", "_EFZ_TM_PrepareNetplayV1"));
        finish_ = reinterpret_cast<void (__cdecl*)()>(resolve(
            "EFZ_TM_FinishNetplayPrepareV1", "_EFZ_TM_FinishNetplayPrepareV1"));
        if (!prepare || !finish_) {
            mod::Log("Training ownership retirement unavailable; online handoff blocked");
            return;
        }
        publication_.lock();
        if (!prepare()) return;
        acquired_ = true;
        alreadySuspended_ = state_export::IsSuspendedForOnlineSimulation();
        if (!state_export::SuspendForOnlineSimulation()) {
            if (!alreadySuspended_) state_export::ResumeControlPlaneUpdates();
            finish_();
            acquired_ = false;
            mod::Log("Training ownership retirement blocked by export drain timeout");
            return;
        }
        ready_ = true;
    }
    ~TrainingHandoff() {
        if (acquired_) {
            state_export::PublishTrainingHandoffBoundary(GetStatus());
            if (!alreadySuspended_) state_export::ResumeControlPlaneUpdates();
            finish_();
        }
    }
    bool Ready() const { return ready_; }
private:
    std::unique_lock<std::mutex> publication_;
    void (__cdecl* finish_)() = nullptr;
    bool ready_ = false;
    bool acquired_ = false;
    bool alreadySuspended_ = false;
};
