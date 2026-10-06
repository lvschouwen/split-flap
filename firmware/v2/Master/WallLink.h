#pragma once
// WallLink.h — the master's end of the wall link (#559/#566): the link task
// listens on WALL_LINK_PORT and holds one TCP connection per row board, opened
// by the row. The master never dials a row.
//
// The link task is the only toucher of these sockets. What a connection means
// is WallLinkCore.h (natively tested); this is the lwIP glue around it. What
// the rows say is published to WallState.

#include <Arduino.h>

// setup(), before tasksInit(): the name this master gives in Welcome.
void wallLinkInit(const String& masterId);

// The link task's body; never returns.
void wallLinkTaskMain(void*);
