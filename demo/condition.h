#ifndef __CONDITION_H__
#define __CONDITION_H__

#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

struct condition {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    char done;
};

void condition_init(struct condition *cond);
void condition_post(struct condition *cond);
int condition_wait(struct condition *cond);
int condition_timedwait(struct condition *cond, int timeout_ms);
void condition_deinit(struct condition *cond);

#ifdef __cplusplus
}
#endif

#endif /* __CONDITION_H__ */
