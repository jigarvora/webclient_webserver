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
#include <sys/time.h>
#include <netdb.h>
#include <pthread.h>

#include "log.h"

#define OPERATION_HEADER	"GetFile"
#define CLIENT_OPERATION	"GET"
#define STATUS_OK		"OK"
#define MAXDATASIZE		1024
#define MAXNUMWORKERS		100

static long port = 8888;
static int workers = 1;
static char *address = "0.0.0.0";
static char *workloadfile = "workload.txt";
static char *downloaddir;
static int requests = 10;
static char *metricsfile = "metrics.txt";
static pthread_mutex_t reqs_finished_mutex = PTHREAD_MUTEX_INITIALIZER;
static int reqs_finished;
static long long num_bytes_recvd;
static int quit_all_threads;

/* mutex for requestq */
static pthread_mutex_t req_mutex = PTHREAD_MUTEX_INITIALIZER;

static STAILQ_HEAD(, request) requestq;
struct request {
	char *filename;
	STAILQ_ENTRY(request) link;          /* Tail queue */
};

static pthread_cond_t req_cond;
static pthread_cond_t workers_done_cond;

static const struct option client_options[] = {
	{ .name = "serveraddress", .val = 's', .has_arg = 1},
	{ .name = "port", .val = 'p', .has_arg = 1},
	{ .name = "workerthreads", .val = 't', .has_arg = 1},
	{ .name = "workloadfile", .val = 'w', .has_arg = 1},
	{ .name = "downloaddir", .val = 'd', .has_arg = 1},
	{ .name = "requests", .val = 'r', .has_arg = 1},
	{ .name = "metricsfile", .val = 'm', .has_arg = 1},
	{ .name = "help", .val = 'h'},
	{ .name = NULL }
};

static void client_usage(void)
{
	printf("usage:\n");
	printf("\twebclient [options]\n");
	printf("options:\n");
	printf("\t-s server address (Default: 0.0.0.0)\n");
	printf("\t-p server port (Default: 8888)\n");
	printf("\t-t number of worker threads (Default: 1, Range: 1-100)\n");
	printf("\t-w path to workload file (Default: workload.txt)\n");
	printf("\t-d path to downloaded file directory (Default: null)\n");
	printf("\t-r number of total requests (Default: 10, Range: 1-1000)\n");
	printf("\t-m path to metrics file (Default: metrics.txt)\n");
	printf("\t-h show help message\n");
}

static void client_opts(int argc, char **argv)
{
	int long_index = 0;
	int opt;
	char *errptr;

	optind = 0;
	while ((opt = getopt_long(argc, argv, "?hp:t:f:r:s:w:d:m:",
	    client_options, &long_index)) != -1) {
	    	switch(opt) {
	    	case 'h':
	    	case '?':
	    		client_usage();
	    		exit(0);
	    	case 'p':
	    		errno = 0;
	    		port = strtoul(optarg, &errptr, 10);
	    		if (*errptr != '\0' || errno) {
	    			client_usage();
	    			exit(-1);
	    		}
	    		break;
	    	case 't':
	    		errno = 0;
	    		workers = strtoul(optarg, &errptr, 10);
	    		if (*errptr != '\0' || errno || !workers ||
	    		    workers > MAXNUMWORKERS) {
	    			client_usage();
	    			exit(-1);
	    		}
	    		break;
	    	case 'r':
	    		errno = 0;
	    		requests = strtoul(optarg, &errptr, 10);
	    		if (*errptr != '\0' || errno || !requests ||
	    		    requests > 1000) {
	    			client_usage();
	    			exit(-1);
	    		}
	    		break;
	    	case 's':
	    		address = optarg;
	    		break;
	    	case 'w':
	    		workloadfile = optarg;
	    		break;
	    	case 'd':
	    		downloaddir = optarg;
	    		break;
	    	case 'm':
	    		metricsfile = optarg;
	    		break;
	    	default:
	    		client_usage();
	    		exit(-1);
	    	}
	}
}

static int client_setup_socket(void)
{
	struct addrinfo hints;
	struct addrinfo *res;
	int sockfd;
	char service[20];
	int status;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	snprintf(service, sizeof(service), "%lu", port);
	log_debug("Connecting to %s:%s", address, service);
	if ((status = getaddrinfo(address, service, &hints, &res))) {
		log_err("%s: getaddrinfo error: %s", __func__,
		    gai_strerror(status));
		return -1;
	}

	// make a socket and connect
	errno = 0;
	sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (errno || sockfd  == -1) {
		perror("socket");
	}
	if (connect(sockfd, res->ai_addr, res->ai_addrlen) == -1) {
	    close(sockfd);
	    perror("connect");
	    return -1;;
	}
	freeaddrinfo(res);
	return sockfd;
}

