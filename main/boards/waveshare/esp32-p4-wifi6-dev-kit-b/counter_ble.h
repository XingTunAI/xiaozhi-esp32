#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif
void StartCounterBle(void);
int CounterBleApprovalState(void);
void CounterBleApprove(void);
#ifdef __cplusplus
}
#endif
