#ifndef SHIM_PTHREAD_H
#define SHIM_PTHREAD_H
/* Minimal <pthread.h> for compile probes: the kernel is effectively
 * cooperatively multitasked; real thread support is out of scope for
 * the JS engines. Only declarations the engine sources reference. */
typedef unsigned long pthread_t;
typedef struct { int dummy; } pthread_mutex_t;
typedef struct { int dummy; } pthread_mutexattr_t;
typedef struct { int dummy; } pthread_cond_t;
typedef struct { int dummy; } pthread_condattr_t;
typedef struct { int dummy; } pthread_attr_t;
typedef unsigned int pthread_key_t;
#define PTHREAD_MUTEX_INITIALIZER { 0 }
#define PTHREAD_COND_INITIALIZER { 0 }
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a);
int pthread_mutex_destroy(pthread_mutex_t *m);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_unlock(pthread_mutex_t *m);
int pthread_mutex_trylock(pthread_mutex_t *m);
int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a);
int pthread_cond_destroy(pthread_cond_t *c);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);
int pthread_create(pthread_t *t, const pthread_attr_t *a,
                   void *(*start)(void *), void *arg);
int pthread_join(pthread_t t, void **retval);
pthread_t pthread_self(void);
int pthread_key_create(pthread_key_t *key, void (*dtor)(void *));
int pthread_key_delete(pthread_key_t key);
void *pthread_getspecific(pthread_key_t key);
int pthread_setspecific(pthread_key_t key, const void *value);
int pthread_once(void *once_control, void (*init)(void));
#endif
