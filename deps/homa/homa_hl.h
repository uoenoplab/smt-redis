#ifndef _ECHO_H
#define _ECHO_H

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

#include "homa.h"
//#include "homals/homals.h"

extern size_t recv_buf_size;
extern char *recv_buf_region;

extern char *homa_recv_buf[65536];

extern struct homa_recvmsg_args control;
extern struct msghdr hdr;

enum {
    ECHO_HOMA,
    ECHO_HOMALS,
    ECHO_HOMALSALT,
    ECHO_HOMALS12,
    ECHO_PROTO_NUM
};

int init_recv_args(int sockfd, void* addr, socklen_t addrlen);
int init_recv_args_per_conn(int sockfd, char **homa_recv_buf_region);

//extern int compare_float(const void * a, const void * b);
//
//extern double calculate_time_delta_us(struct timespec a, struct timespec b);
//extern double calculate_time_delta_s(struct timespec a, struct timespec b);

//extern int get_protocol(char const *protocol_name);
//extern void print_protocol_names();

#endif /* _ECHO_H */
