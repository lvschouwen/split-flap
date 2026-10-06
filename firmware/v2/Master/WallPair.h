#pragma once
// WallPair.h — runs the operator's requests that change which boards make up
// this Split-Flap (#559/#566): pairing a row, releasing one, arranging the
// wall. The web side stages a request in WallState and gets a job id; this
// runs it on the worker task, because pairing is a blocking HTTP call to the
// row (POST /pair, form field `master`) that neither the web server's task
// nor the link task may wait for.
//
// It also pairs again, unasked: a row in the table that the link has lost is
// posted the pairing once per lost mark. That is how a row finds a master
// whose address moved (the row takes its own master's id from a new address
// once the old one has been silent for 25 s); to a row that is merely off it
// is a connection refused.

#include <Arduino.h>

// setup(): the id this master pairs with (its name).
void wallPairInit(const String& masterId);

// Worker task, every pass. Blocks for at most one HTTP timeout.
void wallPairTick();
