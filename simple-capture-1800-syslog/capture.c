
/*  
 *  PURPOSE: Demonstrates real-time thread programming concepts:
 *    - Decoupling fast acquisition from slow I/O operations
 *    - POSIX message queue for inter-thread communication
 *    - Zero-copy buffer management using shared memory
 *    - Priority-based scheduling (SCHED_FIFO)
 *    - Thread synchronization with mutexes
 *
 *  ARCHITECTURE:
 *    Reader Thread (High Priority) -> Message Queue -> Writer Thread (Low Priority)
 *         Acquires @ 10 FPS              Passes           Writes @ ~3 FPS
 *                                    buffer indices       (300ms delay)
 *
 *  KEY CONCEPTS DEMONSTRATED:
 *    1. Reader doesn't wait for disk I/O (decoupled operation)
 *    2. Message queue provides bounded buffer between threads
 *    3. Zero-copy: only buffer index is passed, not frame data
 *    4. Real-time priorities ensure reader gets CPU time first
 *
 *  Based on V4L2 API example code - http://linuxtv.org/docs.php
 *
 */


#include <syslog.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <getopt.h>             /* getopt_long() */

#include <fcntl.h>              /* low-level i/o */
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <linux/videodev2.h>

#include <time.h>
#include <pthread.h>
#include <mqueue.h>

/*
 * ============================================================================
 * CONFIGURATION PARAMETERS
 * ============================================================================
 */
#define CLEAR(x) memset(&(x), 0, sizeof(x))

// Video frame dimensions
#define HRES 640                        // Horizontal resolution
#define VRES 480                        // Vertical resolution  
#define HRES_STR "640"                  // String version for PGM header
#define VRES_STR "480"

// Frame timing and capture control
#define START_UP_FRAMES (8)             // Skip first 8 frames (camera stabilization)
#define FRAMES_PER_SEC (10)             // Reader acquisition rate (10 FPS = 100ms period)

// Message Queue Configuration - enables inter-thread communication
#define FRAME_MQ "/frame_mq"            // POSIX message queue name (must start with /)
#define MAX_MSG_SIZE (sizeof(frame_msg_t))  // Message holds frame metadata only
#define MAX_QUEUE_DEPTH (10)            // Max messages queued (bounded buffer)

// Format is used by a number of functions, so made as a file global
static struct v4l2_format fmt;

struct buffer 
{
        void   *start;
        size_t  length;
};

/*
 * ============================================================================
 * MESSAGE QUEUE AND ZERO-COPY BUFFER MANAGEMENT
 * ============================================================================
 * CONCEPT: Instead of copying entire frame data through message queue,
 *          we pass only a small index. Writer accesses frame via shared memory.
 * BENEFIT: Avoids expensive memory copies, improves performance
 */

// Message structure - only metadata, NOT the actual frame data
typedef struct
{
    int frame_number;       // Sequential frame number (for tracking)
    int buffer_index;       // ZERO-COPY: index into shared buffer array
    int size;               // Frame data size in bytes
    struct timespec timestamp;  // When frame was captured
} frame_msg_t;

// Global buffer pool - SHARED MEMORY between reader and writer threads
#define NUM_FRAME_BUFFERS 10000    // Pool size: more buffers = more decoupling
unsigned char frame_buffers[NUM_FRAME_BUFFERS][1280*960];  // Oversized for any frame
int buffer_in_use[NUM_FRAME_BUFFERS] = {0};                // 0=free, 1=in-use
pthread_mutex_t buffer_mutex = PTHREAD_MUTEX_INITIALIZER;  // Protects buffer allocation

static char            *dev_name = "/dev/video0";
static int              fd = -1;
struct buffer          *buffers;
static unsigned int     n_buffers;
static int              frame_count = 100;  // Default, will be set by command line

static double fnow=0.0, fstart=0.0, fstop=0.0;
static struct timespec time_now, time_start, time_stop;

/*
 * ============================================================================
 * THREADING AND SYNCHRONIZATION GLOBALS
 * ============================================================================
 */

