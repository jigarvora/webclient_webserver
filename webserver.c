#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/queue.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <netdb.h>
#include <pthread.h>
#include <fcntl.h>

#include "log.h"

#define BACKLOG	20	/* max # of pending incoming connections */
#define MAXFILELEN 200
#define OPERATION_HEADER	"GetFile"
#define CLIENT_OPERATION	"GET"

static int maxdatasize = MAXFILELEN + sizeof(OPERATION_HEADER) +
    sizeof(CLIENT_OPERATION) + 6;
static long port = 8888;
static int workers = 1;
static char *path = ".";

/* mutex for requestq */
static pthread_mutex_t req_mutex = PTHREAD_MUTEX_INITIALIZER;

static STAILQ_HEAD(, request) requestq;
struct request {
	int fd;
	STAILQ_ENTRY(request) link;          /* Tail queue */
};

static pthread_cond_t req_cond;

static const struct option server_options[] = {
	{ .name = "port", .val = 'p', .has_arg = 1},
	{ .name = "workerthreads", .val = 't', .has_arg = 1},
	{ .name = "path", .val = 'f', .has_arg = 1},
	{ .name = "help", .val = 'h'},
	{ .name = NULL }
};

static void server_usage(void)
{
	printf("usage:\n");
	printf("\twebserver [options]\n");
	printf("options:\n");
	printf("\t-p port (Default: 8888)\n");
	printf("\t-t number of worker threads (Default: 1, Range: 1-1000)\n");
	printf("\t-f path to static files (Default: .)\n");
	printf("\t-h show help message\n");
}

static void server_opts(int argc, char **argv)
{
	int long_index = 0;
	int opt;
	char *errptr;

	optind = 0;
	while ((opt = getopt_long(argc, argv, "?hp:t:f:",
	    server_options, &long_index)) != -1) {
	    	switch(opt) {
	    	case 'h':
	    	case '?':
	    		server_usage();
	    		exit(0);
	    	case 'p':
	    		errno = 0;
	    		port = strtoul(optarg, &errptr, 10);
	    		if (*errptr != '\0' || errno) {
	    			server_usage();
	    			exit(-1);
	    		}
	    		break;
	    	case 't':
	    		errno = 0;
	    		workers = strtoul(optarg, &errptr, 10);
	    		if (*errptr != '\0' || errno || !workers ||
	    		    workers > 1000) {
	    			server_usage();
	    			exit(-1);
	    		}
	    		break;
	    	case 'f':
	    		path = optarg;
	    		break;
	    	default:
	    		server_usage();
	    		exit(-1);
	    	}
	}
}

static int server_setup_socket(void)
{
	struct addrinfo hints;
	struct addrinfo *res;
	int sockfd;
	char service[20];
	int status;
	int yes = 1;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;     // fill in my IP for me

	snprintf(service, sizeof(service), "%lu", port);
	if ((status = getaddrinfo(NULL, service, &hints, &res))) {
		log_err("%s: getaddrinfo error: %s", __func__,
		    gai_strerror(status));
		return -1;
	}

	// make a socket:
	errno = 0;
	sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (errno || sockfd  == -1) {
		perror("socket");
		return -1;
	}
	// lose the pesky "Address already in use" error message
	if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes,
	    sizeof(int)) == -1) {
		perror("setsockopt");
		return -1;
	} 
	errno = 0;
	// bind it to the port we passed in to getaddrinfo():
	bind(sockfd, res->ai_addr, res->ai_addrlen);
	if (errno) {
		perror("bind");
		return -1;
	}

	free(res);
	// listen on the port for incoming connections
	errno = 0;
	if (listen(sockfd, BACKLOG) || errno) {
		perror("listen");
		return -1;
	}
	return sockfd;
}

static int server_sendall(int s, char *buf, ssize_t *len)
{
	int total = 0;
	ssize_t bytesleft = *len;
	int rc;

	while (total < *len) {
		rc = send(s, buf + total, bytesleft, 0);
		if (rc == -1) {
			perror("send");
			break;
		}
		total += rc;
		bytesleft -= rc;
	}

	*len = total; // return number actually sent here

	return rc;
} 

