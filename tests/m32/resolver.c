/* The resolver's structures from i386: getaddrinfo's list (i386 struct addrinfo, 32 bytes) freed with freeaddrinfo,
 * and gethostbyname's struct hostent. MW3 aborted in the host's freeaddrinfo. resolver.expected is the x86_64 build's
 * output under Rosetta. */
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

int main(void)
{
    struct addrinfo hints, *list = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_CANONNAME;
    int r = getaddrinfo("localhost", "8080", &hints, &list);
    printf("getaddrinfo %d\n", r);
    for (struct addrinfo *a = list; a; a = a->ai_next) {
        struct sockaddr_in *in = (struct sockaddr_in *)a->ai_addr;
        printf("  family %d socktype %d addrlen %u %s:%d canon %s\n", a->ai_family, a->ai_socktype, (unsigned)a->ai_addrlen,
               inet_ntoa(in->sin_addr), ntohs(in->sin_port), a->ai_canonname ? a->ai_canonname : "-");
    }
    freeaddrinfo(list);
    printf("missing %d\n", getaddrinfo("no-such-host.invalid", NULL, &hints, &list) != 0);
    struct hostent *h = gethostbyname("localhost");
    printf("gethostbyname %s type %d length %d first %s\n", h ? h->h_name : "-", h ? h->h_addrtype : 0, h ? h->h_length : 0,
           h && h->h_addr_list[0] ? inet_ntoa(*(struct in_addr *)h->h_addr_list[0]) : "-");
    return 0;
}