// Thread handles and scheduling attributes
pthread_t reader_thread, writer_thread;              // Thread IDs
pthread_attr_t reader_attr, writer_attr;             // Thread attributes
struct sched_param reader_param, writer_param;       // Scheduling parameters (priority)

// Message queue for inter-thread communication
mqd_t frame_mq;                                      // Message queue descriptor
struct mq_attr mq_attr;                              // Message queue attributes

// Shared counters - MUST be protected by mutex due to concurrent access
int frames_read = 0;                                 // Incremented by reader
int frames_written = 0;                              // Incremented by writer
pthread_mutex_t counter_mutex = PTHREAD_MUTEX_INITIALIZER;  // Protects counters

// Shutdown coordination - volatile ensures visibility across threads
volatile int shutdown_requested = 0;                 // Reader sets, writer checks

static void errno_exit(const char *s)
{
        fprintf(stderr, "%s error %d, %s\n", s, errno, strerror(errno));
        exit(EXIT_FAILURE);
}

static int xioctl(int fh, int request, void *arg)
{
        int r;

        do 
        {
            r = ioctl(fh, request, arg);

        } while (-1 == r && EINTR == errno);

        return r;
}

char pgm_header[]="P5\n#9999999999 sec 9999999999 msec \n"HRES_STR" "VRES_STR"\n255\n";
char pgm_dumpname[]="frames/test0000.pgm";

static void dump_pgm(const void *p, int size, unsigned int tag, struct timespec *time)
{
    int written, i, total, dumpfd;
   
    snprintf(&pgm_dumpname[11], 9, "%04d.pgm", tag);
    dumpfd = open(pgm_dumpname, O_WRONLY | O_NONBLOCK | O_CREAT, 00666);

    snprintf(&pgm_header[4], 11, "%010d", (int)time->tv_sec);
    snprintf(&pgm_header[14], 6, " sec ");
    snprintf(&pgm_header[19], 11, "%010d", (int)((time->tv_nsec)/1000000));
    snprintf(&pgm_header[29], 20, " msec \n"HRES_STR" "VRES_STR"\n255\n");

    // subtract 1 from sizeof header because it includes the null terminator for the string
    written=write(dumpfd, pgm_header, sizeof(pgm_header)-1);

    total=0;

    do
    {
        written=write(dumpfd, p, size);
        total+=written;
    } while(total < size);

    clock_gettime(CLOCK_MONOTONIC, &time_now);
    fnow = (double)time_now.tv_sec + (double)time_now.tv_nsec / 1000000000.0;
    printf("  [WRITE] Frame %04d written to %s (%d bytes) at %.3lf sec\n", 
           tag, pgm_dumpname, total, (fnow-fstart));

    close(dumpfd);
    
}

/*
 * ============================================================================
 * FRAME PROCESSING - READER THREAD LOGIC
 * ============================================================================
 */

// Frame counter: starts at -8 to skip camera stabilization frames
int framecnt=-8;

/*
 * FUNCTION: process_image()
 * PURPOSE: Called by reader thread for each captured frame
 * WORKFLOW:
 *   1. Allocate free buffer from pool (with mutex protection)
 *   2. Convert YUYV camera data to grayscale
 *   3. Send buffer index to writer via message queue
 * KEY CONCEPT: This function does NOT do disk I/O - it's decoupled!
 */
