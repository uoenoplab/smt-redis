#include "homa_hl.h"

size_t recv_buf_size = 1000*HOMA_BPAGE_SIZE;
char *recv_buf_region;

struct homa_recvmsg_args control;
struct msghdr hdr;

static const char* protocol_names[ECHO_PROTO_NUM] = {
    "homa", "homals", "homalsalt", "homals12"
};

int init_recv_args(int sockfd, void* addr, socklen_t addrlen) {
    struct homa_set_buf_args arg;

    // Set up buffer region.
    recv_buf_region = (char *) mmap(NULL, recv_buf_size, PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS, 0, 0);
    if (recv_buf_region == MAP_FAILED) {
        printf("Couldn't mmap buffer region: %s\n", strerror(errno));
        return -1;
    }
//    printf("Recv buf size: %ld\n", recv_buf_size);

    arg.start = recv_buf_region;
    arg.length = recv_buf_size;

    if (setsockopt(sockfd, IPPROTO_HOMA, SO_HOMA_SET_BUF, &arg, sizeof(arg)) < 0) {
        printf("Error in setsockopt(SO_HOMA_SET_BUF): %s\n",
                strerror(errno));
        return -1;
    }

    memset(&hdr, 0, sizeof(hdr));
    hdr.msg_name = addr;
    hdr.msg_namelen = addrlen;
    hdr.msg_control = &control;
    hdr.msg_controllen = sizeof(control);
    memset(&control, 0, sizeof(control));

    return 0;
}

int init_recv_args_per_conn(int sockfd, char **homa_recv_buf_region) {
    struct homa_set_buf_args arg;

    // Set up buffer region.
    *homa_recv_buf_region = (char *) mmap(NULL, recv_buf_size, PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS, 0, 0);
    if (recv_buf_region == MAP_FAILED) {
        printf("Couldn't mmap buffer region: %s\n", strerror(errno));
        return -1;
    }
//    printf("Recv buf size: %ld\n", recv_buf_size);

    arg.start = *homa_recv_buf_region;
    arg.length = recv_buf_size;

    if (setsockopt(sockfd, IPPROTO_HOMA, SO_HOMA_SET_BUF, &arg, sizeof(arg)) < 0) {
        printf("Error in setsockopt(SO_HOMA_SET_BUF): %s\n",
                strerror(errno));
        return -1;
    }

    return 0;
}


double calculate_time_delta_us(struct timespec a, struct timespec b) {
    double delta = (a.tv_sec - b.tv_sec) * 1000000.0 + (a.tv_nsec - b.tv_nsec) / 1000.0;
    if (delta < 0) delta = -delta;
    return delta;
}

double calculate_time_delta_s(struct timespec a, struct timespec b) {
    double delta = (a.tv_sec - b.tv_sec) * 1.0 + (a.tv_nsec - b.tv_nsec) / 1000000000.0;
    if (delta < 0) delta = -delta;
    return delta;
}

int compare_float(const void * a, const void * b) {
  return (*(float*)a > *(float*)b) ? 1 : (*(float*)a < *(float*)b) ? -1:0 ;
}

int get_protocol(char const *protocol_name) {
    size_t i;
    for (i = 0; i < ECHO_PROTO_NUM; i++) {
        if (strcmp(protocol_name, protocol_names[i]) == 0) break;
    }
    return i;
}

void print_protocol_names() {
    int nameslen = 0;
    for (size_t i = 0; i < ECHO_PROTO_NUM; i++) {
        nameslen += strlen(protocol_names[i]) + 1;
    }

    char names[nameslen];

    names[0] = 0;
    for (size_t i = 0; i < ECHO_PROTO_NUM; i++) {
        strcat(names, protocol_names[i]);
        if (i != ECHO_PROTO_NUM - 1) strcat(names, " ");
    }

    printf("Unsupported protocol! (choose from %s)\n", names);
}
