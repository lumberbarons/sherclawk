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

/* Preferences (Edit menu, last item). The model row is the search box and the
 * saved value: the dialog lists fetched catalog rows below it and validates a
 * typed id before saving. EditText capacity is the length of the template
 * string: model and key 255 characters, workspace 192 (the validator's cap),
 * limits 3. main.c seeds every field from the live values and creates the
 * Effort popup on the user item at [9]. */
resource 'DLOG' (128, "Preferences") {
    {64, 40, 434, 496},
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
        /* [1] OK */                 {350, 282, 370, 344}, Button { enabled, "OK" };
        /* [2] Cancel */             {350, 352, 370, 424}, Button { enabled, "Cancel" };
        /* [3] Model */              {16, 16, 32, 80}, StaticText { disabled, "Model:" };
        /* [4] */                    {14, 84, 32, 344}, EditText { enabled, "                                                                                                                                                                                                                                                               " };
        /* [5] Find */               {14, 352, 32, 424}, Button { enabled, "Find" };
        /* [6] Results (drawn) */    {34, 16, 158, 424}, UserItem { enabled };
        /* [7] Hint */               {160, 16, 174, 424}, StaticText { disabled, "Loading the popular models..." };
        /* [8] Effort */             {176, 16, 192, 80}, StaticText { disabled, "Effort:" };
        /* [9] Effort popup */       {176, 84, 194, 262}, UserItem { enabled };
        /* [10] API key */           {200, 16, 216, 80}, StaticText { disabled, "API key:" };
        /* [11] */                   {198, 84, 216, 424}, EditText { enabled, "                                                                                                                                                                                                                                                               " };
        /* [12] Workspace */         {226, 16, 242, 80}, StaticText { disabled, "Workspace:" };
        /* [13] */                   {224, 84, 242, 424}, EditText { enabled, "                                                                                                                                                                                                " };
        /* [14] Rounds */            {252, 16, 268, 140}, StaticText { disabled, "Max rounds:" };
        /* [15] */                   {250, 144, 268, 224}, EditText { enabled, "128" };
        /* [16] Tools */             {278, 16, 294, 140}, StaticText { disabled, "Max tool calls:" };
        /* [17] */                   {276, 144, 294, 224}, EditText { enabled, "128" };
        /* [18] Debug toggle */      {300, 16, 316, 32}, CheckBox { enabled, "" };
        /* [19] Debug label */       {300, 36, 316, 424}, StaticText { disabled, "Show tool debug in Conversation" };
        /* [20] Stored hint */       {322, 16, 338, 424}, StaticText { disabled, "Stored in System Folder:Preferences. Limits apply to each run." };
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