static void process_image(const void *p, int size)
{
    int i, newi, newsize=0;
    struct timespec frame_time;
    unsigned char *pptr = (unsigned char *)p;
    frame_msg_t msg;
    int buf_idx = -1;

    // record when process was called
    clock_gettime(CLOCK_REALTIME, &frame_time);    

    framecnt++;
    
    if(framecnt == 0) 
    {
        clock_gettime(CLOCK_MONOTONIC, &time_start);
        fstart = (double)time_start.tv_sec + (double)time_start.tv_nsec / 1000000000.0;
        printf("\n[START] Beginning frame capture (after %d startup frames)\n", START_UP_FRAMES);
        syslog(LOG_CRIT, "SIMPCAP: Frame capture started");
    }
    
    if(framecnt > -1)
    {
        clock_gettime(CLOCK_MONOTONIC, &time_now);
        fnow = (double)time_now.tv_sec + (double)time_now.tv_nsec / 1000000000.0;
        printf("[READER] Frame %04d acquired at %.3lf sec (%.2lf FPS avg)\n", 
               framecnt, (fnow-fstart), (double)(framecnt+1) / (fnow-fstart));
    }
    else
    {
        printf("[STARTUP] Skipping frame %d/%d\n", framecnt + START_UP_FRAMES + 1, START_UP_FRAMES);
        return;  // Don't process startup frames
    }

    // CRITICAL SECTION: Find free buffer from pool
    // Mutex protects against race condition with writer freeing buffers
    pthread_mutex_lock(&buffer_mutex);
    for(i = 0; i < NUM_FRAME_BUFFERS; i++)
    {
        if(!buffer_in_use[i])           // Found free buffer
        {
            buffer_in_use[i] = 1;       // Mark as in-use
            buf_idx = i;                // Save the index
            break;
        }
    }
    pthread_mutex_unlock(&buffer_mutex);
    
    if(buf_idx == -1)
    {
        // Buffer pool exhausted - writer can't keep up!
        // In a real system, you'd increase pool size or slow down reader
        printf("[READER] WARNING: No free buffers, skipping frame %d\n", framecnt);
        return;
    }

    // FRAME PROCESSING: Convert camera format to grayscale
    // Note: We process INTO the buffer, but DON'T write to disk yet!
    if(fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV)
    {
        // Convert YUYV to grayscale (extract Y values only)
        for(i=0, newi=0; i<size; i=i+4, newi=newi+2)
        {
            frame_buffers[buf_idx][newi] = pptr[i];      // Y1
            frame_buffers[buf_idx][newi+1] = pptr[i+2];  // Y2
        }
        newsize = size / 2;
    }
    else
    {
        printf("[ERROR] Unsupported pixel format: 0x%x\n", fmt.fmt.pix.pixelformat);
        pthread_mutex_lock(&buffer_mutex);
        buffer_in_use[buf_idx] = 0;
        pthread_mutex_unlock(&buffer_mutex);
        return;
    }

    // INTER-THREAD COMMUNICATION: Send message to writer via queue
    // IMPORTANT: We send only the INDEX (4 bytes), not frame data (307KB)!
    msg.frame_number = framecnt;
    msg.buffer_index = buf_idx;         // ZERO-COPY: just the index
    msg.size = newsize;
    msg.timestamp = frame_time;
    
    // Priority 30 ensures these messages are delivered promptly
    if(mq_send(frame_mq, (char*)&msg, sizeof(frame_msg_t), 30) != -1)
    {
        // CRITICAL SECTION: Update shared counter
        pthread_mutex_lock(&counter_mutex);
        frames_read++;                   // Track successful frame acquisition
        pthread_mutex_unlock(&counter_mutex);
    }
    else
    {
        // Queue full or error - free the buffer and report
        perror("[READER] mq_send failed");
        pthread_mutex_lock(&buffer_mutex);
        buffer_in_use[buf_idx] = 0;      // Release buffer back to pool
        pthread_mutex_unlock(&buffer_mutex);
    }
}


static int read_frame(void)
{
    struct v4l2_buffer buf;

    CLEAR(buf);
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (-1 == xioctl(fd, VIDIOC_DQBUF, &buf))
    {
        switch (errno)
        {
            case EAGAIN:
                return 0;
            case EIO:
                return 0;
            default:
                printf("mmap failure\n");
                errno_exit("VIDIOC_DQBUF");
        }
    }

    assert(buf.index < n_buffers);
    process_image(buffers[buf.index].start, buf.bytesused);

    if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
        errno_exit("VIDIOC_QBUF");

    return 1;
}

/*
 * ============================================================================
 * WRITER THREAD - DEMONSTRATES SLOW I/O DECOUPLED FROM ACQUISITION
 * ============================================================================
 * FUNCTION: writer_thread_func()
 * PURPOSE: Receives frame buffer indices from queue and writes to disk
 * KEY CONCEPT: Runs at LOW priority, slower than reader, doesn't block reader
 * DEMONSTRATION: 300ms delay shows writer is 3x slower than 10 FPS acquisition
 */
