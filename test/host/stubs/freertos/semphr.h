#ifndef HOST_SEMPHR_H
#define HOST_SEMPHR_H
#include <assert.h>
typedef int *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { static int locked; return &locked; }
static inline int xSemaphoreTake(SemaphoreHandle_t mutex, int delay) { (void)delay; assert(!*mutex); *mutex = 1; return 1; }
static inline int xSemaphoreGive(SemaphoreHandle_t mutex) { assert(*mutex); *mutex = 0; return 1; }
#endif
