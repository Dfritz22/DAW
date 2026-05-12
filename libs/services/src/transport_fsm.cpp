#include "services/transport_fsm.h"

namespace daw::services {

TransportTransition TransportNext(TransportState state, TransportEvent ev) {
    switch (state) {
    case TransportState::Stopped:
        switch (ev) {
        case TransportEvent::PlayPressed:
            return {TransportState::Playing, TransportAction::StartPlayback};
        case TransportEvent::RecordPressed:
            return {TransportState::Recording, TransportAction::StartRecording};
        case TransportEvent::RecordPressedWithCountIn:
            return {TransportState::CountingIn, TransportAction::StartCountIn};
        case TransportEvent::StopPressed:
        case TransportEvent::CountInComplete:
        case TransportEvent::Failed:
            return {state, TransportAction::None};
        }
        break;

    case TransportState::Playing:
        switch (ev) {
        case TransportEvent::StopPressed:
        case TransportEvent::Failed:
            return {TransportState::Stopped, TransportAction::StopPlayback};
        case TransportEvent::RecordPressed:
            // Pro Tools-style: pressing Record while playing punches in.
            // We model that as StopPlayback then StartRecording — caller
            // dispatches StopPlayback first via a separate Stop event if
            // it needs the explicit ordering. Here we just go to Recording.
            return {TransportState::Recording, TransportAction::StartRecording};
        case TransportEvent::RecordPressedWithCountIn:
            return {TransportState::CountingIn, TransportAction::StartCountIn};
        case TransportEvent::PlayPressed:
        case TransportEvent::CountInComplete:
            return {state, TransportAction::None};
        }
        break;

    case TransportState::CountingIn:
        switch (ev) {
        case TransportEvent::CountInComplete:
            return {TransportState::Recording, TransportAction::StartRecording};
        case TransportEvent::StopPressed:
        case TransportEvent::Failed:
            return {TransportState::Stopped, TransportAction::StopPlayback};
        case TransportEvent::PlayPressed:
        case TransportEvent::RecordPressed:
        case TransportEvent::RecordPressedWithCountIn:
            return {state, TransportAction::None};
        }
        break;

    case TransportState::Recording:
        switch (ev) {
        case TransportEvent::StopPressed:
        case TransportEvent::RecordPressed:
        case TransportEvent::Failed:
            return {TransportState::Stopped, TransportAction::StopRecording};
        case TransportEvent::PlayPressed:
        case TransportEvent::RecordPressedWithCountIn:
        case TransportEvent::CountInComplete:
            return {state, TransportAction::None};
        }
        break;
    }
    return {state, TransportAction::None};
}

} // namespace daw::services