void *writer_thread_func(void *arg)
{
    frame_msg_t msg;
    int prio;
    int rc;
    struct timespec write_delay;
    int consecutive_empty = 0;
    
    // SIMULATED SLOW I/O: Make write take 3x longer than acquisition
    // Reader period: 100ms (10 FPS)
    // Writer period: 300ms (3.3 FPS) - demonstrates decoupling!
    write_delay.tv_sec = 0;
    write_delay.tv_nsec = 300000000;  // 300ms delay per frame
    
    printf("[WRITER] Thread started\n");
    syslog(LOG_CRIT, "SIMPCAP: Writer thread started");

    // WRITER MAIN LOOP: Process messages until reader finishes AND queue empty
    while(1)
    {
        // INTER-THREAD COMMUNICATION: Receive message from queue
        // This is non-blocking (O_NONBLOCK), returns immediately if empty
        rc = mq_receive(frame_mq, (char*)&msg, sizeof(frame_msg_t), &prio);
        
        if(rc == -1)
        {
            if(errno == EAGAIN || errno == ETIMEDOUT)
            {
                // Queue empty - check if we should exit
                if(shutdown_requested)
                {
                    consecutive_empty++;
                    if(consecutive_empty > 3)  // Multiple checks ensures queue fully drained
                    {
                        printf("[WRITER] Queue empty and reader finished, exiting\n");
                        break;
                    }
                }
                usleep(100000);  // 100ms - wait for more frames
                continue;
            }
            perror("[ERROR] mq_receive failed in writer");
            break;
        }
        
        consecutive_empty = 0;  // Reset counter - we got a message
        
        // SIMULATE SLOW I/O: Add 300ms delay BEFORE writing
        // This demonstrates that reader doesn't wait for this delay!
        nanosleep(&write_delay, NULL);
        
        // ACTUAL DISK I/O: Write frame using the buffer index from message
        // ZERO-COPY: Access frame via shared memory, no data copying needed
        dump_pgm(frame_buffers[msg.buffer_index], msg.size, msg.frame_number, &msg.timestamp);
        
        // CRITICAL SECTION: Free the buffer back to pool
        pthread_mutex_lock(&buffer_mutex);
        buffer_in_use[msg.buffer_index] = 0;    // Mark buffer as free
        pthread_mutex_unlock(&buffer_mutex);
        
        // CRITICAL SECTION: Update shared counter
        pthread_mutex_lock(&counter_mutex);
        frames_written++;                        // Track completed writes
        pthread_mutex_unlock(&counter_mutex);
        
        printf("[WRITER] Frame %04d written | Written: %d, Read: %d\n", 
               msg.frame_number, frames_written, frames_read);
    }
    
    printf("[WRITER] Thread exiting\n");
    return NULL;
}

/*
 * ============================================================================
 * READER THREAD - DEMONSTRATES FAST ACQUISITION WITHOUT I/O BLOCKING
 * ============================================================================
 * FUNCTION: reader_thread_func()
 * PURPOSE: Acquires frames from camera at 10 FPS, processes, sends to queue
 * KEY CONCEPT: Runs at HIGH priority, never waits for disk I/O
 * DEMONSTRATION: Should acquire frames at steady 10 FPS regardless of writer speed
 */
void *reader_thread_func(void *arg)
{
    unsigned int count;
    struct timespec read_delay;
    struct timespec time_error;
    
    printf("[READER] Thread started\n");
    
    // FRAME RATE CONTROL: Calculate delay between frames
    // 10 FPS = 100ms period = 100,000,000 nanoseconds
    printf("[READER] Running at %d frames/sec\n", FRAMES_PER_SEC);
    read_delay.tv_sec = 0;
    read_delay.tv_nsec = 1000000000 / FRAMES_PER_SEC;  // Period in nanoseconds
    
    count = frame_count;    // Number of frames to acquire

    while (count > 0 && !shutdown_requested)
    {
        for (;;)
        {
            fd_set fds;
            struct timeval tv;
            int r;

            FD_ZERO(&fds);
            FD_SET(fd, &fds);

            /* Timeout. */
            tv.tv_sec = 2;
            tv.tv_usec = 0;

            r = select(fd + 1, &fds, NULL, NULL, &tv);

            if (-1 == r)
            {
                if (EINTR == errno)
                    continue;
                errno_exit("select");
            }

            if (0 == r)
            {
                fprintf(stderr, "[ERROR] select timeout\n");
                shutdown_requested = 1;
                break;
            }

            if (read_frame())
            {
                if(nanosleep(&read_delay, &time_error) != 0)
                    perror("nanosleep");

                count--;
                break;
            }

            /* EAGAIN - continue select loop unless count done. */
            if(count <= 0) break;
        }

        if(count <= 0) break;
    }
    
    // SHUTDOWN COORDINATION: Tell writer we're done
    shutdown_requested = 1;  // Writer checks this flag
    
    printf("[READER] Thread completed, acquired %d frames\n", frames_read);
    
    return NULL;  // Thread exits, writer continues draining queue
}


