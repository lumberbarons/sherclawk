/* Pure display helpers: no Toolbox state or UI side effects. */
#ifndef SHERCLAWK_DISPLAY_H
#define SHERCLAWK_DISPLAY_H
#include <stddef.h>
#include "agent.h"
/* QuickDraw field order, expressed without a Toolbox dependency. */
typedef struct { short top, left, bottom, right; } DisplayRect;
typedef struct {
    DisplayRect ResponseLabelRect;
    DisplayRect ResponseRect;
    DisplayRect ResponseViewRect;
    DisplayRect PromptLabelRect;
    DisplayRect PromptHintRect;
    DisplayRect PromptRect;
    DisplayRect InfoRect;
    DisplayRect StatusRect;
    DisplayRect ModelRect;
    DisplayRect HistoryRect;
    DisplayRect MeterRect;
    DisplayRect UsageRect;
    DisplayRect ContextRect;
    DisplayRect CostRect;
    DisplayRect NewRect;
    DisplayRect HandoffRect;
    DisplayRect StopRect;
    DisplayRect SendRect;
} DisplayLayout;
void ComputeLayout(DisplayLayout *layout);
/* View heights are pixel differences; line counts/heights come from TextEdit.
 * Invalid line heights use its 12-pixel fallback; maximum offsets cap at 32767. */
short ComputeMaxScroll(short line_height, short line_count, int view_height);
short ClampScroll(long pixels, short maximum);
short ResponsePageHeight(short line_height, int view_height);
void FormatTokens(long tokens, char *out, size_t cap);
/* Append whole strings only; out/events must be NUL-terminated when cap > 0. */
void AppendText(char *out, size_t cap, const char *text);
void RenderToolCall(const AgentCall *call, char *out, size_t cap);
void ToolEventsReset(char *events, size_t cap);
void ToolEventsAppend(char *events, size_t cap, const char *event);
int ModelLookupAllowed(const char *model);
/* Nonempty printable ASCII model ID. Lookup uses a narrower URL-safe alphabet. */
int model_id_valid(const char *model);
/* Matches the MacRoman composer: space, CR and tab are blank. */
int prompt_is_blank(const char *prompt);
void FormatPauseReason(int rounds, int tools, int max_rounds, int max_tools,
                       size_t used, char *out, size_t cap);
#endif
