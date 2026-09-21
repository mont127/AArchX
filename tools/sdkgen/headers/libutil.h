#include <sys/types.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>

struct pidfh;
struct in_addr;
struct sockaddr;

int expand_number(const char *_buf, uint64_t *_num);
int humanize_number(char *_buf, size_t _len, int64_t _number,
    const char *_suffix, int _scale, int _flags);
int realhostname(char *host, size_t hsize, const struct in_addr *ip);
int realhostname_sa(char *host, size_t hsize, struct sockaddr *addr,
    int addrlen);
struct pidfh *pidfile_open(const char *path, mode_t mode, pid_t *pidptr);
int pidfile_write(struct pidfh *pfh);
int pidfile_close(struct pidfh *pfh);
int pidfile_remove(struct pidfh *pfh);
int reexec_to_match_kernel(void);
int reexec_to_match_lp64ness(bool isLP64);