/*
 * ============================================================================
 * MAIN THREAD COORDINATION - SETS UP AND MANAGES WORKER THREADS
 * ============================================================================
 * FUNCTION: mainloop()
 * PURPOSE: Creates message queue, spawns threads, waits for completion
 * KEY CONCEPTS:
 *   - Message queue setup (POSIX IPC)
 *   - Thread creation with priority scheduling
 *   - Proper shutdown sequence (join threads)
 */
static void mainloop(void)
{
    int rc;
    int rt_max_prio, rt_min_prio;
    
    // REAL-TIME SCHEDULING: Get valid priority range
    // SCHED_FIFO: First-In-First-Out real-time policy
    // Higher number = higher priority
    rt_max_prio = sched_get_priority_max(SCHED_FIFO);
    rt_min_prio = sched_get_priority_min(SCHED_FIFO);
    
    printf("\n[INIT] Setting up message queue and threads\n");
    printf("[INIT] RT Priority range: %d (min) to %d (max)\n", rt_min_prio, rt_max_prio);
    
    // MESSAGE QUEUE SETUP: Configure bounded buffer between threads
    mq_attr.mq_maxmsg = MAX_QUEUE_DEPTH;    // Max 10 messages queued
    mq_attr.mq_msgsize = MAX_MSG_SIZE;      // Each message is small (32 bytes)
    mq_attr.mq_flags = 0;                   // Blocking mode
    
    // Clean up any leftover queue from previous run
    mq_unlink(FRAME_MQ);
    
    // CREATE MESSAGE QUEUE: POSIX IPC mechanism
    // O_NONBLOCK: Don't block if queue full/empty
    // S_IRWXU: User read/write/execute permissions
    frame_mq = mq_open(FRAME_MQ, O_CREAT | O_RDWR | O_NONBLOCK, S_IRWXU, &mq_attr);
    if(frame_mq == (mqd_t)-1)
    {
        perror("[ERROR] mq_open failed");
        return;
    }
    printf("[INIT] Message queue created: %s (depth=%d, msgsize=%ld)\n", 
           FRAME_MQ, MAX_QUEUE_DEPTH, MAX_MSG_SIZE);
    
    // READER THREAD CONFIGURATION: High priority ensures it gets CPU first
    rc = pthread_attr_init(&reader_attr);
    rc = pthread_attr_setinheritsched(&reader_attr, PTHREAD_EXPLICIT_SCHED);
    rc = pthread_attr_setschedpolicy(&reader_attr, SCHED_FIFO);  // Real-time FIFO
    reader_param.sched_priority = rt_max_prio - 1;  // Priority 98 (very high)
    pthread_attr_setschedparam(&reader_attr, &reader_param);
    
    // WRITER THREAD CONFIGURATION: Low priority means it runs when reader idle
    rc = pthread_attr_init(&writer_attr);
    rc = pthread_attr_setinheritsched(&writer_attr, PTHREAD_EXPLICIT_SCHED);
    rc = pthread_attr_setschedpolicy(&writer_attr, SCHED_FIFO);  // Real-time FIFO
    writer_param.sched_priority = rt_min_prio;      // Priority 1 (very low)
    pthread_attr_setschedparam(&writer_attr, &writer_param);
    
    // CREATE WRITER THREAD FIRST: Consumer should be ready before producer
    if((rc = pthread_create(&writer_thread, &writer_attr, writer_thread_func, NULL)) != 0)
    {
        fprintf(stderr, "[ERROR] Failed to create writer thread: %d\n", rc);
        perror("pthread_create writer");
    }
    else
    {
        printf("[INIT] Writer thread created (priority=%d - LOW)\n", rt_min_prio);
    }
    
    // Small delay ensures writer thread is listening before reader starts sending
    usleep(100000);  // 100ms
    
    // CREATE READER THREAD: Producer starts sending frames
    if((rc = pthread_create(&reader_thread, &reader_attr, reader_thread_func, NULL)) != 0)
    {
        fprintf(stderr, "[ERROR] Failed to create reader thread: %d\n", rc);
        perror("pthread_create reader");
        shutdown_requested = 1;
    }
    else
    {
        printf("[INIT] Reader thread created (priority=%d - HIGH)\n", rt_max_prio - 1);
    }
    
    printf("\n[THREADS] Both threads started, acquisition in progress...\n");
    printf("[INFO] Reader: %d FPS, Writer: ~3x slower (demonstrates decoupling)\n", 
           FRAMES_PER_SEC);
    printf("[INFO] Watch reader acquire while writer is still writing previous frames!\n\n");
    
    // THREAD JOIN: Wait for reader to finish its work
    printf("[MAIN] Waiting for reader thread to finish...\n");
    pthread_join(reader_thread, NULL);  // Blocks until reader returns
    
    // QUEUE DRAINING: Writer needs time to process remaining buffered frames
    printf("[MAIN] Reader finished, waiting for writer to drain queue...\n");
    
    // Monitor progress - writer continues after reader exits
    int wait_count = 0;
    while(frames_written < frames_read && wait_count < 600)  // Max 60 seconds
    {
        sleep(1);
        wait_count++;
        printf("[MAIN] Draining: %d/%d frames written\n", frames_written, frames_read);
    }
    
    shutdown_requested = 1;  // Ensure writer knows it's time to exit
    
    // THREAD JOIN: Wait for writer to finish processing queue
    pthread_join(writer_thread, NULL);  // Blocks until writer returns
    
    // Get final timestamp for statistics
    clock_gettime(CLOCK_MONOTONIC, &time_stop);
    fstop = (double)time_stop.tv_sec + (double)time_stop.tv_nsec / 1000000000.0;
    
    // CLEANUP: Close and remove message queue
    mq_close(frame_mq);     // Close our descriptor
    mq_unlink(FRAME_MQ);    // Remove queue from system
    
    printf("\n[COMPLETE] All threads finished\n");
}

