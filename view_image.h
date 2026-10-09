/* Read-only view_image: load one workspace PNG so the next model request can
 * carry its pixels. Reads in bounded steps; nothing is written or journaled. */
#ifndef SHERCLAWK_VIEW_IMAGE_H
#define SHERCLAWK_VIEW_IMAGE_H
#include "tools.h"
#include <stdint.h>
/* The selected model's AGENT_VISION_* flag. Anything but YES fails closed. */
void view_image_set_vision(int vision);
/* Drop any held image; a new conversation or run must not inherit one. */
void view_image_reset(void);
/* Pending-tool entry points: 2 while reading, 0 once a result is in `out`.
 * Every failure is a non-fatal error result: nothing was changed. */
int view_image_begin(const AgentCall *, char *, size_t, AgentJournal, void *, uint32_t);
/* One <=16 KiB read per turn; stop concludes at once with an error result. */
int view_image_step(char *, size_t, uint32_t, int);
/* The image a completed view_image has not yet handed to the agent, once. */
const AgentImage *view_image_take(void);
/* The pixels for the request that follows the attach; NULL when none are held. */
const AgentImage *view_image_held(void);
#endif
