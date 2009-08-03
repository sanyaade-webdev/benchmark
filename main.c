/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

#include <unistd.h>
#include <time.h>

#include <pthread.h>

#include <cherokee/buffer.h>
#include <cherokee/list.h>

#include <curl/curl.h>
#include <curl/types.h>
#include <curl/easy.h> 

#define EXIT_OK             0
#define EXIT_ERROR          1
#define THREAD_NUM_DEFAULT  10
#define REQUEST_NUM_DEFAULT 10000
#define KEEPALIVE_DEFAULT   0
#define RESPONSES_COUNT_LEN 10

#define ALLOCATE(v,t)				\
	v = (t *) malloc(sizeof(t));		\
	if (unlikely (v == NULL)) {		\
		return ret_nomem;		\
	}

typedef struct {
	cherokee_list_t  entry;
	pthread_t        pthread;
	pthread_mutex_t  start_mutex;
	CURL            *curl;	
} cb_thread_t;
#define THREAD(t) ((cb_thread_t *)(t))

typedef struct {
	cherokee_list_t   entry;
	cherokee_buffer_t url;
} cb_url_t;
#define URL(u) ((cb_url_t *)(u))

typedef struct {
	long http_code;
	long count;
} cb_response_t;


typedef unsigned long long time_msec_t;

/* Globals
 */
static cherokee_list_t        urls;
static int                    keepalive     = KEEPALIVE_DEFAULT;
static int                    thread_num    = THREAD_NUM_DEFAULT;
static long                   request_num   = REQUEST_NUM_DEFAULT;
static int                    verbose       = 0;
static int                    finished      = 0;
static volatile long          request_done  = 0;
static volatile long          request_fails = 0;
static volatile time_msec_t   time_start    = 0;
static volatile off_t         tx_total      = 0;
static volatile cb_response_t responses[RESPONSES_COUNT_LEN];

static time_msec_t
get_time_msecs (void)
{
	struct timeval tv;

	gettimeofday (&tv, NULL);
	return ((tv.tv_sec * 1000) + (tv.tv_usec) / 1000);
}

static void
print_update (void)
{
	int         elapse;
	time_msec_t time_now;
	int         reqs_sec = 0;
	int         tx_sec   = 0;

	time_now = get_time_msecs();
	elapse = time_now - time_start;

	reqs_sec = (int)(request_done / (elapse/1000.0f));
	tx_sec   = (int)(tx_total     / (elapse/1000.0f));

	printf ("threads %d, reqs %lu (%d reqs/s avg), TX %llu (%d bytes/s avg), fails %lu, %.2f secs\n",
		thread_num, request_done, reqs_sec, tx_total, tx_sec, request_fails, elapse/1000.0f);
}

static void
print_error_codes (void)
{
	int i;

	printf ("\nHTTP responses:\n");
	for (i=0; i<RESPONSES_COUNT_LEN; i++) {
		if (responses[i].http_code == 0) {
			printf ("\n");
			return;
		}
		printf ("  HTTP %d: %d (%.2f%%) ", 
			responses[i].http_code, responses[i].count,
			(responses[i].count / (float)request_done) * 100);
	}
}

static void
report_fatal_error (const char *str)
{
	fprintf (stderr, "FATAL ERROR: %s\n", str);
	finished = 1;
}

static void
report_error (const char *str)
{
	fprintf (stderr, "ERROR: %s\n", str);
}

static void
count_response (long http_code)
{
	int i;
	
	if (request_done >= request_num) {
		finished = 1;
		return;
	}

	request_done++;

	for (i=0; i<RESPONSES_COUNT_LEN; i++) {
		if (responses[i].http_code == 0) {
			responses[i].http_code = http_code;
			responses[i].count     = 1;
			return;

		} else if (responses[i].http_code == http_code) {
			responses[i].count++;
			return;
		}
	}

	report_fatal_error ("Run out of http_error space");
}

static size_t
cb_write_data (void *ptr, size_t size, size_t nmemb, void *stream)
{
	long total;

	total = (size * nmemb);
	tx_total += total;
	return total;
}

static size_t
cb_got_header (void *ptr, size_t size, size_t nmemb, void *stream)
{
	long total;

	total = (size * nmemb);
	return total;
}

