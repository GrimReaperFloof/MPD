/* the Music Player Daemon (MPD)
 * (c)2003-2004 by Warren Dukes (shank@mercury.chem.pitt.edu)
 * This project's homepage is: http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "inputStream_http.h"

#include "utils.h"
#include "log.h"
#include "conf.h"

#include <stdio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/param.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#define HTTP_CONN_STATE_CLOSED  0
#define HTTP_CONN_STATE_INIT    1
#define HTTP_CONN_STATE_HELLO   2
#define HTTP_CONN_STATE_OPEN    3
#define HTTP_CONN_STATE_REOPEN  4

#define HTTP_BUFFER_SIZE        131072
#define HTTP_PREBUFFER_SIZE	(HTTP_BUFFER_SIZE >> 2)
//#define HTTP_PREBUFFER_SIZE	0

#define HTTP_REDIRECT_MAX    10

typedef struct _InputStreemHTTPData {
        char * host;
        char * path;
        int port;
        int sock;
        int connState;
        char buffer[HTTP_BUFFER_SIZE];
        size_t buflen;
        int timesRedirected;
        int icyMetaint;
	int prebuffer;
	int icyOffset;
} InputStreamHTTPData;

void inputStream_initHttp() {
	if(getConf()[CONF_HTTP_PROXY_HOST]) {
		char * portStr = getConf()[CONF_HTTP_PROXY_PORT];
		int port = 0;
		char * test;

		if(!portStr) {
			ERROR("http_proxy_host specified but not the http_"
				"proxy_port\n");
			exit(EXIT_FAILURE);
		}

		port = strtol(portStr, &test, 10);
		if(port <= 0 || *test != '\0') {
			ERROR("http_proxy_port \"%s\" is not a positive integer"
				"\n", portStr);
		}

		if(getConf()[CONF_HTTP_PROXY_USER] && 
			!getConf()[CONF_HTTP_PROXY_PASSWORD])
		{
			ERROR("http_proxy_user specified, but not http_proxy_"
				"password\n");
			exit(EXIT_FAILURE);
		}

		if(getConf()[CONF_HTTP_PROXY_PASSWORD] && 
			!getConf()[CONF_HTTP_PROXY_USER])
		{
			ERROR("http proxy password specified, but not http "
					"proxy user\n");
			exit(EXIT_FAILURE);
		}
	}
	else if(getConf()[CONF_HTTP_PROXY_PORT]) {
		ERROR("http_proxy_port specified but not http_proxy_host\n");
		exit(EXIT_FAILURE);
	}
	else if(getConf()[CONF_HTTP_PROXY_USER]) {
		ERROR("http_proxy_user specified but not http_proxy_host\n");
		exit(EXIT_FAILURE);
	}
	else if(getConf()[CONF_HTTP_PROXY_PASSWORD]) {
		ERROR("http_proxy_password specified but not http_proxy_host"
				"\n");
		exit(EXIT_FAILURE);
	}
}

static InputStreamHTTPData * newInputStreamHTTPData() {
        InputStreamHTTPData * ret = malloc(sizeof(InputStreamHTTPData));

        ret->host = NULL;
        ret->path = NULL;
        ret->port = 80;
        ret->connState = HTTP_CONN_STATE_CLOSED;
        ret->timesRedirected = 0;
        ret->icyMetaint = 0;
	ret->prebuffer = 0;
	ret->icyOffset = 0;

        return ret;
}

static void freeInputStreamHTTPData(InputStreamHTTPData * data) {
        if(data->host) free(data->host);
        if(data->path) free(data->path);

        free(data);
}

static int parseUrl(InputStreamHTTPData * data, char * url) {
        char * temp;
        char * colon;
        char * slash;
        int len;

        if(strncmp("http://",url,strlen("http://"))!=0) return -1;

        temp = url+strlen("http://");

        slash = strchr(temp, '/');
        colon = strchr(temp, ':');

        if(slash && colon && slash <= colon) return -1;

        /* fetch the host portion */
        if(colon) len = colon-temp+1;
        else if(slash) len = slash-temp+1;
        else len = strlen(temp)+1;

        if(len<=1) return -1;

        data->host = malloc(len);
        strncpy(data->host,temp,len-1);
        data->host[len-1] = '\0';

        /* fetch the port */
        if(colon && (!slash || slash != colon+1)) {
                char * test;
                data->port = strtol(colon+1,&test,10);

                if(data->port <= 0 || (*test != '\0' && *test != '/')) {
                        return -1;
                }
        }

        /* fetch the path */
        data->path = strdup(slash ? slash : "/");

        return 0;
}