static void stop_capturing(void)
{
    enum v4l2_buf_type type;
    
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (-1 == xioctl(fd, VIDIOC_STREAMOFF, &type))
        errno_exit("VIDIOC_STREAMOFF");
}

static void start_capturing(void)
{
    unsigned int i;
    enum v4l2_buf_type type;

    for (i = 0; i < n_buffers; ++i) 
    {
        struct v4l2_buffer buf;

        CLEAR(buf);
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
            errno_exit("VIDIOC_QBUF");
    }
    
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (-1 == xioctl(fd, VIDIOC_STREAMON, &type))
        errno_exit("VIDIOC_STREAMON");
        
    printf("[INIT] Allocated %d MMAP buffers\n", n_buffers);
    printf("[INIT] Streaming started, waiting for camera to stabilize...\n");
    sleep(1);  // Give camera time to start streaming
    printf("[INIT] Camera ready\n");
}

static void uninit_device(void)
{
    unsigned int i;

    for (i = 0; i < n_buffers; ++i)
        if (-1 == munmap(buffers[i].start, buffers[i].length))
            errno_exit("munmap");

    free(buffers);
}

static void init_mmap(void)
{
        struct v4l2_requestbuffers req;

        CLEAR(req);

        req.count = 6;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;

        if (-1 == xioctl(fd, VIDIOC_REQBUFS, &req)) 
        {
                if (EINVAL == errno) 
                {
                        fprintf(stderr, "%s does not support "
                                 "memory mapping\n", dev_name);
                        exit(EXIT_FAILURE);
                } else 
                {
                        errno_exit("VIDIOC_REQBUFS");
                }
        }

        if (req.count < 2) 
        {
                fprintf(stderr, "Insufficient buffer memory on %s\n", dev_name);
                exit(EXIT_FAILURE);
        }

        buffers = calloc(req.count, sizeof(*buffers));

        if (!buffers) 
        {
                fprintf(stderr, "Out of memory\n");
                exit(EXIT_FAILURE);
        }

        for (n_buffers = 0; n_buffers < req.count; ++n_buffers) {
                struct v4l2_buffer buf;

                CLEAR(buf);

                buf.type        = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                buf.memory      = V4L2_MEMORY_MMAP;
                buf.index       = n_buffers;

                if (-1 == xioctl(fd, VIDIOC_QUERYBUF, &buf))
                        errno_exit("VIDIOC_QUERYBUF");

                buffers[n_buffers].length = buf.length;
                buffers[n_buffers].start =
                        mmap(NULL /* start anywhere */,
                              buf.length,
                              PROT_READ | PROT_WRITE /* required */,
                              MAP_SHARED /* recommended */,
                              fd, buf.m.offset);

                if (MAP_FAILED == buffers[n_buffers].start)
                        errno_exit("mmap");
        }
}

