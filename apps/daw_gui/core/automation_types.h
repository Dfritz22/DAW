#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

// Pure value types live in libs/core. The app uses Track*-prefixed aliases
// for historical naming consistency; the underlying types are identical.

#include "core/automation_curve.h"

using AutomationInterpolationMode = daw::core::AutomationInterpolationMode;
using TrackAutomationPoint        = daw::core::AutomationPoint;
using TrackAutomationCurve        = daw::core::AutomationCurve;

using GainAutomation = TrackAutomationCurve;
using PanAutomation  = TrackAutomationCurve;
using BusAutomation  = TrackAutomationCurve;