static int initHTTPConnection(InputStream * inStream) {
        struct hostent * he;
        struct sockaddr * dest;
        socklen_t destlen;
        struct sockaddr_in sin;
        InputStreamHTTPData * data = (InputStreamHTTPData *)inStream->data;
        int flags;
        int ret;
#ifdef HAVE_IPV6
        struct sockaddr_in6 sin6;
#endif

        if(!(he = gethostbyname(data->host))) {
                return -1;
        }

        memset(&sin,0,sizeof(struct sockaddr_in));
        sin.sin_family = AF_INET;
        sin.sin_port = htons(data->port);
#ifdef HAVE_IPV6
        memset(&sin6,0,sizeof(struct sockaddr_in6));
        sin6.sin6_family = AF_INET6;
        sin6.sin6_port = sin.sin_port;
#endif

        switch(he->h_addrtype) {
        case AF_INET:
                memcpy((char *)&sin.sin_addr.s_addr,(char *)he->h_addr,
                                he->h_length);
                dest = (struct sockaddr *)&sin;
                destlen = sizeof(struct sockaddr_in);
                break;
#ifdef HAVE_IPV6
        case AF_INET6:
                if(!ipv6Supported()) {
                        return -1;
                }
                memcpy((char *)&sin6.sin6_addr.s6_addr,(char *)he->h_addr,
                                he->h_length);
                dest = (struct sockaddr *)&sin6;
                destlen = sizeof(struct sockaddr_in6);
                break;
#endif
        default:
                return -1;
        }

        if((data->sock = socket(dest->sa_family,SOCK_STREAM,0)) < 0) {
                return -1;
        }

        flags = fcntl(data->sock, F_GETFL, 0);
        fcntl(data->sock, F_SETFL, flags | O_NONBLOCK);

        ret = connect(data->sock,dest,destlen);
        if(ret < 0 && errno!=EINPROGRESS) {
                close(data->sock);
                return -1;
        }

        data->connState = HTTP_CONN_STATE_INIT;

        data->buflen = 0;

        return 0;
}

static int finishHTTPInit(InputStream * inStream) {
        InputStreamHTTPData * data = (InputStreamHTTPData *)inStream->data;
        struct timeval tv;
        fd_set writeSet;
        fd_set errorSet;
        int error;
        int error_len = sizeof(int);
        int ret;
        char request[2049];

        tv.tv_sec = 0;
        tv.tv_usec = 0;

        FD_ZERO(&writeSet);
        FD_ZERO(&errorSet);
        FD_SET(data->sock, &writeSet);
        FD_SET(data->sock, &errorSet);

        ret = select(data->sock+1, NULL, &writeSet, &errorSet, &tv);

        if(ret == 0 || (ret < 0 && errno==EINTR))  return 0;

        if(ret < 0) {
                close(data->sock);
                data->connState = HTTP_CONN_STATE_CLOSED;
                return -1;
        }

        getsockopt(data->sock, SOL_SOCKET, SO_ERROR, &error, &error_len);
        if(error) {
                close(data->sock);
                data->connState = HTTP_CONN_STATE_CLOSED;
                return -1;
        }

        memset(request, 0, 2049);
	/* deal with ICY metadata later, for now its fucking up stuff! */
        snprintf(request, 2048, "GET %s HTTP/1.1\r\n"
                             "Host: %s\r\n"
                             "Connection: close\r\n"
                             "User-Agent: %s/%s\r\n"
                             "Range: bytes=%ld-\r\n"
                             "Icy-Metadata:1\r\n"
                             "\r\n",
                             data->path, data->host, "httpTest", "0.0.0",
                             inStream->offset);

        ret = write(data->sock, request, strlen(request));
        if(ret!=strlen(request)) {
                close(data->sock);
                data->connState = HTTP_CONN_STATE_CLOSED;
                return -1;
        }

        data->connState = HTTP_CONN_STATE_HELLO;

        return 0;
}

