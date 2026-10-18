#pragma once
// RestartUnits.h — the `restart-units` job (#624): every unit of the wall is
// restarted, one at a time, through the `restart-unit` job each board has;
// the plan is RestartUnitsPlan.h, the worker task runs it. Started from the
// API's action and from the Home Assistant button alike.
#include <stdint.h>

void restartUnitsInit();
// The job's id in `op`, or the refusal with its HTTP status.
const char* restartUnitsStart(uint32_t& op, int& status);
bool restartUnitsRunning();
// Worker task, every pass.
void restartUnitsTick();
