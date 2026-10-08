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
    {64, 56, 300, 496},
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
        /* [1] OK */                 {202, 282, 222, 344}, Button { enabled, "OK" };
        /* [2] Cancel */             {202, 352, 222, 424}, Button { enabled, "Cancel" };
        /* [3] Model */              {16, 16, 32, 140}, StaticText { disabled, "Model:" };
        /* [4] */                    {14, 144, 32, 424}, EditText { enabled, "                                                                                                                                                                                                                                                               " };
        /* [5] API key */            {44, 16, 60, 140}, StaticText { disabled, "API key:" };
        /* [6] */                    {42, 144, 60, 424}, EditText { enabled, "                                                                                                                                                                                                                                                               " };
        /* [7] Workspace */          {72, 16, 88, 140}, StaticText { disabled, "Workspace:" };
        /* [8] */                    {70, 144, 88, 424}, EditText { enabled, "                                                                                                                                                                                                " };
        /* [9] Rounds */             {100, 16, 116, 140}, StaticText { disabled, "Max rounds:" };
        /* [10] */                   {98, 144, 116, 224}, EditText { enabled, "128" };
        /* [11] Tools */             {128, 16, 144, 140}, StaticText { disabled, "Max tool calls:" };
        /* [12] */                   {126, 144, 144, 224}, EditText { enabled, "128" };
        /* [13] Debug toggle */      {152, 16, 168, 32}, CheckBox { enabled, "" };
        /* [14] Debug label */       {152, 36, 168, 424}, StaticText { disabled, "Show tool debug in Conversation" };
        /* [15] Hint */              {178, 16, 194, 424}, StaticText { disabled, "Stored in System Folder:Preferences. Limits apply to each run." };
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

/* Full status text on demand, without adding UI messages to the transcript. */
resource 'ALRT' (129, "Status") {
    {80, 60, 220, 580},
    130,
    {
        OK, visible, silent,
        OK, visible, silent,
        OK, visible, silent,
        OK, visible, silent
    },
    alertPositionMainScreen
};

resource 'DITL' (130, "Status") {
    {
        /* [1] OK */   {108, 440, 128, 500}, Button { enabled, "OK" };
        /* [2] Text */ {14, 18, 96, 500},    StaticText { disabled, "^0" };
    }
};
