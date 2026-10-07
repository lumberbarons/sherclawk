/*
 * hello.r — resources for "Sherclawk", compiled by Rez at build time.
 *
 * 'SIZE' asks for 8 MB preferred / 4 MB minimum — the Certainly TLS
 * stack, BearSSL tables, tool history and response buffers need more than
 * Retro68's 1 MB default. 'MBAR' and the 'MENU' resources define the
 * menu bar; the app uses SetMenuBar(GetNewMBar(128)). Retro68 adds
 * 'cfrg' and the rest of the application resources automatically.
 */
#include "Processes.r"
#include "SysTypes.r"
#include "Menus.r"
#include "Dialogs.r"

resource 'SIZE' (-1) {
    reserved,
    acceptSuspendResumeEvents,
    reserved,
    canBackground,
    doesActivateOnFGSwitch,
    backgroundAndForeground,
    dontGetFrontClicks,
    ignoreChildDiedEvents,
    is32BitCompatible,
    isHighLevelEventAware,
    onlyLocalHLEvents,
    notStationeryAware,
    dontUseTextEditServices,
    reserved,
    reserved,
    reserved,
    8 * 1024 * 1024,   /* preferred */
    4 * 1024 * 1024    /* minimum   */
};

resource 'MBAR' (128, "Menu bar") {
    { 128, 129, 130 }
};

resource 'MENU' (128, "Apple") {
    128, textMenuProc, allEnabled, enabled, apple, {
        "About Sherclawk\0xC9", noIcon, noKey, noMark, plain;
        "-", noIcon, noKey, noMark, plain;
    }
};

resource 'MENU' (129, "File") {
    129, textMenuProc, allEnabled, enabled, "File", {
        "New Chat", noIcon, "N", noMark, plain;
        "Save Handoff", noIcon, "H", noMark, plain;
        "-", noIcon, noKey, noMark, plain;
        "Quit", noIcon, "Q", noMark, plain;
    }
};

resource 'MENU' (130, "Edit") {
    130, textMenuProc, allEnabled, enabled, "Edit", {
        "Undo",  noIcon, "Z", noMark, plain;
        "-",     noIcon, noKey, noMark, plain;
        "Cut",   noIcon, "X", noMark, plain;
        "Copy",  noIcon, "C", noMark, plain;
        "Paste", noIcon, "V", noMark, plain;
        "Clear", noIcon, noKey, noMark, plain;
        "-", noIcon, noKey, noMark, plain;
        "Select All", noIcon, "A", noMark, plain;
        "-", noIcon, noKey, noMark, plain;
        "Preferences\0xC9", noIcon, noKey, noMark, plain;
    }
};

/* Preferences (Edit menu, last item). EditText capacity is the length of the
 * template string: model and key 255 characters, workspace 192 (the
 * validator's cap), limits 3. main.c seeds every field from the live values. */
resource 'DLOG' (128, "Preferences") {
    {70, 60, 320, 480},
    movableDBoxProc,
    visible,
    noGoAway,
    0x0,
    128,
    "Preferences",
    centerMainScreen
};

resource 'DITL' (128, "Preferences") {
    {
        /* [1] OK */            {222, 262, 242, 324}, Button { enabled, "OK" };
        /* [2] Cancel */        {222, 332, 242, 404}, Button { enabled, "Cancel" };
        /* [3] Model */         {12, 12, 28, 100},   StaticText { disabled, "Model:" };
        /* [4] */               {28, 10, 46, 408},   EditText { enabled, "                                                                                                                                                                                                                                                               " };
        /* [5] API key */       {53, 12, 69, 100},   StaticText { disabled, "API key:" };
        /* [6] */               {69, 10, 87, 408},   EditText { enabled, "                                                                                                                                                                                                                                                               " };
        /* [7] Workspace */     {94, 12, 110, 100},  StaticText { disabled, "Workspace:" };
        /* [8] */               {110, 10, 128, 408}, EditText { enabled, "                                                                                                                                                                                                " };
        /* [9] Rounds */        {136, 12, 152, 130}, StaticText { disabled, "Max model rounds:" };
        /* [10] */              {134, 135, 152, 195}, EditText { enabled, "128" };
        /* [11] Tools */        {136, 215, 152, 325}, StaticText { disabled, "Max tool calls:" };
        /* [12] */              {134, 330, 152, 390}, EditText { enabled, "128" };
        /* [13] Debug toggle */ {162, 12, 178, 28},  CheckBox { enabled, "" };
        /* [14] Debug label */  {162, 32, 178, 408}, StaticText { disabled, "Show tool debug in Conversation" };
        /* [15] Hint */         {192, 12, 208, 408}, StaticText { disabled, "Stored in System Folder:Preferences. Limits apply to each run." };
    }
};

/* One reusable validation alert; main.c substitutes the message with
 * ParamText. The caution icon is ICON 2 from the System file. */
resource 'ALRT' (128, "Preferences Error") {
    {80, 80, 180, 420},
    129,
    {
        OK, visible, sound1,
        OK, visible, sound1,
        OK, visible, sound1,
        OK, visible, sound1
    },
    alertPositionMainScreen
};

resource 'DITL' (129, "Preferences Error") {
    {
        /* [1] OK */   {68, 270, 88, 330},  Button { enabled, "OK" };
        /* [2] Text */ {14, 60, 60, 330},   StaticText { disabled, "^0" };
        /* [3] Icon */ {12, 12, 44, 44},    Icon { disabled, 2 };
    }
};
