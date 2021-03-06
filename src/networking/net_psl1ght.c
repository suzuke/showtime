/*
 *  Showtime Mediacenter
 *  Copyright (C) 2007-2013 Lonelycoder AB
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  This program is also available under a commercial proprietary license.
 *  For more information, contact andreas@lonelycoder.com
 */

#include "config.h"
#include <netdb.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/net.h>
#include <net/netctl.h>
#include <errno.h>

#include "showtime.h"
#include "net_i.h"


#if ENABLE_HTTPSERVER
#include "http_server.h"
#include "ssdp.h"
#endif


#if ENABLE_POLARSSL
/**
 *
 */
static int
polarssl_read(tcpcon_t *tc, void *buf, size_t len, int all,
	      net_read_cb_t *cb, void *opaque)
{
  int ret, tot = 0;
  if(!all) {
    ret = ssl_read(tc->ssl, buf, len);
    return ret > 0 ? ret : -1;
  }

  while(tot != len) {
    ret = ssl_read(tc->ssl, buf + tot, len - tot);
    if(ret < 0) 
      return -1;
    tot += ret;
    if(cb != NULL)
      cb(opaque, tot);
  }
  return tot;
}


/**
 *
 */
static int
polarssl_write(tcpcon_t *tc, const void *data, size_t len)
{
  return ssl_write(tc->ssl, data, len) != len ? ECONNRESET : 0;
}


#endif

/**
 *
 */
static int
tcp_write(tcpcon_t *tc, const void *data, size_t len)
{
#ifdef MSG_NOSIGNAL
  return netSend(tc->fd, data, len, MSG_NOSIGNAL) != len ? ECONNRESET : 0;
#else
  return netSend(tc->fd, data, len, 0           ) != len ? ECONNRESET : 0;
#endif
}


/**
 *
 */
static int
tcp_read(tcpcon_t *tc, void *buf, size_t len, int all,
	 net_read_cb_t *cb, void *opaque)
{
  int x;
  size_t off = 0;
  while(1) {
    size_t r = len - off;
    x = netRecv(tc->fd, buf + off, r, 0);
    if(x < 1)
      return -1;
    
    if(all) {

      off += x;
      if(off == len)
	return len;

      if(cb != NULL)
	cb(opaque, off);

    } else {
      return x;
    }
  }
}


/**
 *
 */
static int
getstreamsocket(int family, char *errbuf, size_t errbufsize)
{
  int fd, optval, r;

  fd = netSocket(family, SOCK_STREAM, IPPROTO_TCP);
  if(fd < 0) {
    snprintf(errbuf, errbufsize, "Unable to create socket: %s",
	     strerror(net_errno));
    return -1;  
  }

  /**
   * Switch to nonblocking
   */
  optval = 1;
  r = netSetSockOpt(fd, SOL_SOCKET, SO_NBIO, &optval, sizeof(optval));
  if(r < 0) {
    snprintf(errbuf, errbufsize, "Unable to go nonblocking: %s",
	     strerror(net_errno));
    netClose(fd);
    return -1;
  }

  optval = 1;
  if(netSetSockOpt(fd, 6, 1, &optval, sizeof(optval)) < 0)
    TRACE(TRACE_INFO, "TCP", "Unable to turn on TCP_NODELAY");

  return fd;
}


/**
 *
 */
