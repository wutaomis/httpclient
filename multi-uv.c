/***************************************************************************
 *                                  _   _ ____  _
 *  Project                     ___| | | |  _ \| |
 *                             / __| | | | |_) | |
 *                            | (__| |_| |  _ <| |___
 *                             \___|\___/|_| \_\_____|
 *
 * Copyright (C) 1998 - 2020, Daniel Stenberg, <daniel@haxx.se>, et al.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution. The terms
 * are also available at https://curl.se/docs/copyright.html.
 *
 * You may opt to use, copy, modify, merge, publish, distribute and/or sell
 * copies of the Software, and permit persons to whom the Software is
 * furnished to do so, under the terms of the COPYING file.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ***************************************************************************/

/* <DESC>
 * multi_socket API using libuv
 * </DESC>
 */
/* Example application using the multi socket interface to download multiple
   files in parallel, powered by libuv.

   Requires libuv and (of course) libcurl.

   See https://nikhilm.github.io/uvbook/ for more information on libuv.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <pthread.h>
#include <uv.h>
#include <curl/curl.h>

uv_loop_t *loop;
CURLM *curl_handle;
CURL *handles[128];
uv_timer_t timeout;
char global_url[256];
struct timeval tv;

typedef struct curl_context_s {
  uv_poll_t poll_handle;
  curl_socket_t sockfd;
} curl_context_t;

static int counter = 0;
static int times = 0;

static curl_context_t *create_curl_context(curl_socket_t sockfd)
{
  curl_context_t *context;

  context = (curl_context_t *) malloc(sizeof(*context));

  context->sockfd = sockfd;

  uv_poll_init_socket(loop, &context->poll_handle, sockfd);
  context->poll_handle.data = context;

  return context;
}

static void curl_close_cb(uv_handle_t *handle)
{
  curl_context_t *context = (curl_context_t *) handle->data;
  free(context);
}

static void destroy_curl_context(curl_context_t *context)
{
  uv_close((uv_handle_t *) &context->poll_handle, curl_close_cb);
}

static void add_download(const char *url, int num)
{
  char filename[50];
  FILE *file;
  int i = 0;


  /*snprintf(filename, 50, "%d.download", num);

  file = fopen(filename, "wb");
  if(!file) {
    fprintf(stderr, "Error opening %s\n", filename);
    return;
  }
  */

  for (i = 0 ; i < num ; i ++ ) {
    CURL *handle = curl_easy_init();
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, stdout);
    curl_easy_setopt(handle, CURLOPT_PRIVATE, stdout);
    curl_easy_setopt(handle, CURLOPT_URL, url);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, 30L);
    //curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 0L);
    //curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(handle, CURLOPT_VERBOSE, 1L);
    //curl_easy_setopt(handle, CURLOPT_PIPEWAIT, 1L);
    handles[i] = handle;
    fprintf(stdout,"curl_multi_add_handle\n");
    curl_multi_add_handle(curl_handle, handle);
    counter ++;
    //fprintf(stderr, "Added download %s -> %s\n", url, filename);
  }
  gettimeofday(&tv, NULL);
  fprintf(stdout, "add_download end tm=%ld:%ld\n",tv.tv_sec,tv.tv_usec/1000);
  strcpy(global_url,url);
  fflush(stdout);
}

static void check_multi_info(void)
{
  char *done_url;
  CURLMsg *message;
  int pending;
  CURL *easy_handle;
  FILE *file;
  char buf[128];

  while((message = curl_multi_info_read(curl_handle, &pending))) {
    fprintf(stdout, "\nmessage = %d, pending = %d, tid = %lu",message->msg, pending, pthread_self());
    switch(message->msg) {
    case CURLMSG_DONE:
      /* Do not use message data after calling curl_multi_remove_handle() and
         curl_easy_cleanup(). As per curl_multi_info_read() docs:
         "WARNING: The data the returned pointer points to will not survive
         calling curl_multi_cleanup, curl_multi_remove_handle or
         curl_easy_cleanup." */
      easy_handle = message->easy_handle;
      curl_easy_getinfo(easy_handle, CURLINFO_EFFECTIVE_URL, &done_url);
      curl_easy_getinfo(easy_handle, CURLINFO_PRIVATE, &file);
      fprintf(stdout, "\n%ud, %s DONE\n", easy_handle, done_url);
      if (counter < times) {
        curl_multi_remove_handle(curl_handle, easy_handle);
        curl_easy_setopt(easy_handle, CURLOPT_URL, global_url);
        curl_multi_add_handle(curl_handle, easy_handle);
        counter ++;
        fprintf(stdout, "set url again %s, counter %d, tid=%lu\n", global_url, counter, pthread_self());
      }
      break;

    default:
      fprintf(stderr, "CURLMSG default\n");
      break;
    }
  }
  fflush(stdout);
}

