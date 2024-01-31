#ifndef _HOMALS_H
#define _HOMALS_H

#include <linux/tls.h>

#ifdef __cplusplus
extern "C" {
#endif

struct homals_crypto_info {
  struct tls12_crypto_info_aes_gcm_128 crypto_info_aes_gcm_128;
  uint32_t addr;  // network order
  uint16_t port;  // network order
  uint8_t reuse;
  uint8_t padding;
};

extern void set_crypto_info(
    struct tls12_crypto_info_aes_gcm_128 *crypto_info_send,
    struct tls12_crypto_info_aes_gcm_128 *crypto_info_read, int server);
extern void set_crypto_info_alter(
    struct tls12_crypto_info_aes_gcm_128 *crypto_info_send,
    struct tls12_crypto_info_aes_gcm_128 *crypto_info_read,
    int server);  // for 2 echo client with different keys
extern void set_crypto_info_tls12(
    struct tls12_crypto_info_aes_gcm_128 *crypto_info_send,
    struct tls12_crypto_info_aes_gcm_128 *crypto_info_read, int server);

extern int homals_setsockopt_wrapper(int sockfd, uint32_t addr, uint16_t port,
                                     int server, int tls13);
extern int tcpktls_setsockopt_wrapper(int sockfd, int server, int tls13);

#ifdef __cplusplus
}
#endif

#endif /* _HOMALS_H */
