/*******************************************************************************
#                                                                              #
#      MJPG-streamer allows to stream JPG frames from an input-plugin          #
#      to several output plugins                                               #
#                                                                              #
#      Copyright (C) 2007 Tom Stöveken                                         #
#      Modified (C) 2025 For stdin input                                       #
#                                                                              #
# This program is free software; you can redistribute it and/or modify         #
# it under the terms of the GNU General Public License as published by         #
# the Free Software Foundation; version 2 of the License.                      #
#                                                                              #
# This program is distributed in the hope that it will be useful,              #
# but WITHOUT ANY WARRANTY; without even the implied warranty of               #
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the                #
# GNU General Public License for more details.                                 #
#                                                                              #
# You should have received a copy of the GNU General Public License            #
# along with this program; if not, write to the Free Software                  #
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA    #
#                                                                              #
*******************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <pthread.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include "../../mjpg_streamer.h"
#include "../../utils.h"

#define INPUT_PLUGIN_NAME "STDIN input plugin"
#define MAX_FRAME_SIZE (10 * 1024 * 1024)  // 10MB max frame size
#define BOUNDARY_LEN 128

/* private functions and variables to this plugin */
static pthread_t   worker;
static globals     *pglobal;

void *worker_thread(void *);
void worker_cleanup(void *);
void help(void);

static int plugin_number;
static int use_mjpeg_boundary = 0;
static char boundary[BOUNDARY_LEN] = "--boundary";

/* global variables for this plugin */
static int stdin_fd = STDIN_FILENO;

/*** plugin interface functions ***/
int input_init(input_parameter *param, int id)
{
    int i;
    plugin_number = id;

    param->argv[0] = INPUT_PLUGIN_NAME;

    /* show all parameters for DBG purposes */
    for(i = 0; i < param->argc; i++) {
        DBG("argv[%d]=%s\n", i, param->argv[i]);
    }

    reset_getopt();
    while(1) {
        int option_index = 0, c = 0;
        static struct option long_options[] = {
            {"h", no_argument, 0, 0},
            {"help", no_argument, 0, 0},
            {"b", required_argument, 0, 0},
            {"boundary", required_argument, 0, 0},
            {"m", no_argument, 0, 0},
            {"mjpeg", no_argument, 0, 0},
            {0, 0, 0, 0}
        };

        c = getopt_long_only(param->argc, param->argv, "", long_options, &option_index);

        /* no more options to parse */
        if(c == -1) break;

        /* unrecognized option */
        if(c == '?') {
            help();
            return 1;
        }

        switch(option_index) {
            /* h, help */
        case 0:
        case 1:
            DBG("case 0,1\n");
            help();
            return 1;
            break;

            /* b, boundary */
        case 2:
        case 3:
            DBG("case 2,3\n");
            strncpy(boundary, optarg, BOUNDARY_LEN - 1);
            boundary[BOUNDARY_LEN - 1] = '\0';
            use_mjpeg_boundary = 1;
            break;

            /* m, mjpeg */
        case 4:
        case 5:
            DBG("case 4,5\n");
            use_mjpeg_boundary = 1;
            break;

        default:
            DBG("default case\n");
            help();
            return 1;
        }
    }

    pglobal = param->global;

    IPRINT("STDIN input plugin initialized\n");
    IPRINT("MJPEG boundary mode: %s\n", use_mjpeg_boundary ? "enabled" : "disabled");
    if(use_mjpeg_boundary) {
        IPRINT("Boundary string: %s\n", boundary);
    }

    param->global->in[id].name = malloc((strlen(INPUT_PLUGIN_NAME) + 1) * sizeof(char));
    sprintf(param->global->in[id].name, INPUT_PLUGIN_NAME);

    return 0;
}

int input_stop(int id)
{
    DBG("will cancel input thread\n");
    pthread_cancel(worker);
    return 0;
}

int input_run(int id)
{
    pglobal->in[id].buf = NULL;

    if(pthread_create(&worker, 0, worker_thread, NULL) != 0) {
        free(pglobal->in[id].buf);
        fprintf(stderr, "could not start worker thread\n");
        exit(EXIT_FAILURE);
    }

    pthread_detach(worker);

    return 0;
}

/*** private functions for this plugin below ***/
void help(void)
{
    fprintf(stderr, " ---------------------------------------------------------------\n" \
    " Help for input plugin..: "INPUT_PLUGIN_NAME"\n" \
    " ---------------------------------------------------------------\n" \
    " The following parameters can be passed to this plugin:\n\n" \
    " [-h | --help ].........: show this help\n" \
    " [-b | --boundary ].....: MJPEG boundary string (default: --boundary)\n" \
    " [-m | --mjpeg ]........: enable MJPEG stream mode with boundaries\n" \
    " ---------------------------------------------------------------\n");
}

/* Read line from stdin */
static int read_line(char *buffer, int max_len)
{
    int i = 0;
    char c;
    
    while (i < max_len - 1) {
        if (read(stdin_fd, &c, 1) != 1) {
            return -1;
        }
        
        if (c == '\n') {
            buffer[i] = '\0';
            return i;
        } else if (c == '\r') {
	    char next;
	    if (read(stdin_fd, &next, 1) != 1) {
		buffer[i] = '\0';
		return i;
	    }
	    if (next == '\n') {
		buffer[i] = '\0';
		return i;
	    } else {
		buffer[i++] = c;
		if (i < max_len - 1) {
		    buffer[i++] = next;
		}
	    }
	} else {
            buffer[i++] = c;
	}
    }
    
    buffer[max_len - 1] = '\0';
    return max_len - 1;
}

