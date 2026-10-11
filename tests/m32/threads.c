#include <pthread.h>
#include <stdio.h>
static __thread int tls = 100;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static pthread_key_t key;
static int counter, ready, once_runs;
static void init_once(void) { once_runs++; }
static void *work(void *arg) {
    pthread_once(&once, init_once);
    tls += (int)(long)arg;
    pthread_setspecific(key, arg);
    for (int i = 0; i < 100000; i++) { pthread_mutex_lock(&mu); counter++; pthread_mutex_unlock(&mu); }
    pthread_mutex_lock(&mu); ready++; pthread_cond_signal(&cv); pthread_mutex_unlock(&mu);
    return (void *)(long)(tls + (int)(long)pthread_getspecific(key));
}
int main(void) {
    pthread_key_create(&key, NULL);
    pthread_t t[4];
    for (long i = 0; i < 4; i++) pthread_create(&t[i], NULL, work, (void *)(i + 1));
    pthread_mutex_lock(&mu); while (ready < 4) pthread_cond_wait(&cv, &mu); pthread_mutex_unlock(&mu);
    long sum = 0; for (int i = 0; i < 4; i++) { void *r; pthread_join(t[i], &r); sum += (long)r; }
    printf("counter %d once %d sum %ld main-tls %d\n", counter, once_runs, sum, tls);
    pthread_mutex_t m2; pthread_mutex_init(&m2, NULL); int a = pthread_mutex_trylock(&m2), b = pthread_mutex_trylock(&m2);
    printf("trylock %d %d\n", a, b != 0);
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 1 << 20); size_t ss = 0; pthread_attr_getstacksize(&at, &ss);
    printf("attr %zu self-eq %d\n", ss, pthread_equal(pthread_self(), pthread_self()));
    return 0;
}