static int getHTTPHello(InputStream * inStream) {
        InputStreamHTTPData * data = (InputStreamHTTPData *)inStream->data;
        fd_set readSet;
        struct timeval tv;
        int ret;
        char * needle;
        char * cur = data->buffer;
        int rc;
        long readed;

        FD_ZERO(&readSet);
        FD_SET(data->sock, &readSet);
        
        tv.tv_sec = 0;
        tv.tv_usec = 0;

        ret = select(data->sock+1,&readSet,NULL,NULL,&tv);
        
        if(ret == 0 || (ret < 0 && errno==EINTR)) return 0;

        if(ret < 0) {
                data->connState = HTTP_CONN_STATE_CLOSED;
                close(data->sock);
                data->buflen = 0;
                return -1;
        }

        if(data->buflen >= HTTP_BUFFER_SIZE-1) {
                data->connState = HTTP_CONN_STATE_CLOSED;
                close(data->sock);
                return -1;
        }

        readed = recv(data->sock, data->buffer+data->buflen,
                        HTTP_BUFFER_SIZE-1-data->buflen, 0);
        
        if(readed < 0 && (errno == EAGAIN || errno == EINTR)) return 0;

        if(readed <= 0) {
                data->connState = HTTP_CONN_STATE_CLOSED;
                close(data->sock);
                data->buflen = 0;
                return -1;
        }

        data->buffer[data->buflen+readed] = '\0';
        data->buflen += readed;

        needle = strstr(data->buffer,"\r\n\r\n");

        if(!needle) return 0;

        if(0 == strncmp(cur, "HTTP/1.0 ", 9)) {
                inStream->seekable = 0;
                rc = atoi(cur+9);
        }
        else if(0 == strncmp(cur, "HTTP/1.1 ", 9)) {
                inStream->seekable = 1;
                rc = atoi(cur+9);
        }
        else if(0 == strncmp(cur, "ICY 200 OK", 10)) {
                inStream->seekable = 0;
                rc = 200;
        }
        else if(0 == strncmp(cur, "ICY 400 Server Full", 19)) rc = 400;
        else if(0 == strncmp(cur, "ICY 404", 7)) rc = 404;
        else {
                close(data->sock);
                data->connState = HTTP_CONN_STATE_CLOSED;
                return -1;
        }

        switch(rc) {
        case 200:
        case 206:
                break;
        case 301:
        case 302:
                cur = strstr(cur, "Location: ");
                if(cur) {
                        char * url;
                        int curlen = 0;
                        cur+= strlen("Location: ");
                        while(*(cur+curlen)!='\0' && *(cur+curlen)!='\r') {
                                curlen++;
                        }
                        url = malloc(curlen+1);
                        memcpy(url,cur,curlen);
                        url[curlen] = '\0';
                        ret = parseUrl(data,url);
                        free(url);
                        if(ret == 0 && data->timesRedirected < 
                                        HTTP_REDIRECT_MAX) 
                        {
                                data->timesRedirected++;
                                close(data->sock);
                                data->connState = HTTP_CONN_STATE_REOPEN;
                                data->buflen = 0;
                                return 0;
                        }
                }
        case 404:
        case 400:
        case 401:
        default:
                close(data->sock);
                data->connState = HTTP_CONN_STATE_CLOSED;
                data->buflen = 0;
                return -1;
        }

        cur = strstr(data->buffer,"\r\n");
        while(cur && cur!=needle) {
                if(0 == strncmp(cur,"\r\nContent-Length: ",18)) {
                        if(!inStream->size) inStream->size = atol(cur+18);
                }
                else if(0 == strncmp(cur, "\r\nicy-metaint:", 14)) {
                        data->icyMetaint = atoi(cur+14);
                }
                else if(0 == strncmp(cur, "\r\nicy-name:", 11)) {
                        int incr = 11;
                        char * temp = strstr(cur+incr,"\r\n");
                        if(!temp) break;
                        *temp = '\0';
                        if(inStream->metaName) free(inStream->metaName);
                        while(*(incr+cur) == ' ') incr++;
                        inStream->metaName = strdup(cur+incr);
                        *temp = '\r';
			DEBUG("inputStream_http: metaName: %s\n", 
					inStream->metaName);
                }
                else if(0 == strncmp(cur, "\r\nx-audiocast-name:", 19)) {
                        int incr = 19;
                        char * temp = strstr(cur+incr,"\r\n");
                        if(!temp) break;
                        *temp = '\0';
                        if(inStream->metaName) free(inStream->metaName);
                        while(*(incr+cur) == ' ') incr++;
                        inStream->metaName = strdup(cur+incr);
                        *temp = '\r';
			DEBUG("inputStream_http: metaName: %s\n", 
					inStream->metaName);
                }
                else if(0 == strncmp(cur, "\r\nContent-Type:", 15)) {
                        int incr = 15;
                        char * temp = strstr(cur+incr,"\r\n");
                        if(!temp) break;
                        *temp = '\0';
                        if(inStream->mime) free(inStream->mime);
                        while(*(incr+cur) == ' ') incr++;
                        inStream->mime = strdup(cur+15);
                        *temp = '\r';
                }

                cur = strstr(cur+2,"\r\n");
        }

        if(inStream->size <= 0) inStream->seekable = 0;

        needle += 4; /* 4 == strlen("\r\n\r\n") */
        data->buflen -= (needle-data->buffer);
        /*fwrite(data->buffer, 1, data->buflen, stdout);*/
        memmove(data->buffer, needle, data->buflen);

        data->connState = HTTP_CONN_STATE_OPEN;

	data->prebuffer = 1;

	/*mark as unseekable till we actually implement seeking*/
	inStream->seekable = 0;

        return 0;
}

