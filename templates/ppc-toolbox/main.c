/* MrC Toolbox baseline: C89 plus Apple's Pascal-string extension.
 * Keep update drawing and cooperative event handling when changing behavior.
 */
#include <Types.h>
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <Dialogs.h>
#include <TextEdit.h>
#include <Events.h>

QDGlobals qd;

int main(void)
{
    WindowPtr window;
    EventRecord event;
    Rect bounds;
    int done = 0;

    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(0L);
    InitCursor();
    SetRect(&bounds, 80, 60, 470, 210);
    window = NewWindow(0L, &bounds, "\pSherclawk native template", 1,
                       documentProc, (WindowPtr)-1L, 1, 0L);
    if (!window)
        return 1;

    while (!done) {
        if (!WaitNextEvent(everyEvent, &event, 30L, 0L))
            continue;
        if (event.what == updateEvt && (WindowPtr)event.message == window) {
            SetPort(window);
            BeginUpdate(window);
            EraseRect(&window->portRect);
            MoveTo(16, 44);
            DrawString("\pBuilt natively with MrC, PPCLink and Rez.");
            MoveTo(16, 70);
            DrawString("\pClick this window or press Command-Q to quit.");
            EndUpdate(window);
        } else if (event.what == mouseDown) {
            WindowPtr hit;
            if (FindWindow(event.where, &hit) == inContent && hit == window)
                done = 1;
        } else if (event.what == keyDown && (event.modifiers & cmdKey) &&
                   (event.message & charCodeMask) == 'q') {
            done = 1;
        }
    }
    DisposeWindow(window);
    return 0;
}