static void curl_perform(uv_poll_t *req, int status, int events)
{
  int running_handles;
  int flags = 0;
  curl_context_t *context;
  
  if(events & UV_READABLE)
    flags |= CURL_CSELECT_IN;
  if(events & UV_WRITABLE)
    flags |= CURL_CSELECT_OUT;

  context = (curl_context_t *) req->data;
  gettimeofday(&tv, NULL);
  fprintf(stdout, "curl_perform before multi_socket_action, event=%d,tm=%ld:%ld\n",events,tv.tv_sec,tv.tv_usec/1000);
  curl_multi_socket_action(curl_handle, context->sockfd, flags, &running_handles);
  fprintf(stdout, "curl_perform after multi_socket_action, handles=%d,tm=%ld:%ld\n",running_handles,tv.tv_sec,tv.tv_usec/1000);
  check_multi_info();
  gettimeofday(&tv, NULL);
  fprintf(stdout, "curl_perform after check_multi_info, event=%d,tm=%ld:%ld\n",events,tv.tv_sec,tv.tv_usec/1000);
  fprintf(stdout, "===============================================================\n");
  fflush(stdout);
}

static void on_timeout(uv_timer_t *req)
{
  int running_handles;
  gettimeofday(&tv, NULL);
  fprintf(stdout,"on_timeout before multi_socket_action, handles=%d, tm=%ld:%ld\n", running_handles,tv.tv_sec,tv.tv_usec/1000);
  curl_multi_socket_action(curl_handle, CURL_SOCKET_TIMEOUT, 0,&running_handles);
  gettimeofday(&tv, NULL);
  fprintf(stdout,"on_timeout after multi_socket_action, handles=%d, tm=%ld:%ld\n", running_handles,tv.tv_sec,tv.tv_usec/1000);
  check_multi_info();
  gettimeofday(&tv, NULL);
  fprintf(stdout,"on_timeout after check_multi_info, handles=%d, tm=%ld:%ld\n", running_handles,tv.tv_sec,tv.tv_usec/1000);
  fprintf(stdout, "**************************************************************\n");
  fflush(stdout);
}

static int start_timeout(CURLM *multi, long timeout_ms, void *userp)
{
  gettimeofday(&tv, NULL);
  fprintf(stdout,"start_timeout called timeout_ms=%ld, tm=%ld:%ld\n", timeout_ms,tv.tv_sec,tv.tv_usec/1000);
  if(timeout_ms < 0) {
    uv_timer_stop(&timeout);
  }
  else {
    if(timeout_ms == 0)
      timeout_ms = 1; /* 0 means directly call socket_action, but we'll do it
                         in a bit */
    uv_timer_start(&timeout, on_timeout, timeout_ms, 0);
  }
  fflush(stdout);
  return 0;
}

static int handle_socket(CURL *easy, curl_socket_t s, int action, void *userp,
                  void *socketp)
{
  curl_context_t *curl_context;
  int events = 0;

  gettimeofday(&tv, NULL);
  fprintf(stdout,"handle_socket called: action=%d,tm=%ld:%ld\n",action,tv.tv_sec,tv.tv_usec/1000);
  switch(action) {
  case CURL_POLL_IN:
  case CURL_POLL_OUT:
  case CURL_POLL_INOUT:
    curl_context = socketp ?
      (curl_context_t *) socketp : create_curl_context(s);

    curl_multi_assign(curl_handle, s, (void *) curl_context);

    if(action != CURL_POLL_IN)
      events |= UV_WRITABLE;
    if(action != CURL_POLL_OUT)
      events |= UV_READABLE;

    uv_poll_start(&curl_context->poll_handle, events, curl_perform);
    break;
  case CURL_POLL_REMOVE:
    if(socketp) {
      uv_poll_stop(&((curl_context_t*)socketp)->poll_handle);
      destroy_curl_context((curl_context_t*) socketp);
      curl_multi_assign(curl_handle, s, NULL);
    }
    break;
  default:
    abort();
  }
  fflush(stdout);
  return 0;
}

int main(int argc, char **argv)
{
  loop = uv_default_loop();
  if(argc < 4)
    return 0;
  int num_handles = atoi(argv[1]);
  times = atoi(argv[2])*num_handles;
  memset(global_url,0,sizeof(global_url));
  strcpy(global_url,argv[3]);
  
  if(curl_global_init(CURL_GLOBAL_ALL)) {
    fprintf(stderr, "Could not init curl\n");
    return 1;
  }

  uv_timer_init(loop, &timeout);

  curl_handle = curl_multi_init();
  curl_multi_setopt(curl_handle, CURLMOPT_PIPELINING, CURLPIPE_HTTP1);
  curl_multi_setopt(curl_handle, CURLMOPT_MAX_TOTAL_CONNECTIONS , num_handles);
  curl_multi_setopt(curl_handle, CURLMOPT_SOCKETFUNCTION, handle_socket);
  curl_multi_setopt(curl_handle, CURLMOPT_TIMERFUNCTION, start_timeout);

  for (int i = 0 ; i < num_handles ; i ++ ) {
    handles[i] = NULL;
  }

  add_download(global_url, num_handles);

  uv_run(loop, UV_RUN_DEFAULT);
  for (int i = 0 ; i < num_handles ; i ++ ) {
      if (handles[i]) {
        curl_easy_cleanup(handles[i]);
        handles[i] = NULL;
      }
  }
  curl_multi_cleanup(curl_handle);
  return 0;

}