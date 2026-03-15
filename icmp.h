#ifndef ICMP_H
#define ICMP_H

/*
 * icmp_check — send a single ICMP echo request to target_ip and wait
 * up to 2 seconds for a reply.
 *
 * Returns  0 on success (host reachable).
 * Returns -1 if no reply arrives or on socket error.
 *
 * Requires CAP_NET_RAW / root privileges (SOCK_RAW).
 */
int icmp_check(const char *target_ip);

#endif /* ICMP_H */