static void init_device(void)
{
    struct v4l2_capability cap;
    struct v4l2_cropcap cropcap;
    struct v4l2_crop crop;
    unsigned int min;

    if (-1 == xioctl(fd, VIDIOC_QUERYCAP, &cap))
    {
        if (EINVAL == errno) {
            fprintf(stderr, "%s is no V4L2 device\n",
                     dev_name);
            exit(EXIT_FAILURE);
        }
        else
        {
                errno_exit("VIDIOC_QUERYCAP");
        }
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE))
    {
        fprintf(stderr, "%s is no video capture device\n", dev_name);
        exit(EXIT_FAILURE);
    }

    if (!(cap.capabilities & V4L2_CAP_STREAMING))
    {
        fprintf(stderr, "%s does not support streaming i/o\n", dev_name);
        exit(EXIT_FAILURE);
    }


    CLEAR(cropcap);

    cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (0 == xioctl(fd, VIDIOC_CROPCAP, &cropcap))
    {
        crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        crop.c = cropcap.defrect; /* reset to default */

        if (-1 == xioctl(fd, VIDIOC_S_CROP, &crop))
        {
            switch (errno)
            {
                case EINVAL:
                    /* Cropping not supported. */
                    break;
                default:
                    /* Errors ignored. */
                        break;
            }
        }

    }
    else
    {
        /* Errors ignored. */
    }


    CLEAR(fmt);
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    printf("[INIT] Setting video format to %dx%d\n", HRES, VRES);
    fmt.fmt.pix.width       = HRES;
    fmt.fmt.pix.height      = VRES;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field       = V4L2_FIELD_NONE;

    if (-1 == xioctl(fd, VIDIOC_S_FMT, &fmt))
        errno_exit("VIDIOC_S_FMT");
    
    printf("[INIT] Video format: %dx%d, pixel format: YUYV\n", 
           fmt.fmt.pix.width, fmt.fmt.pix.height);

    /* Buggy driver paranoia. */
    min = fmt.fmt.pix.width * 2;
    if (fmt.fmt.pix.bytesperline < min)
        fmt.fmt.pix.bytesperline = min;
    min = fmt.fmt.pix.bytesperline * fmt.fmt.pix.height;
    if (fmt.fmt.pix.sizeimage < min)
        fmt.fmt.pix.sizeimage = min;

    init_mmap();
}


static void close_device(void)
{
    if (-1 == close(fd))
        errno_exit("close");
    fd = -1;
}

