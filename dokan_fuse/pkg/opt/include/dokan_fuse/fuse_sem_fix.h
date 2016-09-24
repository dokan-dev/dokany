#ifndef FUSE_SEM_FIX_H_
#define FUSE_SEM_FIX_H_

#ifdef __CYGWIN__
#include <semaphore.h>

#ifdef __cplusplus
extern "C" {
#endif

int my_sem_init(sem_t *sem, int pshared, int initial);
int my_sem_destroy(sem_t *sem);
int my_sem_post (sem_t * sem);
int my_sem_wait (sem_t * sem);
#define sem_init my_sem_init
#define sem_destroy my_sem_destroy
#define sem_wait my_sem_wait
#define sem_post my_sem_post

#ifdef __cplusplus
};
#endif


#endif

#endif //FUSE_SEM_FIX_H_