int inputStream_httpOpen(InputStream * inStream, char * url) {
        InputStreamHTTPData * data = newInputStreamHTTPData();

        inStream->data = data;

        if(parseUrl(data,url) < 0) {
                freeInputStreamHTTPData(data);
                return -1;
        }

        if(initHTTPConnection(inStream) < 0) {
                freeInputStreamHTTPData(data);
                return -1;
        }

        inStream->seekFunc = inputStream_httpSeek;
        inStream->closeFunc = inputStream_httpClose;
        inStream->readFunc = inputStream_httpRead;
        inStream->atEOFFunc = inputStream_httpAtEOF;
        inStream->bufferFunc = inputStream_httpBuffer;

	return 0;
}

int inputStream_httpSeek(InputStream * inStream, long offset, int whence) {
	return -1;
}

static void parseIcyMetadata(InputStream * inStream, char * metadata,
		int size) 
{
	char * r;
	char * s;
	char * temp = malloc(size+1);
	memcpy(temp, metadata, size);
	temp[size] = '\0';
	s = strtok_r(temp, ";", &r);
	while(s) {
		if(0 == strncmp(s, "StreamTitle=", 12)) {
			int cur = 12;
			if(inStream->metaTitle) free(inStream->metaTitle);
			if(*(s+cur) == '\'') cur++;
			if(s[strlen(s)-1] == '\'') {
				s[strlen(s)-1] = '\0';
			}
			inStream->metaTitle = strdup(s+cur);
			DEBUG("inputStream_http: metaTitle: %s\n", 
						inStream->metaTitle);
		}
		s = strtok_r(NULL, ";", &r);
	}
	free(temp);
}