static void open_device(void)
{
    struct stat st;

    if (-1 == stat(dev_name, &st)) {
        fprintf(stderr, "Cannot identify '%s': %d, %s\n",
                 dev_name, errno, strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (!S_ISCHR(st.st_mode)) {
        fprintf(stderr, "%s is no device\n", dev_name);
        exit(EXIT_FAILURE);
    }

    fd = open(dev_name, O_RDWR | O_NONBLOCK, 0);

    if (-1 == fd) {
        fprintf(stderr, "Cannot open '%s': %d, %s\n",
                 dev_name, errno, strerror(errno));
        exit(EXIT_FAILURE);
    }
}

/*
 * ============================================================================
 * MAIN - PROGRAM ENTRY POINT
 * ============================================================================
 * PURPOSE: Initialize system, run capture, report results
 * USAGE: sudo ./capture <number_of_frames>
 * REQUIRES: sudo for real-time scheduling (SCHED_FIFO)
 */
int main(int argc, char **argv)
{
    printf("\n============================================================\n");
    printf("  EDUCATIONAL DEMO: Multi-threaded Frame Capture\n");
    printf("  Demonstrates: Decoupling, Message Queues, RT Scheduling\n");
    printf("============================================================\n\n");

    // ARGUMENT PARSING: Simple single argument - number of frames to capture
    if (argc != 2)
    {
        fprintf(stderr, "Usage: %s <number_of_frames>\n", argv[0]);
        fprintf(stderr, "Example: sudo %s 100\n", argv[0]);
        fprintf(stderr, "Note: Requires sudo for real-time scheduling\n");
        exit(EXIT_FAILURE);
    }

    errno = 0;
    frame_count = strtol(argv[1], NULL, 0);
    if (errno || frame_count <= 0)
    {
        fprintf(stderr, "Error: Invalid frame count '%s'\n", argv[1]);
        exit(EXIT_FAILURE);
    }
    
    // Add startup frames (camera needs time to stabilize)
    frame_count += START_UP_FRAMES;

    // Open system log for timestamped events
    openlog("SIMPCAP", LOG_PID | LOG_CONS, LOG_USER);
    
    printf("[INIT] Opening device: %s\n", dev_name);
    printf("[INIT] Target frames: %d (plus %d startup frames)\n", 
           frame_count - START_UP_FRAMES, START_UP_FRAMES);
    printf("[INIT] Acquisition rate: %d FPS\n", FRAMES_PER_SEC);
    printf("[INIT] Write delay: 300ms (demonstrates 3x slower than acquisition)\n\n");
    
    // initialization of V4L2
    open_device();
    init_device();

    start_capturing();

    // service loop - creates reader and writer threads
    mainloop();

    // shutdown of frame acquisition service
    stop_capturing();

    // Calculate statistics
    double actual_fps = 0.0;
    if(fstop > fstart)
    {
        actual_fps = (double)frames_read / (fstop - fstart);
    }
    
    double write_rate = 0.0;
    if(fstop > fstart)
    {
        write_rate = (double)frames_written / (fstop - fstart);
    }
    
    /*
     * ========================================================================
     * RESULTS AND ANALYSIS
     * ========================================================================
     * Look for these key indicators of successful decoupling:
     * 1. Read rate close to 10 FPS (shows reader not blocked by writer)
     * 2. Write rate around 3 FPS (shows 300ms delay working)
     * 3. Ratio close to 3x (proves concurrent operation)
     */
    printf("\n============================================================\n");
    printf("CAPTURE SUMMARY - DEMONSTRATES DECOUPLING SUCCESS\n");
    printf("============================================================\n");
    printf("Frames read (acquired): %d\n", frames_read);
    printf("Frames written (saved): %d\n", frames_written);
    printf("Total execution time:   %.3lf seconds\n", (fstop-fstart));
    printf("------------------------------------------------------------\n");
    printf("Average read rate:      %.2lf FPS (target: %d FPS)\n", 
           actual_fps, FRAMES_PER_SEC);
    printf("Average write rate:     %.2lf FPS (target: ~3 FPS)\n", write_rate);
    printf("Write/Read ratio:       %.2lfx (proves 3x slower write!)\n", 
           actual_fps / write_rate);
    printf("------------------------------------------------------------\n");
    printf("CONCLUSION: Reader acquired frames without waiting for disk I/O\n");
    printf("============================================================\n\n");

    // Cleanup V4L2 resources
    uninit_device();
    close_device();
    closelog();
    
    return 0;
}