static int client_process_request(struct request *req)
{
	int socket;
	char *operation;
	char buf[MAXDATASIZE];
	ssize_t numbytes;
	char *endptr;
	long long bytes_recvd = 0;
	long long filesize = 0;
	long long filebytes= 0;
	int header_checked = 0;
	int filesize_checked = 0;
	int numwritten;
	int offset = 0;
	char *buf_ptr;
	char *end_of_len;
	int bytestocopy;
	FILE *fp = NULL;
	int op_header_size = sizeof(OPERATION_HEADER) - 1;
	int status_size = sizeof(STATUS_OK) - 1;
	char *filetowrite;

	socket = client_setup_socket();
	if (socket < 0) {
		log_err("couldn't open socket");
		goto finish;
	}
	asprintf(&operation, "%s %s %s", OPERATION_HEADER, CLIENT_OPERATION,
	    req->filename);
	log_debug("%s", operation);
	if (send(socket, operation, strlen(operation) + 1, 0) == -1) {
		perror("send");
		free(operation);
		close(socket);
		goto finish;
	}
	free(operation);
	while (1) {
		numbytes = recv(socket, buf + offset,
		    sizeof(buf) - offset, 0);
		if (numbytes == -1) {
			perror("recv");
			break;
		}
		if (!numbytes) {
			break;
		}
		bytes_recvd += numbytes;
		offset += numbytes;
		if (bytes_recvd <= sizeof(OPERATION_HEADER) +
		    sizeof(STATUS_OK)) {
			continue;
		}
		if (!header_checked) {
			/* check the header */
			header_checked = 1;
			if (strncmp(buf, OPERATION_HEADER,
			    op_header_size - 1)) {
    				log_warn("bad header from server");
    				break;
			}
			if (strncmp(buf + sizeof(OPERATION_HEADER),
			    STATUS_OK, status_size - 1)) {
    				log_warn("file %s not found on server",
    				    req->filename);
    				break;
    			}
		}
		if (!filesize_checked) {
			/* get the filesize */
			buf_ptr = buf + sizeof(OPERATION_HEADER) +
			    sizeof(STATUS_OK);
			end_of_len = strchr(buf_ptr, ' ');
			if (end_of_len == NULL ||
			    (end_of_len - buf) > bytes_recvd) {
				continue;
			}
			*end_of_len = '\0';
			errno = 0;
  			filesize = strtoull(buf_ptr, &endptr, 10);
    			if (errno || *endptr != '\0') {
    				log_warn("bad size");
    				break;
    			}
    			offset = 0;
    			filesize_checked = 1;
    			bytestocopy = bytes_recvd - (end_of_len - buf + 1);
    			buf_ptr = end_of_len + 1;
			if (downloaddir) {
				asprintf(&filetowrite, "%s/%s", downloaddir, req->filename);
				fp = fopen(filetowrite, "w");
				free(filetowrite);
				if (!fp) {
					perror("fopen");
				}
			}
		} else {
			bytestocopy = numbytes;
			buf_ptr = buf;
		}
		if (bytestocopy) {
			filebytes += bytestocopy;
			if (filebytes > filesize) {
				log_warn("server sent more than the filesize");
				bytes_recvd = filesize;
				bytestocopy -= (bytes_recvd - filesize);
			}
			/* put the bytestream into a file */
			if (fp) {
				numwritten =
				    fwrite(buf_ptr, 1, bytestocopy, fp);
				if (numwritten != bytestocopy) {
					perror("fwrite");
					break;
				}
			}
			offset = 0;
		}
		if (filebytes == filesize) {
			break;
		}
	}
	if (fp) {
		fclose(fp);
	}
	close(socket);
finish:
	free(req->filename);
	free(req);
	return bytes_recvd;
}

/* Return diff in seconds along with updated result structure  */
float client_timeval_subtract(struct timeval *result, struct timeval *t2,
		    struct timeval *t1)
{
	long int diff = (t2->tv_usec + 1000000 * t2->tv_sec) -
	    (t1->tv_usec + 1000000 * t1->tv_sec);
	result->tv_sec = diff / 1000000;
	result->tv_usec = diff % 1000000;
	return diff / 1000000.0;
}

