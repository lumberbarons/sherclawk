/* Queued Apple-event replies require a high-level-event-aware application.
 * Keep this spike separate from the main app's resource declaration. */
#include "Types.r"
#include "Processes.r"
resource 'SIZE' (-1) {
    reserved, acceptSuspendResumeEvents, reserved, canBackground,
    doesActivateOnFGSwitch, backgroundAndForeground, dontGetFrontClicks,
    ignoreChildDiedEvents, is32BitCompatible, isHighLevelEventAware,
    onlyLocalHLEvents, notStationeryAware, dontUseTextEditServices,
    reserved, reserved, reserved, 4 * 1024 * 1024, 2 * 1024 * 1024
};
