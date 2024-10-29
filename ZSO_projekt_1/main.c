#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

#define NUM_EXPEDIENTS 3
#define NUM_PRODUCTS 5
#define INITIAL_NUM_CLIENTS 5

pthread_mutex_t expedient_mutex[NUM_EXPEDIENTS];
pthread_cond_t expedient_cond[NUM_EXPEDIENTS];
pthread_mutex_t helper_mutex;
pthread_cond_t helper_cond;
pthread_mutex_t products_mutex[NUM_PRODUCTS];
int products[NUM_PRODUCTS];
int products_needed[NUM_PRODUCTS];
int helper_busy = 0;
int NUM_CLIENTS = INITIAL_NUM_CLIENTS;

void* client(void* arg) {
    int id = *(int*)arg;
#ifdef DEBUG
    printf("Client %d entered the shop.\n", id);
#endif

    
    for (int i = 0; i < NUM_PRODUCTS; i++) {
        int expedient_id = id % NUM_EXPEDIENTS;

        pthread_mutex_lock(&expedient_mutex[expedient_id]);
#ifdef DEBUG
        printf("Client %d is being served by expedient %d for product %d.\n", id, expedient_id, i);
#endif

        pthread_mutex_lock(&products_mutex[i]);
        if (products[i] > 0) {
            products[i]--;
            pthread_mutex_unlock(&products_mutex[i]);
#ifdef DEBUG
            printf("Product %d given to client %d by expedient %d.\n", i, id, expedient_id);
#endif
        } else {
            pthread_mutex_unlock(&products_mutex[i]);
#ifdef DEBUG
            printf("Product %d is not available. Requesting helper's assistance.\n", i);
#endif

            pthread_mutex_lock(&helper_mutex);
            while (helper_busy) {
                pthread_cond_wait(&helper_cond, &helper_mutex);
            }
            helper_busy = 1;
            pthread_mutex_unlock(&helper_mutex);

#ifdef DEBUG
            usleep(100000);
#endif

            pthread_mutex_lock(&helper_mutex);
            pthread_mutex_lock(&products_mutex[i]);
            products[i] += products_needed[i];
            helper_busy = 0;
            pthread_cond_signal(&helper_cond);
            pthread_mutex_unlock(&products_mutex[i]);
            pthread_mutex_unlock(&helper_mutex);

            pthread_mutex_lock(&products_mutex[i]);
            products[i]--;
            pthread_mutex_unlock(&products_mutex[i]);
#ifdef DEBUG
            printf("Product %d given to client %d by expedient %d after helper's assistance.\n", i, id, expedient_id);
#endif
        }

        pthread_mutex_unlock(&expedient_mutex[expedient_id]);
    }

#ifdef DEBUG
    printf("Client %d is paying and leaving the shop.\n", id);
#endif
    return NULL;
}

void* expedient(void* arg) {
    int id = *(int*)arg;
#ifdef DEBUG
    printf("Expedient %d is ready to serve.\n", id);
#endif
    return NULL;
}

void* helper(void* arg) {
#ifdef DEBUG
    printf("Helper is ready to assist.\n");
#endif
    return NULL;
}

void projekt_zso() {
    for (int i = 0; i < NUM_PRODUCTS; i++) {
        products[i] = 5;  // Static value for product count
        products_needed[i] = 5;  // Static value for product replenishment
        pthread_mutex_init(&products_mutex[i], NULL);
    }

    for (int i = 0; i < NUM_EXPEDIENTS; i++) {
        pthread_mutex_init(&expedient_mutex[i], NULL);
        pthread_cond_init(&expedient_cond[i], NULL);
    }
    pthread_mutex_init(&helper_mutex, NULL);
    pthread_cond_init(&helper_cond, NULL);

    pthread_t expedients[NUM_EXPEDIENTS];
    pthread_t helper_thread;
    int expedient_ids[NUM_EXPEDIENTS];
    int client_ids[NUM_CLIENTS];

    for (int i = 0; i < NUM_EXPEDIENTS; i++) {
        expedient_ids[i] = i;
        pthread_create(&expedients[i], NULL, expedient, &expedient_ids[i]);
    }

    pthread_create(&helper_thread, NULL, helper, NULL);

    pthread_t clients[NUM_CLIENTS];

    for (int i = 0; i < NUM_CLIENTS; i++) {
        client_ids[i] = i;
        pthread_create(&clients[i], NULL, client, &client_ids[i]);
    }

    for (int i = 0; i < NUM_CLIENTS; i++) {
        pthread_join(clients[i], NULL);
    }

    for (int i = 0; i < NUM_EXPEDIENTS; i++) {
        pthread_join(expedients[i], NULL);
        pthread_mutex_destroy(&expedient_mutex[i]);
        pthread_cond_destroy(&expedient_cond[i]);
    }

    pthread_join(helper_thread, NULL);
    pthread_mutex_destroy(&helper_mutex);
    pthread_cond_destroy(&helper_cond);

    for (int i = 0; i < NUM_PRODUCTS; i++) {
        pthread_mutex_destroy(&products_mutex[i]);
    }

    printf("Shop is closed.\n");
}

int main() {
    for (int i = 0; i < 10; i++) {
        struct timespec start, end;
        clock_gettime(CLOCK_REALTIME, &start);

        projekt_zso();

        clock_gettime(CLOCK_REALTIME, &end);
        double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1000000000.0;
        printf("Iteration %d took %f seconds\n", i + 1, elapsed);

        if (elapsed > 5.0) {
            NUM_CLIENTS = (int)(NUM_CLIENTS * 0.9);
            printf("Reducing number of clients to %d\n", NUM_CLIENTS);
        } else if (elapsed < 4.0) {
            NUM_CLIENTS = (int)(NUM_CLIENTS * 1.1);
            printf("Increasing number of clients to %d\n", NUM_CLIENTS);
        }
    }
    return 0;
}