static void *client_worker(void *arg)
{
	struct request *req;
	long long bytes_recvd;
	int exit = 0;

	while (1) {
		pthread_mutex_lock(&req_mutex);
		while (STAILQ_EMPTY(&requestq)) {
			pthread_cond_wait(&req_cond, &req_mutex);
			if (quit_all_threads) {
				pthread_mutex_unlock(&req_mutex);
				return NULL;
			}
		}
		req = STAILQ_FIRST(&requestq);
		STAILQ_REMOVE_HEAD(&requestq, link);
		pthread_mutex_unlock(&req_mutex);
		bytes_recvd = client_process_request(req);
		pthread_mutex_lock(&reqs_finished_mutex);
		num_bytes_recvd += bytes_recvd;
		reqs_finished++;
		if (reqs_finished == requests) {
			pthread_cond_signal(&workers_done_cond);
			exit = 1;
		}
		pthread_mutex_unlock(&reqs_finished_mutex);
		if (exit) {
			break;
		}
	}

	return NULL;
}

int main(int argc, char **argv)
{
	pthread_t thread[MAXNUMWORKERS];
	FILE *fp;
	size_t len = 0;
	ssize_t read;
	char *line = NULL;
	struct request *req;
	int num_reqs_generated = 0;
	int i;
	char *lastchar;
	struct timeval tv_start;
	struct timeval tv_end;
	struct timeval tv_diff;
	float timediff;

	/* measure beginning */
	gettimeofday(&tv_start, NULL);

	client_opts(argc, argv);

	/* Initialize vars */
	STAILQ_INIT(&requestq);
	pthread_cond_init(&req_cond, NULL);

	/* Start the worker threads */
	for (i = 0; i < workers; i++) {
		pthread_create(&thread[i], NULL, client_worker, NULL);
	}
	fp = fopen(workloadfile, "r");
	if (fp == NULL) {
		log_err("%s not found", workloadfile);
		exit(EXIT_FAILURE);
	}
read_from_top:
	while ((read = getline(&line, &len, fp)) != -1 &&
	    num_reqs_generated < requests) {
		lastchar = &line[strlen(line) - 1];
		if (*lastchar == '\n' || *lastchar == ' ') {
			/* replace the char with \0 */
			*lastchar = '\0';
		}
		req = calloc(1, sizeof(struct request));
		req->filename = line;
		pthread_mutex_lock(&req_mutex);
		STAILQ_INSERT_TAIL(&requestq, req, link);
		pthread_mutex_unlock(&req_mutex);
		/*
		 * instead of creating all jobs at once and then starting the
		 * workers, its better to start processing the requests as they
		 * are created. this reduces the amount of peak heap usage and
		 * the overall performance is faster.
		 */
		pthread_cond_signal(&req_cond);
		num_reqs_generated++;
		line = NULL;
		len = 0;
	}
	if (num_reqs_generated != requests) {
		/* go back to the top of the file */
		fseek(fp, 0, SEEK_SET);
		goto read_from_top;
	}
	fclose(fp);
	pthread_mutex_lock(&reqs_finished_mutex);
	if (reqs_finished != requests) {
		pthread_cond_wait(&workers_done_cond, &reqs_finished_mutex);
	}
	pthread_mutex_unlock(&reqs_finished_mutex);
	pthread_mutex_lock(&req_mutex);
	quit_all_threads = 1;
	pthread_mutex_unlock(&req_mutex);
	pthread_cond_broadcast(&req_cond);
	for (i = 0; i < workers; i++) {
		pthread_join(thread[i], NULL);
	}
	gettimeofday(&tv_end, NULL);
	timediff = client_timeval_subtract(&tv_diff, &tv_end, &tv_start);
	printf("Time Elapsed: %ld.%06ld\n", tv_diff.tv_sec, tv_diff.tv_usec);
	fp = fopen(metricsfile, "w");
	fprintf(fp, "webclient stats report:\n");
	fprintf(fp, "\telapsed time:\t\t\t%ld.%06ld secs\n",
	    tv_diff.tv_sec, tv_diff.tv_usec);
	fprintf(fp, "\tbytes received:\t\t\t%llu bytes\n", num_bytes_recvd);
	fprintf(fp, "\tthroughput:\t\t\t%.1f bytes/sec\n",
	    num_bytes_recvd / timediff);
	fprintf(fp, "\trequests serviced:\t\t\t%d reqs\n", requests);
	fprintf(fp, "\tavg response time:\t\t\t%.1f reqs/sec\n", requests / timediff);
	fclose(fp);
	return 0;
}