/* Skip data until boundary found */
static int skip_to_boundary(void)
{
    char buffer[1024];
    int boundary_len = strlen(boundary);
    int matched = 0;
    int i;
    char c;
    
    while(1) {
        if(read(stdin_fd, &c, 1) != 1) {
            return -1;
        }
        
        if(c == boundary[matched]) {
            matched++;
            if(matched == boundary_len) {
                // Read the rest of the boundary line
                while(1) {
                    if(read(stdin_fd, &c, 1) != 1) return -1;
                    if(c == '\n') break;
                }
                return 0;
            }
        } else {
            matched = 0;
        }
    }
}

/* Parse MJPEG headers */
static int parse_mjpeg_headers(size_t *content_length)
{
    char buffer[1024];
    char *key, *value;
    
    *content_length = 0;
    
    while(1) {
        if(read_line(buffer, sizeof(buffer)) < 0) {
            return -1;
        }
        
        // Empty line means end of headers
        if (buffer[0] == '\0' || buffer[0] == '\r') {
            break;
	}
	
        // Parse Content-Length header
        if(strncasecmp(buffer, "Content-Length:", 15) == 0) {
            *content_length = atol(buffer + 15);
        }
        // Parse Content-Type header (optional)
        else if(strncasecmp(buffer, "Content-Type:", 13) == 0) {
            // We don't need to do anything with this, just skip
        }
    }
    
    return 0;
}

/* the single writer thread */
void *worker_thread(void *arg)
{
    unsigned char *frame_buffer = NULL;
    size_t frame_size = 0;
    size_t bytes_read = 0;
    struct timeval timestamp;
    int ret;
    
    /* set cleanup handler to cleanup allocated resources */
    pthread_cleanup_push(worker_cleanup, NULL);
    
    // Allocate initial buffer
    frame_buffer = malloc(MAX_FRAME_SIZE);
    if(frame_buffer == NULL) {
        perror("Could not allocate frame buffer");
        goto thread_quit;
    }
    
    IPRINT("STDIN input thread started, waiting for data...\n");
    
    while(!pglobal->stop) {
        if(use_mjpeg_boundary) {
            // MJPEG stream mode: look for boundary
            ret = skip_to_boundary();
            if(ret < 0) {
                if(errno == EAGAIN || errno == EWOULDBLOCK) {
                    usleep(10000); // 10ms
                    continue;
                }
                perror("Error finding boundary");
                break;
            }
            
            // Parse headers
            ret = parse_mjpeg_headers(&frame_size);
            if(ret < 0 || frame_size == 0 || frame_size > MAX_FRAME_SIZE) {
                if(ret < 0) perror("Error parsing headers");
                else fprintf(stderr, "Invalid content length: %zu\n", frame_size);
                continue;
            }
        } else {
            // Raw JPEG mode: read until EOF or error
            frame_size = 0;
        }
        
        // Read frame data
        bytes_read = 0;
        while(bytes_read < frame_size) {
            ret = read(stdin_fd, frame_buffer + bytes_read, frame_size - bytes_read);
            if(ret < 0) {
                if(errno == EAGAIN || errno == EWOULDBLOCK) {
                    usleep(1000); // 1ms
                    continue;
                }
                perror("Error reading frame data");
                goto thread_quit;
            } else if(ret == 0) {
                // EOF
                break;
            }
            bytes_read += ret;
        }
        
        if(bytes_read == 0) {
            // No data read, maybe just wait
            usleep(100000); // 100ms
            continue;
        }
        
        // Update actual frame size for raw mode
        if(!use_mjpeg_boundary) {
            frame_size = bytes_read;
        }
        
        // Copy frame to global buffer
        pthread_mutex_lock(&pglobal->in[plugin_number].db);
        
        /* allocate memory for frame */
        if(pglobal->in[plugin_number].buf != NULL) {
            free(pglobal->in[plugin_number].buf);
        }
        
        pglobal->in[plugin_number].buf = malloc(frame_size + (1 << 16));
        if(pglobal->in[plugin_number].buf == NULL) {
            fprintf(stderr, "could not allocate memory for frame\n");
            pthread_mutex_unlock(&pglobal->in[plugin_number].db);
            continue;
        }
        
        memcpy(pglobal->in[plugin_number].buf, frame_buffer, frame_size);
        pglobal->in[plugin_number].size = frame_size;

        gettimeofday(&timestamp, NULL);
        pglobal->in[plugin_number].timestamp = timestamp;
        
        DBG("new frame received (size: %zu)\n", frame_size);
        
        /* signal fresh_frame */
        pthread_cond_broadcast(&pglobal->in[plugin_number].db_update);
        pthread_mutex_unlock(&pglobal->in[plugin_number].db);
    }
    
thread_quit:
    if(frame_buffer != NULL) {
        free(frame_buffer);
    }
    
    DBG("leaving input thread, calling cleanup function now\n");
    /* call cleanup handler, signal with the parameter */
    pthread_cleanup_pop(1);
    
    return NULL;
}

void worker_cleanup(void *arg)
{
    static unsigned char first_run = 1;
    
    if(!first_run) {
        DBG("already cleaned up resources\n");
        return;
    }
    
    first_run = 0;
    DBG("cleaning up resources allocated by input thread\n");
    
    if(pglobal->in[plugin_number].buf != NULL) {
        free(pglobal->in[plugin_number].buf);
        pglobal->in[plugin_number].buf = NULL;
    }
    
    IPRINT("STDIN input plugin cleaned up\n");
}
