#pragma once

// Pure three-way decision. Explicit choices, profile guards and journaling stay
// in the coordinator; this function cannot perform I/O or mutate progress.
enum class SyncDecision { Unchanged, Upload, ApplyRemote, Conflict };

constexpr SyncDecision decideSync(bool baselineKnown, bool localChanged, bool remoteChanged) {
    if (localChanged && !remoteChanged) return SyncDecision::Upload;
    if (remoteChanged && !localChanged) return SyncDecision::ApplyRemote;
    if (baselineKnown && !localChanged && !remoteChanged) return SyncDecision::Unchanged;
    return SyncDecision::Conflict;
}