tcpcon_t *
tcp_connect(const char *hostname, int port, char *errbuf, size_t errbufsize,
	    int timeout, int ssl, cancellable_t *c)
{
  struct net_hostent *hp;
  char *tmphstbuf;
  int fd, r, err, herr, optval;
  const char *errtxt;
  struct sockaddr_in in;
  socklen_t errlen = sizeof(int);


  if(!strcmp(hostname, "localhost")) {
    if((fd = getstreamsocket(AF_INET, errbuf, errbufsize)) == -1)
      return NULL;

    memset(&in, 0, sizeof(in));
    in.sin_family = AF_INET;
    in.sin_port = htons(port);
    in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    r = netConnect(fd, (struct sockaddr *)&in, sizeof(struct sockaddr_in));
  } else {

    herr = 0;
    tmphstbuf = NULL; /* free NULL is a nop */
    hp = netGetHostByName(hostname);
    if(hp == NULL)
      herr = h_errno;

    if(herr != 0) {
      switch(herr) {
      case HOST_NOT_FOUND:
	errtxt = "Unknown host";
	break;

      case NO_ADDRESS:
	errtxt = "The requested name is valid but does not have an IP address";
	break;
      
      case NO_RECOVERY:
	errtxt = "A non-recoverable name server error occurred";
	break;
      
      case TRY_AGAIN:
	errtxt = "A temporary error occurred on an authoritative name server";
	break;
      
      default:
	errtxt = "Unknown error";
	break;
      }

      snprintf(errbuf, errbufsize, "%s", errtxt);
      free(tmphstbuf);
      return NULL;
    } else if(hp == NULL) {
      snprintf(errbuf, errbufsize, "Resolver internal error");
      free(tmphstbuf);
      return NULL;
    }

    if((fd = getstreamsocket(hp->h_addrtype, errbuf, errbufsize)) == -1) {
      free(tmphstbuf);
      return NULL;
    }

    switch(hp->h_addrtype) {
    case AF_INET:
      memset(&in, 0, sizeof(in));
      in.sin_family = AF_INET;
      in.sin_port = htons(port);
      lv2_void* netaddrlist = (lv2_void*)(u64)hp->h_addr_list;
      memcpy(&in.sin_addr, (char*)(u64)netaddrlist[0], sizeof(struct in_addr));
      r = netConnect(fd, (struct sockaddr *)&in, sizeof(struct sockaddr_in));
      break;

    default:
      snprintf(errbuf, errbufsize, "Invalid protocol family");
      free(tmphstbuf);
      return NULL;
    }

    free(tmphstbuf);
  }



  tcpcon_t *tc = calloc(1, sizeof(tcpcon_t));
  tc->fd = fd;
  tc->c = c;
  htsbuf_queue_init(&tc->spill, 0);

  if(c != NULL)
    cancellable_bind(c, tcp_cancel, tc);



  if(r < 0) {
    if(net_errno == NET_EINPROGRESS) {

      struct pollfd pfd;
      pfd.fd = fd;
      pfd.events = POLLOUT | POLLERR;
      pfd.revents = 0;

      r = netPoll(&pfd, 1, timeout);
      if(r == 0) {
	/* Timeout */
	snprintf(errbuf, errbufsize, "Connection attempt timed out");
        tcp_close(tc);
	return NULL;
      }
      
      if(r == -1) {
	snprintf(errbuf, errbufsize, "poll() error: %s", 
		 strerror(net_errno));
        tcp_close(tc);
	return NULL;
      }

      if(pfd.revents & POLLERR) {
	snprintf(errbuf, errbufsize, "Connection refused");
        tcp_close(tc);
	return NULL;
      }

      netGetSockOpt(fd, SOL_SOCKET, SO_ERROR, (void *)&err, &errlen);
    } else {
      err = net_errno;
    }
  } else {
    err = 0;
  }

  if(err != 0) {
    snprintf(errbuf, errbufsize, "%s", strerror(err));
    tcp_close(tc);
    return NULL;
  }
  
  optval = 0;
  r = netSetSockOpt(fd, SOL_SOCKET, SO_NBIO, &optval, sizeof(optval));
  if(r < 0) {
    snprintf(errbuf, errbufsize, "Unable to go blocking: %s",
	     strerror(net_errno));
    netClose(fd);
    return NULL;
  }

  if(ssl) {
#if ENABLE_POLARSSL
    if(1) {
      tc->ssl = malloc(sizeof(ssl_context));
      if(ssl_init(tc->ssl)) {
	snprintf(errbuf, errlen, "SSL failed to initialize");
        tcp_close(tc);
	return NULL;
      }

      tc->ssn = malloc(sizeof(ssl_session));
      tc->hs = malloc(sizeof(havege_state));

      havege_init(tc->hs);
      memset(tc->ssn, 0, sizeof(ssl_session));


      ssl_set_endpoint(tc->ssl, SSL_IS_CLIENT );
      ssl_set_authmode(tc->ssl, SSL_VERIFY_NONE );

      ssl_set_rng(tc->ssl, havege_random, tc->hs );
      ssl_set_bio(tc->ssl, net_recv, &tc->fd, net_send, &tc->fd);
      ssl_set_ciphersuites(tc->ssl, ssl_default_ciphersuites );
      ssl_set_session(tc->ssl, tc->ssn );
      
      tc->read = polarssl_read;
      tc->write = polarssl_write;
      
    } else
#endif
    {

      snprintf(errbuf, errlen, "SSL not supported");
      tcp_close(tc);
      return NULL;
    }
  } else {
    tc->read = tcp_read;
    tc->write = tcp_write;
  }

  return tc;
}



/**
 *
 */
void
tcp_huge_buffer(tcpcon_t *tc)
{
  int v = 128 * 1024;
  int r = netSetSockOpt(tc->fd, SOL_SOCKET, SO_RCVBUF, &v, sizeof(v));
  if(r < 0)
    TRACE(TRACE_ERROR, "TCP", "Unable to increase RCVBUF");
}


/**
 *
 */
void
tcp_set_read_timeout(tcpcon_t *tc, int ms)
{
  struct timeval tv;
  tv.tv_sec  = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  if(netSetSockOpt(tc->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
    TRACE(TRACE_ERROR, "TCP", "Unable to set RCVTIMO");
}



/**
 *
 */
void
tcp_close(tcpcon_t *tc)
{
#if ENABLE_POLARSSL
  if(tc->ssl != NULL) {
    ssl_close_notify(tc->ssl);
    ssl_free(tc->ssl);

    free(tc->ssl);
    free(tc->ssn);
    free(tc->hs);
  }
#endif
  cancellable_unbind(tc->c);
  htsbuf_queue_flush(&tc->spill);
  netClose(tc->fd);
  free(tc);
}



void
tcp_shutdown(tcpcon_t *tc)
{
  shutdown(tc->fd, SHUT_RDWR);
}


/**
 *
 */
tcpcon_t *
tcp_from_fd(int fd)
{
  tcpcon_t *tc = calloc(1, sizeof(tcpcon_t));
  tc->fd = fd & ~0x40000000;
  htsbuf_queue_init(&tc->spill, 0);
  tc->read = tcp_read;
  tc->write = tcp_write;
  return tc;
}




/**
 *
 */
netif_t *
net_get_interfaces(void)
{
  union net_ctl_info info;

  if(netCtlGetInfo(NET_CTL_INFO_IP_ADDRESS, &info))
    return NULL;

  netif_t *ni = calloc(2, sizeof(netif_t));

  snprintf(ni[0].ifname, sizeof(ni[0].ifname), "eth");
  ni[0].ipv4 = inet_addr(info.ip_address);
  return ni;
}


/**
 * Called from code in arch/
 */
void
net_initialize(void)
{
}


/**
 *
 */
void
net_change_nonblocking(int fd, int on)
{
  int optval = on;
  netSetSockOpt(fd, SOL_SOCKET, SO_NBIO, &optval, sizeof(optval));
}
