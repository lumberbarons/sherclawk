/* PPCLink supplies cfrg; append SIZE without replacing the resource fork.
   Self-render needs the 8-bit GWorld (width x height bytes) on the heap. */
#include "Types.r"

resource 'SIZE' (-1) {
    reserved,
    acceptSuspendResumeEvents,
    reserved,
    canBackground,
    doesActivateOnFGSwitch,
    backgroundAndForeground,
    dontGetFrontClicks,
    ignoreAppDiedEvents,
    is32BitCompatible,
    isHighLevelEventAware,
    onlyLocalHLEvents,
    notStationeryAware,
    dontUseTextEditServices,
    reserved, reserved, reserved,
    2048 * 1024,
    1536 * 1024
};
