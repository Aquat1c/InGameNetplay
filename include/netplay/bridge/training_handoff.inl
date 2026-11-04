std::mutex g_trainingHandoffPublicationMutex;

class TrainingHandoff {
public:
    explicit TrainingHandoff(bool needed = true) : publication_(g_trainingHandoffPublicationMutex, std::defer_lock) {
        if (!needed) { state_ = State::Bypassed; return; }
        HMODULE module = GetModuleHandleA("efz_training_mode.dll");
        if (!module) { state_ = State::Bypassed; return; }
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
        const bool alreadySuspended = state_export::IsSuspendedForOnlineSimulation();
        if (!state_export::SuspendForOnlineSimulation()) {
            if (!alreadySuspended) state_export::ResumeControlPlaneUpdates();
            finish_();
            mod::Log("Training ownership retirement blocked by export drain timeout");
            return;
        }
        state_ = alreadySuspended ? State::PreparedKeepSuspended : State::PreparedResumeExports;
    }
    ~TrainingHandoff() {
        if (state_ == State::PreparedKeepSuspended || state_ == State::PreparedResumeExports) {
            state_export::PublishTrainingHandoffBoundary(GetStatus());
            if (state_ == State::PreparedResumeExports) state_export::ResumeControlPlaneUpdates();
            finish_();
        }
    }
    bool Ready() const { return state_ != State::Unavailable; }
private:
    std::unique_lock<std::mutex> publication_;
    void (__cdecl* finish_)() = nullptr;
    enum class State { Unavailable, Bypassed, PreparedKeepSuspended, PreparedResumeExports };
    State state_ = State::Unavailable;
};
