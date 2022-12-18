#include <fcntl.h>
#include <mqueue.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define NEVENTS 10
#define BUFSIZE 8192 - sizeof(struct timespec)
#define MQUEUE "/sample1"
#define QUEUE_SIZE 10

typedef struct {
    int fd;
    void *data;
} EVHANDLE;

typedef struct {
    struct timespec send_ts;
    char data[BUFSIZE];
} user_data_t;

typedef struct {
    user_data_t data[QUEUE_SIZE];
    size_t index;
    size_t num;
} queue_t;

pthread_mutex_t q_mut = PTHREAD_MUTEX_INITIALIZER;
queue_t ev_queue;
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

void init(queue_t *q) {
    size_t i;

    for (i = 0; i < QUEUE_SIZE; i++) {
        memset(&q->data[i], 0, sizeof(user_data_t));
    }

    q->index = q->num = 0;
}

void increment(queue_t *q) {
    q->index++;

    if (q->index == QUEUE_SIZE) {
        q->index = 0;
    }
}

bool enqueue(queue_t *q, user_data_t *data) {
    if (q->num >= QUEUE_SIZE) {
        fprintf(stderr, "Error: queue is full.\n");
        return false;
    }

    memcpy(&q->data[(q->index + q->num) % QUEUE_SIZE], data,
           sizeof(user_data_t));
    q->num++;

    return true;
}

bool dequeue(queue_t *q, user_data_t *data) {
    if (q->num == 0) {
        fprintf(stderr, "Error: queue is empty.\n");
        return false;
    }

    memcpy(data, &q->data[q->index % QUEUE_SIZE], sizeof(user_data_t));
    increment(q);
    q->num--;

    return true;
}

void clear(queue_t *q) { init(q); }

#define LOOP 10000
#define USLEEP 1000
void *sender(void *arg) {
    int *efd = (int *)arg;
    mqd_t mqd;

    mqd = mq_open(MQUEUE, O_WRONLY | O_CREAT, 0644, NULL);
    if (mqd < 0) {
        perror("mq_open");
        return NULL;
    }

    usleep(USLEEP);

    /* mqueue */
    for (int i; i < LOOP; i++) {
        user_data_t send_data;
        memset(send_data.data, i, sizeof(send_data.data));
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &send_data.send_ts);
        if (-1 == mq_send(mqd, (char *)&send_data, sizeof(user_data_t), 0)) {
            perror("mq_send");
        }
        usleep(USLEEP);
    }

    /* eventfd */
    eventfd_t val_ev = 1;
    for (int i; i < LOOP; i++) {
        user_data_t send_data;
        memset(send_data.data, i, sizeof(send_data.data));
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &send_data.send_ts);
        pthread_mutex_lock(&q_mut);
        enqueue(&ev_queue, &send_data);
        pthread_mutex_unlock(&q_mut);
        if (-1 == eventfd_write(*efd, val_ev)) {
            perror("eventfd_write");
        }
        usleep(USLEEP);
    }

    for (int i; i < LOOP; i++) {
        user_data_t send_data;
        memset(send_data.data, i, sizeof(send_data.data));
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &send_data.send_ts);
        pthread_mutex_lock(&q_mut);
        enqueue(&ev_queue, &send_data);
        pthread_cond_signal(&cond);
        pthread_mutex_unlock(&q_mut);
        usleep(USLEEP);
    }

    close(*efd);
    mq_close(mqd);
    mq_unlink(MQUEUE);
    exit(0);
    return NULL;
}

void *io_thread(void *arg) {
    while (1) {
        user_data_t recv_data;
        pthread_mutex_lock(&q_mut);
        pthread_cond_wait(&cond, &q_mut);
        dequeue(&ev_queue, &recv_data);
        pthread_mutex_unlock(&q_mut);
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        printf("%ld.%09ld,%ld.%09ld,cond_signal\n", recv_data.send_ts.tv_sec,
               recv_data.send_ts.tv_nsec, ts.tv_sec, ts.tv_nsec);
    }
}

int main() {
    int epfd;
    struct epoll_event ev, ev_ret[NEVENTS];
    mqd_t mqd;
    int efd;

    init(&ev_queue);

    epfd = epoll_create(1);
    if (epfd < 0) {
        perror("epoll_create");
        return -1;
    }

    mq_unlink(MQUEUE);
    struct mq_attr attr = {0, 10, sizeof(user_data_t), 0};
    mqd = mq_open(MQUEUE, O_RDONLY | O_CREAT, 0644, &attr);
    if (mqd < 0) {
        perror("mq_open");
        return -1;
    }

    EVHANDLE ev_mq;
    ev_mq.fd = mqd;
    ev_mq.data = &ev_queue;
    ev.events = EPOLLIN;
    ev.data.ptr = &ev_mq;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, mqd, &ev) != 0) {
        perror("epoll_ctl");
        return -1;
    }

    efd = eventfd(0, 0);
    if (efd < 0) {
        perror("eventfd");
        return -1;
    }

    EVHANDLE ev_evd;
    ev_evd.fd = efd;
    ev_evd.data = &ev_queue;
    ev.events = EPOLLIN;
    ev.data.ptr = &ev_evd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, efd, &ev) != 0) {
        perror("epoll_ctl");
        return -1;
    }

    pthread_t io_th;
    if (0 != pthread_create(&io_th, NULL, io_thread, NULL)) {
        perror("pthread_create");
        return -1;
    }

    pthread_t send_th;
    if (0 != pthread_create(&send_th, NULL, sender, &efd)) {
        perror("pthread_create");
        return -1;
    }

    while (1) {
        int nfds;
        nfds = epoll_wait(epfd, ev_ret, NEVENTS, -1);
        if (nfds <= 0) {
            perror("epoll_wait");
            return -1;
        }
        for (int i = 0; i < nfds; i++) {
            EVHANDLE *ev = ev_ret[i].data.ptr;
            if (ev->fd == mqd) {
                user_data_t recv_data;
                if (-1 == mq_receive(ev->fd, (char *)&recv_data,
                                     sizeof(user_data_t), NULL)) {
                    perror("mq_receive");
                    return -1;
                }
                struct timespec ts;
                clock_gettime(CLOCK_MONOTONIC, &ts);
                printf("%ld.%09ld,%ld.%09ld,mq\n", recv_data.send_ts.tv_sec,
                       recv_data.send_ts.tv_nsec, ts.tv_sec, ts.tv_nsec);
            }
            if (ev->fd == efd) {
                eventfd_t val;
                if (-1 == eventfd_read(ev->fd, &val)) {
                    perror("eventfd_read");
                    return -1;
                }
                user_data_t recv_data;
                pthread_mutex_lock(&q_mut);
                dequeue((queue_t *)ev->data, &recv_data);
                pthread_mutex_unlock(&q_mut);
                struct timespec ts;
                clock_gettime(CLOCK_MONOTONIC, &ts);
                printf("%ld.%09ld,%ld.%09ld,eventfd\n",
                       recv_data.send_ts.tv_sec, recv_data.send_ts.tv_nsec,
                       ts.tv_sec, ts.tv_nsec);
            }
        }
    }
    pthread_join(send_th, NULL);
    pthread_join(io_th, NULL);
    mq_unlink(MQUEUE);
    return 0;
}