static void server_process_request(struct request *req)
{
	char buf[maxdatasize];
	ssize_t numbytes;
	int bytes_recvd = 0;
	int header_checked = 0;
	char *filename = NULL;
	int file_fd;
	off_t offset;
	off_t filesize;
	struct stat stat_buf;
	char *filetoopen;
	int op_header_size = sizeof(OPERATION_HEADER) - 1;
	int cli_op_size = sizeof(CLIENT_OPERATION) - 1;

	buf[maxdatasize - 1] = '\0';
	while (1) {
		numbytes = recv(req->fd, buf + bytes_recvd,
		    sizeof(buf) - bytes_recvd - 1, 0);
		if (numbytes == -1) {
			perror("recv");
			break;
		}
		if (!numbytes) {
			break;
		}
		bytes_recvd += numbytes;
		if (bytes_recvd <= sizeof(OPERATION_HEADER) +
		    sizeof(CLIENT_OPERATION)) {
			continue;
		}
		if (!header_checked) {
			/* check the header */
			header_checked = 1;
			if (strncmp(buf, OPERATION_HEADER, op_header_size)) {
    				log_warn("bad header from client");
    				break;
			}
			if (strncmp(buf + sizeof(OPERATION_HEADER),
			    CLIENT_OPERATION, cli_op_size)) {
    				log_warn("bad operation from client");
    				break;
    			}
    			filename = buf + sizeof(OPERATION_HEADER) +
			    sizeof(CLIENT_OPERATION);
		}
		if (filename && (buf[bytes_recvd - 1] == '\0' ||
		    !(sizeof(buf) - bytes_recvd - 1))) {
			break;
		}
	}
	if (!filename) {
		goto finish;
	}
	log_debug("Request for file %s", filename);
	asprintf(&filetoopen, "%s/%s", path, filename);
	errno = 0;
	file_fd = open(filetoopen, O_RDONLY);
	free(filetoopen);
	if (file_fd == -1) {
		numbytes = snprintf(buf, sizeof(buf), "%s FILE_NOT_FOUND 0 0",
		    OPERATION_HEADER);
	} else {
		fstat(file_fd, &stat_buf);
		errno = 0;
		filesize = stat_buf.st_size;
		numbytes = snprintf(buf, sizeof(buf), "%s OK %lu ",
		    OPERATION_HEADER, (unsigned long)filesize);
	}
	log_debug("%s", buf);
	if (server_sendall(req->fd, buf, &numbytes) == -1) {
		goto finish;
	}
	if (file_fd == -1) {
		goto finish;
	}
	offset = 0;
	while (filesize > 0) {
		numbytes = sendfile(req->fd, file_fd, &offset, filesize);
    		if (numbytes < 0) {
    			close(file_fd);
			perror("sendfile");
        		break;
		}
		offset += numbytes;
		filesize -= numbytes;
	}
    	close(file_fd);
finish:
	close(req->fd);
	free(req);
}

static void *server_worker(void *arg)
{
	struct request *req;

	while (1) {
		pthread_mutex_lock(&req_mutex);
		while (STAILQ_EMPTY(&requestq)) {
			pthread_cond_wait(&req_cond, &req_mutex);
		}
		req = STAILQ_FIRST(&requestq);
		STAILQ_REMOVE_HEAD(&requestq, link);
		pthread_mutex_unlock(&req_mutex);
		server_process_request(req);
	}

	return NULL;
}

int main(int argc, char **argv)
{
	int server_socket;
	struct sockaddr_storage their_addr; // connector's address information
	socklen_t sin_size;
	int new_fd;
	struct request *req;
	pthread_attr_t attr;
	pthread_t thread_id;
	int i;

	server_opts(argc, argv);
	server_socket = server_setup_socket();
	if (server_socket < 0) {
		exit(-1);
	}
	/* Initialize vars */
	STAILQ_INIT(&requestq);
	pthread_cond_init(&req_cond, NULL);

	/* Start the detached worker threads */
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	for (i = 0; i < workers; i++) {
		pthread_create(&thread_id, &attr, server_worker, NULL);
	}

	/* Accept connections */
	while (1) {
		sin_size = sizeof(their_addr);
		new_fd = accept(server_socket, (struct sockaddr *)&their_addr,
		    &sin_size);
		if (new_fd == -1) {
        	    perror("accept");
		    continue;
		}
		req = calloc(1, sizeof(struct request));
		if (!req) {
			perror("calloc");
		}
		req->fd = new_fd;
		pthread_mutex_lock(&req_mutex);
		STAILQ_INSERT_TAIL(&requestq, req, link);
		pthread_mutex_unlock(&req_mutex);
		pthread_cond_signal(&req_cond);
	}
	return 0;
}