static void *
thread_routine (void *me)
{
	int          re;
	long         http_code;
	cb_thread_t *thread     = (cb_thread_t *)me;
	cb_url_t    *url        = (cb_url_t *)urls.next;

	sleep(1);

	if (time_start == 0) {
		time_start = get_time_msecs();
	}

	while (! finished) {
		/* Configure curl, if needed
		 */
		if (thread->curl == NULL) {
			thread->curl = curl_easy_init();
			curl_easy_setopt (thread->curl, CURLOPT_NOPROGRESS, 1);
			curl_easy_setopt (thread->curl, CURLOPT_WRITEFUNCTION,  cb_write_data);
			curl_easy_setopt (thread->curl, CURLOPT_HEADERFUNCTION, cb_got_header);
			curl_easy_setopt (thread->curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
		}
		
		curl_easy_setopt (thread->curl, CURLOPT_URL, url->url.buf);

		/* Request it
		 */
		re = curl_easy_perform (thread->curl);
		switch (re) {
		case CURLE_OK:
			curl_easy_getinfo (thread->curl, CURLINFO_RESPONSE_CODE, &http_code);
			count_response (http_code);
			break;

		case CURLE_COULDNT_RESOLVE_HOST:
			request_fails++;
			report_fatal_error (curl_easy_strerror(re));
			break;

		default:
			request_fails++;
			if (verbose) {
				report_error (curl_easy_strerror(re));
			}
		}

		/* Prepare for the next request
		 */
		if (! keepalive) {
			curl_easy_cleanup (thread->curl);
			thread->curl = NULL;
		}

		url = (cb_url_t *)((url->entry.next == &urls) ? urls.next : url->entry.next);
	}

	return NULL;
}

static ret_t
thread_launch (cherokee_list_t *threads, int num)
{
	int              i;
	int              re;
	cb_thread_t     *thread;
	cherokee_list_t *item;

	/* Create threads
	 */
	for (i=0; i<num; i++) {
		ALLOCATE (thread, cb_thread_t);

		INIT_LIST_HEAD (&thread->entry);
		cherokee_list_add (&thread->entry, threads);
		thread->curl = NULL;

		pthread_mutex_init (&thread->start_mutex, NULL);
		pthread_mutex_lock (&thread->start_mutex);

		re = pthread_create (&thread->pthread, NULL, thread_routine, thread);
		if (re != 0) {
			PRINT_ERROR_S ("Couldn't create pthread\n");

			free (thread);
			return ret_error;
		}
	}

	/* Activate threads
	 */
	list_for_each (item, threads) {
		pthread_mutex_unlock (&((cb_thread_t *)(item))->start_mutex);
	}

	return ret_ok;
}

static void
print_help (void)
{
}

static ret_t
process_parameters (int argc, char **argv)
{
	int c;

	while ((c = getopt(argc, argv, "hvkc:n:")) != -1) {
		switch(c) {
		case 'k':
			keepalive = 1;
			break;
		case 'v':
			verbose = 1;
			break;
		case 'c':
			thread_num = atoi(optarg);
			break;
		case 'n':
			request_num = atol(optarg);
			break;
		case 'h':
		case '?':
		default:
			print_help();
			return ret_eof;
		}
	}

	for (c=0; c<argc; c++) {
		cb_url_t *u;

		if (strncmp ("http", argv[c], 4) != 0) {
			continue;
		}

		ALLOCATE (u, cb_url_t);
		cherokee_buffer_init (&u->url);
		cherokee_buffer_add  (&u->url, argv[c], strlen(argv[c]));
		cherokee_list_add (&u->entry, &urls);
	}

	return ret_ok;
}

int
main (int argc, char **argv)
{
	int             i;
	ret_t           ret;
	cherokee_list_t threads;

	INIT_LIST_HEAD (&threads);
	INIT_LIST_HEAD (&urls);

	for (i=0; i<RESPONSES_COUNT_LEN; i++) {
		responses[i].http_code = 0;
		responses[i].count     = 0;
	}

	ret = process_parameters (argc, argv);
	if (ret != ret_ok) {
		exit (EXIT_ERROR);
	}

	if (cherokee_list_empty (&urls)) {
		print_help();
		exit (EXIT_ERROR);		
	}

	curl_global_init (CURL_GLOBAL_ALL);

	ret = thread_launch (&threads, thread_num);
	if (ret != ret_ok) {
		exit (EXIT_ERROR);
	}

	sleep(1);
	while (! finished) {
		sleep(1);
		print_update();
	}

	if (verbose) {
		print_error_codes();
	}
	
	return EXIT_OK;
}
