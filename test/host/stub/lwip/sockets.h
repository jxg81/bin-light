#pragma once
// The host has real BSD sockets; captive_dns.c only needs lwip's spelling of
// them. Nothing here is exercised by the tests - they drive build_reply()
// directly on byte buffers - but it has to compile.
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

#define ipaddr_addr(s) inet_addr(s)