size_t inputStream_httpRead(InputStream * inStream, void * ptr, size_t size, 
		size_t nmemb)
{
        InputStreamHTTPData * data = (InputStreamHTTPData *)inStream->data;
        long tosend = 0;
        long inlen = size*nmemb;
	long maxToSend = data->buflen;

        inputStream_httpBuffer(inStream);

        switch(data->connState) {
        case HTTP_CONN_STATE_OPEN:
		if(data->prebuffer || data->buflen < data->icyMetaint) return 0;

		break;
        case HTTP_CONN_STATE_CLOSED:
		if(data->buflen) break;
        default:
                return 0;
        }

	if(data->icyMetaint > 0) {
		if(data->icyOffset >= data->icyMetaint) {
			int metalen = *(data->buffer);
			metalen <<= 4;
			if(metalen < 0) metalen = 0;
			if(metalen+1 > data->buflen) {
				/* damn that's some fucking big metadata! */
				if(HTTP_BUFFER_SIZE < metalen+1) {
                        		data->connState = 
							HTTP_CONN_STATE_CLOSED;
                        		close(data->sock);
					data->buflen = 0;
				}
				return 0;
			}
			if(metalen > 0) {
				parseIcyMetadata(inStream, data->buffer+1,
						metalen);
			}
			data->buflen -= metalen+1;
			memmove(data->buffer, data->buffer+metalen+1, 
					data->buflen);
			data->icyOffset = 0;
		}
		maxToSend = data->icyMetaint-data->icyOffset;
		maxToSend = maxToSend > data->buflen ? data->buflen : maxToSend;
	}

        if(data->buflen > 0) {
                tosend = inlen > maxToSend ? maxToSend : inlen;
		tosend = (tosend/size)*size;
       
                memcpy(ptr, data->buffer, tosend);
		/*fwrite(ptr,1,readed,stdout);*/
                data->buflen -= tosend;
		data->icyOffset+= tosend;
		/*fwrite(data->buffer,1,readed,stdout);*/
                memmove(data->buffer, data->buffer+tosend, data->buflen);

                inStream->offset += tosend;
        }

	return tosend/size;
}

int inputStream_httpClose(InputStream * inStream) {
        InputStreamHTTPData * data = (InputStreamHTTPData *)inStream->data;

        switch(data->connState) {
        case HTTP_CONN_STATE_CLOSED:
                break;
        default:
                close(data->sock);
        }

        freeInputStreamHTTPData(data);

        return 0;
}

int inputStream_httpAtEOF(InputStream * inStream) {
        InputStreamHTTPData * data = (InputStreamHTTPData *)inStream->data;
        switch(data->connState) {
        case HTTP_CONN_STATE_CLOSED:
                if(data->buflen == 0) return 1;
        default:
                return 0;
        }
}

int inputStream_httpBuffer(InputStream * inStream) {
        InputStreamHTTPData * data = (InputStreamHTTPData *)inStream->data;
        ssize_t readed = 0;

        if(data->connState == HTTP_CONN_STATE_REOPEN) {
                if(initHTTPConnection(inStream) < 0) return -1;
        }

        if(data->connState == HTTP_CONN_STATE_INIT) {
                if(finishHTTPInit(inStream) < 0) return -1;
        }

        if(data->connState == HTTP_CONN_STATE_HELLO) {
                if(getHTTPHello(inStream) < 0) return -1;
        }

        switch(data->connState) {
        case HTTP_CONN_STATE_OPEN:
        case HTTP_CONN_STATE_CLOSED:
                break;
        default:
                return -1;
        }

	if(data->buflen == 0 || data->buflen < data->icyMetaint) {
		data->prebuffer = 1;
	}
	else if(data->buflen > HTTP_PREBUFFER_SIZE) data->prebuffer = 0;

        if(data->connState == HTTP_CONN_STATE_OPEN &&
                                data->buflen < HTTP_BUFFER_SIZE-1) 
        {
                readed = read(data->sock, data->buffer+data->buflen, 
                                (size_t)(HTTP_BUFFER_SIZE-1-data->buflen));

                if(readed < 0 && (errno == EAGAIN || errno == EINTR)) {
                        readed = 0;
                }
                else if(readed <= 0) {
                        close(data->sock);
                        data->connState = HTTP_CONN_STATE_CLOSED;
                        readed = 0;
                }
		/*fwrite(data->buffer+data->buflen,1,readed,stdout);*/
		data->buflen += readed;
        }

	if(data->buflen > HTTP_PREBUFFER_SIZE) data->prebuffer = 0;

        return (readed ? 1 : 0);
}
