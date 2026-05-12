#include <gtest/gtest.h>

#include "services/transport_fsm.h"

using daw::services::TransportAction;
using daw::services::TransportEvent;
using daw::services::TransportNext;
using daw::services::TransportState;

namespace {

void Expect(TransportState from, TransportEvent ev,
            TransportState toState, TransportAction action) {
    const auto t = TransportNext(from, ev);
    EXPECT_EQ(static_cast<int>(t.newState), static_cast<int>(toState))
        << "from=" << static_cast<int>(from) << " ev=" << static_cast<int>(ev);
    EXPECT_EQ(static_cast<int>(t.action), static_cast<int>(action))
        << "from=" << static_cast<int>(from) << " ev=" << static_cast<int>(ev);
}

} // namespace

TEST(TransportFsm, StoppedPlayStartsPlayback) {
    Expect(TransportState::Stopped, TransportEvent::PlayPressed,
           TransportState::Playing, TransportAction::StartPlayback);
}

TEST(TransportFsm, StoppedRecordStartsRecording) {
    Expect(TransportState::Stopped, TransportEvent::RecordPressed,
           TransportState::Recording, TransportAction::StartRecording);
}

TEST(TransportFsm, StoppedRecordWithCountInGoesToCountingIn) {
    Expect(TransportState::Stopped, TransportEvent::RecordPressedWithCountIn,
           TransportState::CountingIn, TransportAction::StartCountIn);
}

TEST(TransportFsm, CountingInCompleteBeginsRecording) {
    Expect(TransportState::CountingIn, TransportEvent::CountInComplete,
           TransportState::Recording, TransportAction::StartRecording);
}

TEST(TransportFsm, CountingInStopGoesBackToStopped) {
    // Recording subsystem is armed during count-in, so stopping must tear
    // down recording (caller chains StopPlayback after).
    Expect(TransportState::CountingIn, TransportEvent::StopPressed,
           TransportState::Stopped, TransportAction::StopRecording);
}

TEST(TransportFsm, PlayingStopStopsPlayback) {
    Expect(TransportState::Playing, TransportEvent::StopPressed,
           TransportState::Stopped, TransportAction::StopPlayback);
}

TEST(TransportFsm, RecordingStopStopsRecording) {
    Expect(TransportState::Recording, TransportEvent::StopPressed,
           TransportState::Stopped, TransportAction::StopRecording);
}

TEST(TransportFsm, RecordingRecordPressTogglesOff) {
    Expect(TransportState::Recording, TransportEvent::RecordPressed,
           TransportState::Stopped, TransportAction::StopRecording);
}

TEST(TransportFsm, FailedFromPlayingFallsToStopped) {
    Expect(TransportState::Playing, TransportEvent::Failed,
           TransportState::Stopped, TransportAction::StopPlayback);
}

TEST(TransportFsm, FailedFromRecordingFallsToStopped) {
    Expect(TransportState::Recording, TransportEvent::Failed,
           TransportState::Stopped, TransportAction::StopRecording);
}

TEST(TransportFsm, IllegalEventsAreNoOps) {
    // Play while playing.
    Expect(TransportState::Playing, TransportEvent::PlayPressed,
           TransportState::Playing, TransportAction::None);
    // Stop while stopped.
    Expect(TransportState::Stopped, TransportEvent::StopPressed,
           TransportState::Stopped, TransportAction::None);
    // CountInComplete from Stopped (spurious).
    Expect(TransportState::Stopped, TransportEvent::CountInComplete,
           TransportState::Stopped, TransportAction::None);
    // Failed from Stopped.
    Expect(TransportState::Stopped, TransportEvent::Failed,
           TransportState::Stopped, TransportAction::None);
    // CountInComplete from Recording (already done).
    Expect(TransportState::Recording, TransportEvent::CountInComplete,
           TransportState::Recording, TransportAction::None);
}

TEST(TransportFsm, PlayingPunchInToRecording) {
    Expect(TransportState::Playing, TransportEvent::RecordPressed,
           TransportState::Recording, TransportAction::StartRecording);
}
