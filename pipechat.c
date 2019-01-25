/*

  Copyright (c) 2018 Andrew Benson

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.

*/

/* minitalk fork with fifodir notification system implementation by: */

/*

  Copyright (c) 2018, Martin Mišúth - /ETC, 960 01 Zvolen, Slovak Republic
  All rights reserved.

  This code is derived by Martin "eto" Mišúth from minitalk project
  by Andrew Benson.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

  1. Redistributions of source code must retain the above copyright notice, this
     list of conditions and the following disclaimer.
  2. Redistributions in binary form must reproduce the above copyright notice,
     this list of conditions and the following disclaimer in the documentation
     and/or other materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
  ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
  ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
  (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
  ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

  The views and conclusions contained in the software and documentation are those
  of the authors and should not be interpreted as representing official policies,
  either expressed or implied, of neither the minitalk project nor pipechat project.

  In case, when support or consultation related to the modified version
  of this code is needed, you can contact it's author, Martin Mišúth
  directly at code.support{at}ethome.sk (replace {at} with @) email
  address, or submit inquiry at https://etc.ethome.eu/code-support.

*/

/* Word about minitalk, fifodirs and pipechat
 *
 * Original implementation of this program called
 * minitalk uses traditional unix' "tail -f"-like
 * polling approach to detect chatlog modification,
 * implemented using combination of usleep() +
 * select() polling mechanism.
 *
 * This mechanism is not very optimal and is a bit
 * wasteful and somewhat crude, but it works.
 *
 * Minitalk periodically wakes up to check new
 * chatlog messages, but it just blindly reads
 * that log and has no notion of other readers
 * and writes or writers.
 *
 * I wanted something as simple as the original
 * minitalk but even more resurce friendly and
 * preferably event based, so that it knows
 * when events occured.
 *
 * I think, I have achieved that goal. Almost
 * zero CPU consumption with no polling was
 * achieved for majority of the process runtime.
 *
 * This is possible by applying several seminal
 * concepts pioneered by Daniel "djb" Bertstein
 * and Laurent "skarnet" Bercot.
 *
 * First, we are actually using eventloop
 * based mainloop, implemented through poll()
 * sycall, to watch over several file descripotors,
 * treating them as event sources.
 *
 * First such descriptor is used to increase
 * overall program reliability, aand it is
 * seminal selfpipe trick discovered (or at
 * least popularised) by Bernstein:
 *  - http://cr.yp.to/docs/selfpipe.html
 *  - https://skarnet.org/software/skalibs/libstddjb/selfpipe.html
 *
 * This very powerful design approach
 * allows one to safely mix select()/poll()
 * event cores with signal handling.
 *
 * Second descriptor we observe is
 * fifodir's event source pipe.
 *
 * Each instance of pipechat "connected"
 * to the same chatlog (or rather chatdir)
 * creates it's own fifo pipe (representing
 * its chatdir "connection"), in specific
 * subdirectory within that chatdir.
 *
 * This directory is rendevouz point of all
 * connected "clients" and is known as
 * fifodir.
 *
 * When any of the connected pipechat
 * instances performs any action, like,
 * for example, appends a message to chatlog,
 * it is also responsible to write single
 * byte, representing given event as
 * notification, into all pipes that exist
 * in fifodir and are representing all
 * chat "clients" or rather all "connected"
 * pipechat instances.
 *
 * Because these instaces are all "stuck"
 * blocked in poll() indefinitely, they are
 * deep "sleeping" not consuming any CPU.
 *
 * But when "event" byte is written
 * into their event pipe, poll() syscall
 * exits and each pipechat resumes its
 * operation. By reading and decoding
 * byte that was written, it now knows,
 * what kind of action writer wants
 * it to perform, like, for example
 * "to read new data from chatlog".
 *
 * Once event is handled, each
 * pipechat re-invokes poll() and
 * thus enters deep sleep again.
 *
 * This way multiple pipechat
 * instances can coordinate chatlog
 * access and maximize conservation
 * of CPU resources. They only read
 * new chat messages when they are
 * written.
 *
 * Final desciptor observed is,
 * naturally, process stdin
 * descriptor which gets readable
 * everytime user presses a key.
 *
 * In such case we want to invoke
 * pipechat command line processing
 * no matter what is happening on
 * other descriptors.
 *
 *
 */

/***********************************************************************
 * pipechat - small chat system for multiple users on a UNIX-like host *
 ***********************************************************************/

#define VERSION "0.0.1"

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <errno.h>

#include <unistd.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <poll.h>
#include <dirent.h>
#include <pwd.h>
#include <grp.h>

#include <readline/readline.h>
#include <readline/history.h>


// maximum length of a nick/username string (will be truncated)
#define MAX_NICK_LEN 15

// time format is fixed
#define TIME_STR_FORMAT "%Y.%m.%d %H:%M:%S"
#define MAX_TIME_STR_LEN 20

// maximum lenght of notify pipe name
#define MAX_NOTIFY_NAME_LEN 32

// maximum lenght of generic infoline
#define MAX_INFO_LINE_LEN 128

// maximum supported chatlog read buffer size, including timestamps/usernames
#define MAX_CHAT_READ_BUFFER_LEN 1024

// points to global var
#define PROMPT promptstr


typedef enum check_result_e {
    CHECK_ERROR = -1,
    CHECK_NOTHING,
    CHECK_TIMEOUT,
    CHECK_SIGNAL,
    CHECK_INPUT,
    CHECK_MESSAGE,
} check_result;

typedef struct fds_s {
    int fd_selfpipe;  // "selfpipe"  for reliable signals
    int fd_chatdir;   // "channel"   dirfd holding dir to the chat channel data
    int fd_chatlog;   // "chatlog"   fd holding regular chat log data file
    int fd_event;     // "eventpipe" fd holding pipe, where notifications about new messages are sent
} fds_t;


// GLOBALS


// readline completion
char * pipechat_commands[] = {

    "/?",
    "/h",
    "/q",
    "/l",
    "/w",
    "/p",
    "/s",

    "/help",
    "/quit",
    "/list",
    "/whois",
    "/ptyof",
//    "/save",
    "/destroy",

    NULL
};

// important fds, see above struct
fds_t fds = {0};

// fifodir holding event listeners
DIR * event_fifodir;

// process effective GID
gid_t egid = 0;


// various cached strings
static char pidstr_buf[30] = {0};
static char promptstr_buf[64] = {0};

static char * chatdirstr = NULL;
static char * nickstr = NULL;
static char * groupstr = NULL;
static char * pidstr = &pidstr_buf[0];
static char * promptstr = &promptstr_buf[0];

// track of the last position we read from chatlog.
static long last_chatlog_read_pos = 0;

// boolean magic
typedef enum { NO, YES } BOOL;

// process eventloop core will run as long as this is set to YES.
BOOL run = YES;
BOOL log_leaving_message = YES;


// few forward declarations
static void send_message (const char *message);
int notify_new_message(DIR * eventdirptr);


// marks fd as CLOEXEC (close on exec)
size_t
fd_coe (int fd)
{
    int fd_flags = 0;
    if ((fd_flags = fcntl(fd, F_GETFD)) == -1) {
        return 0;
    }
    fd_flags |= FD_CLOEXEC;
    if (fcntl(fd, F_SETFD, fd_flags) == -1) {
        return 0;
    }
    return 1;
}


// closes fd
void
fd_close (int fdtoclose)
{
    int e = 0, res = -1;
    e = errno;
    do {
        res = close(fdtoclose);
    } while ((res == -1) && errno == EINTR);
    errno = e;
}


// writes buffer into fd
int
fd_write (int fd, void * data, size_t size)
{
    int res = -1;
    do {
        res = write(fd, data, size);
    } while ((res == -1) && errno == EINTR);
    return res;
}


// writes string into fd
int
fd_writestr (int fd, char * data)
{
    int res = -1;
    do {
        res = write(fd, data, strlen(data));
    } while ((res == -1) && errno == EINTR);
    return res;
}


// reads buffer from fd
int
fd_read (const int fd, char *data, const size_t size)
{
    int res = -1;

    do {
        res = read(fd, data, size);
        if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
            break;
        } else if (res == 0) {
            errno = EPIPE;
            res = -1;
        }
    } while ((res == -1) && errno == EINTR);

    return res;
}


// reads buffer at position from fd
int
fd_pread (const int fd, char *data, const size_t size, long pos)
{
    int res = -1;

    do {
        res = pread(fd, data, size, pos);
        if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
            break;
        } else if (res == 0) {
            errno = EPIPE;
            res = -1;
        }
    } while ((res == -1) && errno == EINTR);

    return res;
}


// reads string from fd
int
fd_readstr (const int fd, char ** data, const size_t size)
{
    int res = -1;

    if (data == NULL) {
        errno = EINVAL;
        return -1;
    }

    *(data) = calloc(size, 1);

    if (*(data)) {
        res = fd_read(fd, *(data), size);
    } else {
        errno = ENOMEM;
        return -1;
    }
    return res;
}


// "spits" buffer into filename anchored at dirfd
int
fd_spitat (const int dirfd, const char * filename, char * data, const size_t size)
{
    int fd = -1, res = -1;

    do {
        fd = openat(dirfd, filename, O_WRONLY | O_NONBLOCK);
    } while ((fd == -1) && errno == EINTR);

    if (fd == -1) {
        return -1;
    }

    res = fd_write(fd, data, size);
    fd_close(fd);

    return res;
}


// "spits" string into filename anchored at dirfd
int
fd_spitstrat (const int dirfd, const char * filename, char * data)
{
    int fd = -1, res = -1;

    do {
        fd = openat(dirfd, filename, O_WRONLY | O_NONBLOCK);
    } while ((fd == -1) && errno == EINTR);

    if (fd == -1) {
        return -1;
    }

    res = fd_writestr(fd, data);

    fd_close(fd);

    return res;
}


// opens dirfd at specified path
int
dfd_opendir (const char *dirname)
{
    int dfd = -1;

    do {
        dfd = open(dirname, O_RDONLY | O_NONBLOCK | O_DIRECTORY);
    } while ((dfd == -1) && errno == EINTR);

    return dfd;
}


// opens dirfd "anchored" at another dirfd
int
dfd_openat (int dirfd, const char *dirname)
{
    int dfd = -1;

    do {
        dfd = openat(dirfd, dirname, O_RDONLY | O_NONBLOCK | O_DIRECTORY);
    } while ((dfd == -1) && errno == EINTR);
    return dfd;
}


// opendir()s dir "anchored" at dirfd
DIR *
dir_opendirat (int dirfd, const char *dirname)
{
    int dfd = -1;

    do {
        dfd = openat(dirfd, dirname, O_RDONLY | O_NONBLOCK | O_DIRECTORY);
    } while ((dfd == -1) && errno == EINTR);

    if (dfd == -1) {
        return NULL;
    }

    DIR * dirptr = fdopendir(dfd);
    if (dirptr == NULL) {
        close(dfd);
    }

    return dirptr;
}


// recursively removes children of dirfd
int
dfd_rmr(int dirfd)
{
    int res = 0;
    DIR *dirptr;
    struct stat sb;
    struct dirent *dentry;

    if (dirfd < 0) {
        errno = EBADF;
        return -1;
    }

    if (fstat(dirfd, &sb) < 0) {
        return -1;
    }

    if (!S_ISDIR(sb.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }

    dirptr = fdopendir(dirfd);

    // iteration through entries in the directory
    while ((dentry = readdir(dirptr)) != NULL) {

        // skip entries "." and ".."
        if (!strcmp(dentry->d_name, ".") || !strcmp(dentry->d_name, "..")) {
            continue;
        }

        if (fstatat(dirfd, dentry->d_name, &sb, 0) < 0) {
            if (errno != ENOENT) {
                dprintf(2, "Error stating item '%s': %s", dentry->d_name, strerror(errno));
                return -1;
            }
            continue;
        }

        // recursively remove a nested directory
        if (S_ISDIR(sb.st_mode) != 0) {
            if ((res = dfd_rmr(dfd_openat(dirfd, dentry->d_name))) < 0) {
                return -1;
            }
            if (unlinkat(dirfd, dentry->d_name, AT_REMOVEDIR) < 0) {
                dprintf(2, "Unable to remove directory '%s': %s", dentry->d_name, strerror(errno));
                return -1;
            }
            continue;
        }

        // remove a file object
        if (unlinkat(dirfd, dentry->d_name, 0) < 0) {
            dprintf(2, "Unable to remove file '%s': %s", dentry->d_name, strerror(errno));
            return -1;
        }
    }

    closedir(dirptr);
    return 0;
}


// removes chatdir usinr dfd_rmr()
int
rmr_chatdir(const char * dir)
{
    if (dfd_rmr(dfd_opendir(dir)) < 0) {
        dprintf(2, "Unable to remove contents of chatdir '%s': %s", dir, strerror(errno));
        return -1;
    }
    if (rmdir(dir) < 0) {
        dprintf(2, "Unable to remove chatdir '%s': %s", dir, strerror(errno));
        return -1;
    }
    return 0;
}


// builds time string for message timestamping.
static void
get_timestr (char *dt)
{
    struct tm tm = {0};
    time_t t = 0;

    t = time(NULL);
    gmtime_r(&t, &tm); // UTC

    strftime(dt, MAX_TIME_STR_LEN, TIME_STR_FORMAT, &tm);
}


// prints buffer to the screen while playing nice with readline
static void
print_buffer (char *buffer)
{

    char *saved_line = NULL;
    int saved_point = 0;

    /* this is readline stuff.
     *  - save the cursor position
     *  - save the current line contents
     *  - set the line to blank
     *  - tell readline we're done mucking
     *  - print the message
     *  - restore the standard prompt
     *  - restore the line contents
     *  - restore the cursor position
     *  - tell readline we're done mucking (again)
     */
    saved_point = rl_point;
    saved_line = rl_copy_text(0, rl_end);
    rl_set_prompt("");
    rl_clear_message();
    rl_replace_line("", 0);
    rl_redisplay();
    dprintf(1, "%s", buffer);
    rl_set_prompt(PROMPT);
    rl_replace_line(saved_line, 0);
    rl_point = saved_point;
    rl_redisplay();
    free(saved_line);
}


// prints raw string into the chatlog
static void
writechat_raw (const char *string)
{
    fd_write(fds.fd_chatlog, (void *) string, strlen(string));
    fsync(fds.fd_chatlog);
}


// emits "status" into the chatlog, i.e. user joined/left
static void
writechat_status (const char *status, size_t notify, size_t echo)
{
    char time[MAX_TIME_STR_LEN] = {0};
    char status_info[MAX_INFO_LINE_LEN] = {0};

    if (fds.fd_chatlog > -1) {
        get_timestr(time);
        snprintf(status_info, sizeof(status_info), "[%s][%s] *** <%s> %s ***\n", pidstr, time, nickstr, status);
        fd_write(fds.fd_chatlog, status_info, strlen(status_info));
        fsync(fds.fd_chatlog);

        if (notify) {
            notify_new_message(event_fifodir);
        }

        if (echo) {
            print_buffer(status_info);
        }
    }
}


/* "unregisters" from notifications
 * - by closing notify pipe
 */
void
notify_unregister_pipe(void)
{
    char notify_name[MAX_NOTIFY_NAME_LEN] = {0};
    int ret = -1;

    if ((ret = snprintf(notify_name, sizeof(notify_name), "event/%s", pidstr)) < 0 || ret > sizeof(notify_name)) {
        dprintf(2, "warning: Notify event listener name too long for '%s/event' or error occured: %s", chatdirstr, strerror(errno));
    } else if (fds.fd_chatdir != -1 && unlinkat(fds.fd_chatdir, notify_name, 0) < 0 && errno != ENOENT) {
        dprintf(2, "warning: Unable to unregister notify event listener '%d:%s' at '%s': %s", fds.fd_chatdir, notify_name, chatdirstr, strerror(errno));
    }
}


/* sends "event" to listeners in fifodir
 * - "event" is single byte message
 */
int
notify_fifodir (DIR * eventdirptr, char * event, size_t notify_self)
{
    struct dirent * dentry = NULL;
    int dfd = -1;

    rewinddir(eventdirptr);
    dfd = dirfd(eventdirptr);

    if (dfd < 0) return -1;

    while ((dentry = readdir(eventdirptr)) != NULL)	{
        if (dentry->d_type == DT_FIFO) {
            if (strstr(dentry->d_name, pidstr) && notify_self) {
                fd_spitat(dfd, dentry->d_name, event, 1);
            } else {
                fd_spitat(dfd, dentry->d_name, event, 1);
            }
        }
    }

    return 0;
}


/* writes newline to other listeners in fifodir
 * - to indicate to them they should reread chatlog
 */
int
notify_new_message (DIR * eventdirptr)
{
    return notify_fifodir(eventdirptr, "\n", 1);
}


/* writes 'W' to other listeners in fifodir
 * - to indicate to them they should emit their nick/pid pairs
 */
int
notify_list (DIR * eventdirptr)
{
    int ret = -1;

    char list_query[MAX_INFO_LINE_LEN] = {0};
    char time[MAX_TIME_STR_LEN] = {0};
    get_timestr(time);

    if ((ret = snprintf(list_query, sizeof(list_query), "[%s][%s] <%s> /list\n", pidstr, time, nickstr)) > 0 && ret <= sizeof(list_query)) {
        writechat_raw(list_query);
    }

    return notify_fifodir(eventdirptr, "L", 1);
}


/* writes 'w' to specific listener in fifodir
 * - to indicate to it it should emit it's username
 */
int
notify_whois (DIR * eventdirptr, pid_t pid)
{
    char userpid[MAX_NOTIFY_NAME_LEN] = {0};
    int dfd = -1, ret = -1;

    dfd = dirfd(eventdirptr);

    if (dfd < 0) return -1;

    if ((ret = snprintf(userpid, sizeof(userpid), "%d", pid)) < 0 || ret > sizeof(userpid)) {
        return -1;
    }

    {
        char whois_query[MAX_INFO_LINE_LEN] = {0};
        char time[MAX_TIME_STR_LEN] = {0};
        get_timestr(time);
        if ((ret = snprintf(whois_query, sizeof(whois_query), "[%s][%s] <%s> /whois %s ?\n", pidstr, time, nickstr, userpid)) > 0 && ret <= sizeof(whois_query)) {
            writechat_raw(whois_query);
            ret = fd_spitat(dfd, userpid, "w", 1);
        }
    }

    return ret;
}


/* writes 'p' to specific listener in fifodir
 * - to indicate to it it should emit it's pid
 */
int
notify_pty (DIR * eventdirptr, pid_t pid)
{
    char userpid[MAX_NOTIFY_NAME_LEN] = {0};
    int dfd = -1, ret = -1;

    dfd = dirfd(eventdirptr);

    if (dfd < 0) return -1;

    if ((ret = snprintf(userpid, sizeof(userpid), "%d", pid)) < 0 || ret > sizeof(userpid)) {
        return -1;
    }

    {
        char whois_query[MAX_INFO_LINE_LEN] = {0};
        char time[MAX_TIME_STR_LEN] = {0};
        get_timestr(time);
        if ((ret = snprintf(whois_query, sizeof(whois_query), "[%s][%s] <%s> /ptyof %s\n", pidstr, time, nickstr, userpid)) > 0 && ret <= sizeof(whois_query)) {
            writechat_raw(whois_query);
            ret = fd_spitat(dfd, userpid, "p", 1);
        }
    }

    return ret;
}


/* writes 'D' to other listeners in fifodir
 * - to indicate to them that chatroom was "destroyed"
 */
int
notify_destroy (DIR * eventdirptr)
{
    char destroy_info[MAX_INFO_LINE_LEN] = {0};
    char time[MAX_TIME_STR_LEN] = {0};

    get_timestr(time);

    if (snprintf(destroy_info, sizeof(destroy_info), "[%s][%s] <%s> /destroy %s\n", pidstr, time, nickstr, chatdirstr) > 0) {
        writechat_raw(destroy_info);
        notify_fifodir(eventdirptr, "\n", 0);
    }
    return notify_fifodir(eventdirptr, "D", 1);
}


// dispatches input line obtained from readline
static void
dispatch_input_line (char *line)
{
    char lmsg[MAX_CHAT_READ_BUFFER_LEN] = {0};

    // we care only if we're called on a real input
    if (line) {

        // readline keeps history, let's make use of it
        add_history(line);

        /* process commands:
         *  - quit if we are told to quit
         *  - list participants if we are told to list them
         *  - ... etc
         *  - or write the message, if there's something to say
         */
        if(strncmp(line, "/help", 5) == 0 || strncmp(line, "/h", 2) == 0 || strncmp(line, "/?", 2) == 0 )  {
            print_buffer("commands:\n");
            print_buffer("  /help, /h, /?        - print this help\n");
            print_buffer("  /quit, /q            - quit\n");
            print_buffer("  /list, /l            - list active connected users\n");
            print_buffer("  /whois $pid, /w $pid - try to identify connection by $pid\n");
            print_buffer("  /ptyof $pid, /p $pid - try to identify terminal line by $pid\n");
            print_buffer("  /destroy             - disconnect all users and destroy chatroom\n");
            //print_buffer("  /save $logfile       - save copy of chatlog as file named $logfile\n");
        } else if(strncmp(line, "/quit", 5) == 0 || strncmp(line, "/q", 2) == 0)  {
            run = NO;
        } else if(strncmp(line, "/list", 7) == 0 || strncmp(line, "/l", 2) == 0)  {
            notify_list(event_fifodir);
        } else if(strncmp(line, "/whois", 4) == 0 || strncmp(line, "/w", 2) == 0)  {
            if ((line = strstr(line, " "))) {
                pid_t pid = atoi(line);
                if (pid) {
                    notify_whois(event_fifodir, pid);
                } else {
                    snprintf(lmsg, MAX_CHAT_READ_BUFFER_LEN, "Invalid pid!\n");
                    print_buffer(lmsg);
                }
            } else {
                snprintf(lmsg, MAX_CHAT_READ_BUFFER_LEN, "Invalid pid!\n");
                print_buffer(lmsg);
            }
        } else if(strncmp(line, "/pty", 4) == 0 || strncmp(line, "/p", 2) == 0)  {
            if ((line = strstr(line, " "))) {
                pid_t pid = atoi(line);
                if (pid) {
                    notify_pty(event_fifodir, pid);
                } else {
                    snprintf(lmsg, MAX_CHAT_READ_BUFFER_LEN, "Invalid pid!\n");
                    print_buffer(lmsg);
                }
            } else {
                snprintf(lmsg, MAX_CHAT_READ_BUFFER_LEN, "Invalid pid!\n");
                print_buffer(lmsg);
            }
        } else if(strncmp(line, "/destroy", 8) == 0)  {
            notify_destroy(event_fifodir);
            usleep(200000);
            rmr_chatdir(chatdirstr);
        } else if(strncmp(line, "/save", 5) == 0)  {

        } else if(strncmp(line, "/", 1) == 0) {
            snprintf(lmsg, MAX_CHAT_READ_BUFFER_LEN, "Unknown command: %s\n", line + 1);
            print_buffer(lmsg);
        } else if(strlen(line) > 0) {
            send_message(line);
        }
    }
}


// returns list of possible completions to readline
char *
rlcb_commands_generator(const char *text, int state)
{
    static int list_index, len;
    char *name;

    if (!state) {
        list_index = 0;
        len = strlen(text);
    }

    while ((name = pipechat_commands[list_index++])) {
        if (strncmp(name, text, len) == 0) {
            return strdup(name);
        }
    }

    return NULL;
}


// handles readline completion
char * *
rlcb_commands_completion(const char *text, int start, int end)
{
    rl_attempted_completion_over = 1;
    return rl_completion_matches(text, rlcb_commands_generator);
}


// handles readline input line
static void
rlcb_handle_line (char *line)
{
    /* we want to ignore when readline says it has the end of a message,
     * as we'll only care if the user presses ENTER.
     * if line buffer is empty, it means user sent EOF,
     * and wants to quit.
     */
    if (line != NULL) {
        rl_set_prompt(PROMPT);
        rl_already_prompted = 1;
    } else {
        run = NO;
    }
}


// handles readline enter keypress
static int
rlcb_handle_enter (int x, int y)
{
    char *line = NULL;

    /* handle when a user presses enter.
     *  - save the contents of the line.
     *  - set the prompt to nothing.
     *  - blank the line.
     *  - pass the message to the message handler
     *  - rl_copy_text returns malloc'd mem, so free it
     *  - restore the prompt
     *  - tell readline we're done mucking
     */
    line = rl_copy_text(0, rl_end);
    rl_set_prompt("");
    rl_replace_line("", 1);
    rl_redisplay();

    dispatch_input_line(line);

    free(line);

    rl_set_prompt(PROMPT);
    rl_redisplay();

    rl_done = 1;
    return 0;
}


/* reads all of the messages from the chatlog since the last read
 * and prints them
 */
static void
process_messages (void)
{
    char buffer[MAX_CHAT_READ_BUFFER_LEN] = {0};
    ssize_t read = -1;

    read = fd_pread(fds.fd_chatlog, buffer, MAX_CHAT_READ_BUFFER_LEN, last_chatlog_read_pos);

    // if we failed to read from chatlog something went really wrong
    if (read == -1) {
        if (errno != EPIPE) {
            run = NO;
            dprintf(2, "chatlog read failed with errno %d: %s\n", errno, strerror(errno));
        }
        return;
    }

    if (read)  {
        print_buffer(buffer);
    }

    // update the last read position
    last_chatlog_read_pos = lseek(fds.fd_chatlog, last_chatlog_read_pos + read, SEEK_SET);

    // set the file position at the end of the file
    //lseek(fds.fd_chatlog, 0, SEEK_END);
}


// handles specific event pipe notifcation events
static check_result
process_event (char * event)
{
    if (event[0] == '\n') {
        return CHECK_MESSAGE;
    } else if (event[0] == 'L' || event[0] == 'w') {
        int ret = -1;
        char list_ident[MAX_INFO_LINE_LEN] = {0};
        if ((ret = snprintf(list_ident, sizeof(list_ident), "[%s] is <%s>\n", pidstr, nickstr)) > 0 && ret <= sizeof(list_ident)) {
            writechat_raw(list_ident);
            notify_new_message(event_fifodir);
        }
    } else if (event[0] == 'p') {
        int ret = -1;
        char pty_ident[MAX_INFO_LINE_LEN] = {0};
        if ((ret = snprintf(pty_ident, sizeof(pty_ident), "[%s] is <%s> on fds[ 0='%s', 1='%s' ]\n", pidstr, nickstr, ttyname(0), ttyname(1))) > 0 && ret <= sizeof(pty_ident)) {
            writechat_raw(pty_ident);
            notify_new_message(event_fifodir);
        }
    } else if (event[0] == 'D') {
        char time[MAX_TIME_STR_LEN] = {0};
        get_timestr(time);
        rl_set_prompt("");
        rl_clear_message();
        rl_redisplay();
        dprintf(1, "[%s] *** chatroom '%s' destroyed...\n", time, chatdirstr);
        run = NO;
        log_leaving_message = NO;
    }
    return CHECK_NOTHING;
}


/* this is a "message" emitter, i.e. it "sends" a chat message from one user to another.
 *  - it first writes into chatlog file
 *  - then it notifies other users registered through event fifodir
 *    by writing newline into their "notify" pipes
 */
static void
send_message (const char *message)
{
    char time[MAX_TIME_STR_LEN] = {0};
    get_timestr(time);
    dprintf(fds.fd_chatlog, "[%s][%s] <%s>: %s\n", pidstr, time, nickstr, message);
    fsync(fds.fd_chatlog);
    notify_new_message(event_fifodir);
}


/* tiny eventloop "core" based on (p)poll(), it either:
 * - handles signals
 * - notification events
 * - messages
 * - or nothing
 */
static check_result
check_events ()
{
    struct pollfd pfd[3] = {0};
    char event[1] = {0};
    int changed = 0;

    pfd[0].fd = fds.fd_selfpipe; // selfpipe
    pfd[0].events = POLLIN;
    pfd[1].fd = fds.fd_event;    // event notifications
    pfd[1].events = POLLIN;
    pfd[2].fd = 0;               // user input
    pfd[2].events = POLLIN;

    do {
        changed = ppoll(pfd, 3, NULL, NULL);

        if (changed < 0 && errno != EINTR) {
            return CHECK_ERROR;
        } else if (changed == 0) {
            return CHECK_NOTHING;
        } else {
            if ((pfd[0].revents & POLLIN) == POLLIN) {

                return CHECK_SIGNAL;

            } else if ((pfd[1].revents & POLLIN) == POLLIN) {

                if (fd_read(pfd[1].fd, event, 1) == 1) {
                    return process_event(event);
                }
                return CHECK_NOTHING;

            } else if ((pfd[2].revents & POLLIN) == POLLIN) {

                return CHECK_INPUT;
            }
        }
    } while ((changed == -1) && errno == EINTR);

    return CHECK_ERROR;
}


static void
check_signal(void)
{
    // TODO: implement selfpipe signal handling
}


static void
main_usage(char * progname)
{
    dprintf(1, "%s v%s - a small fifodir based chat system for multiple users\n\n", progname, VERSION);
    dprintf(1, "Usage: %s [OPTIONS] chatdir [groupname]\n\n", progname);
    dprintf(1, "OPTIONS\n");
    dprintf(1, " -h     this help\n");
    dprintf(1, "\n");
}


int
main (int argc, char *argv[])
{
    int ret = -1;

    if (!isatty(0) || !isatty(1)) {
        dprintf(2, "This program must be run on real terminal.");
        exit(1);
    }

    /* we always require a chatdir, and we guess a nick
     * from process state and environment.
     *
     * user can specify arbitrary user group as well,
     * so that multiple users can chat together,
     * otherwise default user group will be used.
     * - beware this default might not be what user wants
     * - on most unix systems default group is user's
     *   own group which will prevent other user's from
     *   in different group from "connecting"
     */
    if (argc < 2) {
        main_usage(argv[0]);
        return 1;
    }

    for (int argi = 1; argi < argc; argi++) {
        if (argv[argi][0] == '-') {
            if (argv[argi][1] == 'h') {
                main_usage(argv[0]);
                return 0;
            } else if (argv[argi][1] == '-') {
                argv += argi;
                argc -= argi;
                break;
            }
            dprintf(2, "Unknown option: %s\n", argv[argi]);
            main_usage(argv[0]);
            exit(1);
        } else {
            break;
        }
    }

    //  get chatdir name
    if (argv[1] == NULL) {
        dprintf(2, "Can't determine chatdir. Specify chatdir on the command line.\n");
        exit(1);
    } else {
        chatdirstr = argv[1];
    }

    //  get user name
    {
        /* we get username from following sources in this order:
         *   - SUDO_USER envvar
         *   - LOGNAME envvar
         *   - USER envvar
         *   - process EUID
         *   - process RUID
         */
        nickstr = getenv("SUDO_USER");
        if (nickstr == NULL) {
            nickstr = getenv("LOGNAME");
            if (nickstr == NULL) {
                nickstr = getenv("USER");
                if (nickstr == NULL) {
                    uid_t uid = geteuid();
                    if (uid == 0) {
                        uid = getuid();
                    }
                    struct passwd * pwd = getpwuid(uid) ;
                    if (pwd == NULL) {
                        dprintf(2, "Can't determine username. Fix your system: %s\n", strerror(errno));
                        exit(1);
                    }
                    nickstr = pwd->pw_name;
                    if (nickstr == NULL) {
                        nickstr = calloc(MAX_NOTIFY_NAME_LEN, 1);
                        if (nickstr) {
                            if ((ret = snprintf(nickstr, MAX_NOTIFY_NAME_LEN, "%d", uid)) < 0 || ret > sizeof(MAX_NOTIFY_NAME_LEN)) {
                                dprintf(2, "Failed to generate username: %s\n", strerror(errno));
                                exit(1);
                            }
                        } else {
                            dprintf(2, "Failed to generate username: %s\n", strerror(errno));
                            exit(1);
                        }
                    }
                }
            }
        }
    }

    // get user group
    if (argc > 2) {

        if (argv[2] == NULL) {
            dprintf(2, "Can't determine user group. Specify proper user group.\n");
            exit(1);
        }

        groupstr = argv[2];

        {
            struct group * grp;

            errno = 0; // px specifies we need to reset errno, if we want to be able to detect all failures
            grp = getgrnam(groupstr);

            if (grp == NULL) {
                dprintf(2, "User group '%s' not found in system's group database: %s\n", groupstr, strerror(errno));
                exit(1);
            }

            egid = grp->gr_gid;

            if (getegid() != 0 && egid == 0) {
                dprintf(2, "warning: user group '%s' is superuser group, this is potentially unsafe!\n", groupstr);
            }
        }
    } else {
        egid = getegid();
    }

    /* convert PID to string for further use
     *  - PID of process should never change so it's okay to "cache" it
     */
    if ((ret = snprintf(pidstr, sizeof(pidstr_buf), "%d", getpid())) < 0 || ret > sizeof(pidstr_buf)) {
        dprintf(2, "Can't convert pid to string %d %d %ld\n", ret, getpid(), sizeof(pidstr_buf));
        exit(1);
    }

    // construct chat prompt
    if ((ret = snprintf(promptstr, sizeof(promptstr_buf), "[%s]<%s>: ", pidstr, nickstr)) < 0 || ret > sizeof(promptstr_buf)) {
        dprintf(2, "Can't create prompt string\n");
        exit(1);
    }

    // TODO: implement selfpipe
    fds.fd_selfpipe = -1;

    /* "bind" to chatdir
     * - check whether it exists
     * - if it does not, create it
     * - then fix chatdir perms if necessary
     * - and finally "bind" to it by holding onto it's fd
     */
    {
        struct stat sb;
        if (stat(chatdirstr, &sb) < 0 && errno != ENOENT) {
            dprintf(2, "Unable to stat chatdir '%s': %s\n", chatdirstr, strerror(errno));
            exit(1);
        } else if (errno == ENOENT) {
            if (mkdir(chatdirstr, S_IRUSR|S_IWUSR|S_IXUSR|S_IRGRP|S_IWGRP|S_IXGRP) < 0) {
                dprintf(2, "Unable to create chatdir '%s': %s\n", chatdirstr, strerror(errno));
                exit(1);
            }
        } else if (! S_ISDIR(sb.st_mode)) {
            dprintf(2, "Chatdir '%s' is not directory.\n", chatdirstr);
            exit(1);
        }
        if (groupstr && chown(chatdirstr, geteuid(), egid)) {
            dprintf(2, "Unable to change group ownership of chatdir '%s' to '%s': %s\n", chatdirstr, groupstr, strerror(errno));
            exit(1);
        }
        if (groupstr && chmod(chatdirstr, S_IRUSR|S_IWUSR|S_IXUSR|S_IRGRP|S_IWGRP|S_IXGRP) < 0) {
            dprintf(2, "Unable to change permissions on chatdir '%s': %s\n", chatdirstr, strerror(errno));
            exit(1);
        }
        if ((fds.fd_chatdir = dfd_opendir(chatdirstr)) < 0 ) {
            dprintf(2, "Unable to open chatdir '%s': %s\n", chatdirstr, strerror(errno));
            exit(1);
        }
    }

    /* "bind" to chatlog
     * - try opening chatlog anchored at chatdir directory
     * - fix chatlog perms if necessary
     */
    {
        if ((fds.fd_chatlog = openat(fds.fd_chatdir, "log", O_RDWR | O_APPEND | O_CREAT | O_NONBLOCK | O_NOFOLLOW, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP)) < 0) {
            dprintf(2, "Unable to open chatlog file 'log' in '%s': %s\n", chatdirstr, strerror(errno));
            exit(1);
        }
        if (groupstr && fchown(fds.fd_chatlog, geteuid(), egid) < 0) {
            dprintf(2, "Unable to change group ownership of chatlog file '%s/log': %s\n", chatdirstr, strerror(errno));
            exit(1);
        }
        if (groupstr && fchmod(fds.fd_chatlog, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP) < 0) {
            dprintf(2, "Unable to set permissions on chatlog file '%s/log': %s\n", chatdirstr, strerror(errno));
            exit(1);
        }
    }

    /* bind to "event" fifodir
     * - check whether it exists
     * - if it does not, create it
     * - if asked, fix perms
     */
    {
        struct stat sb = {0};
        if (fstatat(fds.fd_chatdir, "event", &sb, 0) < 0 && errno != ENOENT) {
            dprintf(2, "Unable to stat eventdir 'event' at '%s': %s\n", chatdirstr, strerror(errno));
            exit(1);
        } else if (errno == ENOENT) {
            if (mkdirat(fds.fd_chatdir, "event", S_IRUSR|S_IWUSR|S_IXUSR|S_IRGRP|S_IWGRP|S_IXGRP) < 0) {
                dprintf(2, "Unable to create eventdir 'event' at '%s': %s\n", chatdirstr, strerror(errno));
                exit(1);
            }
            if (fstatat(fds.fd_chatdir, "event", &sb, 0) < 0) {
                dprintf(2, "Unable to stat eventdir 'event' at '%s': %s\n", chatdirstr, strerror(errno));
                exit(1);
            }
        } else if (! S_ISDIR(sb.st_mode)) {
            dprintf(2, "eventdir 'event' at '%s' is not directory.\n", chatdirstr);
            exit(1);
        }
        /* critical: we need to properly handle gorup permissions on eventdir */
        if (groupstr) {
           if (fchownat(fds.fd_chatdir, "event", geteuid(), egid, 0) < 0) {
                dprintf(2, "Unable to change group ownership of eventdir '%s/event' to '%s': %s\n", chatdirstr, groupstr, strerror(errno));
                exit(1);
            }
        }  else {
            egid = sb.st_gid;
        }
        if (groupstr && fchmodat(fds.fd_chatdir, "event", S_IRUSR|S_IWUSR|S_IXUSR|S_IRGRP|S_IWGRP|S_IXGRP, 0) < 0) {
            dprintf(2, "Unable to change permissions on eventdir '%s/event': %s\n", chatdirstr, strerror(errno));
            exit(1);
        }
    }

    /* register for pipe in event fifodir for events notification
     * - ensure proper perms
     */
    {
        int event_dfd = -1;
        if ((event_dfd = dfd_openat(fds.fd_chatdir, "event")) < 0) {
            dprintf(2, "Unable to open eventdir 'event' in '%s': %s\n", chatdirstr, strerror(errno));
            exit(1);
        } else {
            char notify_name[30] = {0};
            ret = -1;
            if ((ret = snprintf(notify_name, sizeof(notify_name), "%s", pidstr)) < 0 || ret > sizeof(notify_name)) {
                dprintf(2, "Notify event listener name too long for '%s/event' or error occured: %s\n", chatdirstr, strerror(errno));
                exit(1);
            }
            if (mkfifoat(event_dfd, notify_name, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP) < 0) {
                dprintf(2, "Unable to register notify event listener '%s' at '%s/event': %s\n", notify_name, chatdirstr, strerror(errno));
                exit(1);
            }
            if ((fds.fd_event = openat(event_dfd, notify_name, O_RDWR | O_NONBLOCK | O_NOFOLLOW)) < 0) {
                // add event unlink
                dprintf(2, "Unable to open notify event listener '%s' at '%s/event': %s\n", notify_name, chatdirstr, strerror(errno));
                exit(1);
            }
            if (groupstr) {
                if (getegid() != 0 && egid == 0) {
                    dprintf(2, "warning: user group '%s' is superuser group, this is potentially unsafe!\n", groupstr);
                }
            } else {
                if (getegid() != 0 && egid == 0) {
                    dprintf(2, "warning: user group '%d' is superuser group, this is potentially unsafe!\n", egid);
                }
            }

            atexit(notify_unregister_pipe);

            if (fchown(fds.fd_event, geteuid(), egid) < 0) {
                dprintf(2, "Unable to change group ownership of listener '%s' at '%s/event': %s %d\n", notify_name, chatdirstr, strerror(errno), egid);
                exit(1);
            }
            if (fchmod(fds.fd_event, S_IRUSR|S_IWUSR|S_IWGRP) < 0) {
                dprintf(2, "Unable to set permissions on event listener '%s' at '%s/event': %s\n", notify_name, chatdirstr, strerror(errno));
                exit(1);
            }
            if ((event_fifodir = fdopendir(event_dfd)) == NULL) {
                dprintf(2, "Unable to open notify event listener '%s' at '%s/event': %s\n", notify_name, chatdirstr, strerror(errno));
                exit(1);
            }
        }
    }

    // by default, we don't want to see messages from the past, as they could be loooooong
    lseek(fds.fd_chatlog, 0, SEEK_END);

    // so this is now our "last read" position
    last_chatlog_read_pos = lseek(fds.fd_chatlog, 0, SEEK_CUR);

    /* we register handlers with readline to let us know when the user hits enter
     * and bind the compeltion key.
     */
    rl_bind_key(RETURN, rlcb_handle_enter);
    rl_bind_key(TAB, rl_complete);


    /* we setup the "fake" handler for when readline
     * thinks user is done editing line
     */
    rl_callback_handler_install(PROMPT, rlcb_handle_line);

    /* we register completion function
     */
    rl_attempted_completion_function = rlcb_commands_completion;

    /* finally done with chatdir setup, chatdir binding and readline!
     * - now we can let everyone know that the user has arrived.
     */
    writechat_status("joined", 1, 0);

    /* until we decide to quit, we run the program's eventloop core
     *  - on new message notification we read and display chatlog
     *  - on user input we tell readline to grab input character
     *  - on signal we handle that
     * if run statevar becomes false we exit
     */
    while(run) {

        switch(check_events()) {

            case CHECK_NOTHING :
            case CHECK_TIMEOUT : {
                ;
            } break;

            case CHECK_ERROR : {
                // TODO: implement error handling
                ;
            } break;

            case CHECK_SIGNAL : {
                check_signal();
            } break;

            case CHECK_MESSAGE : {
                process_messages();
            } break;

            case CHECK_INPUT : {
                rl_callback_read_char();
            } break;
        }
    }

    /* we're quitting now.
     * - we say goodbye, echoing it for our current user too,
     *   as there won't be another eventloop iter to get notified
     *   and display it
     * - however if this is not wanted (like on room destruction)
     *   we skip it completely
     */
    if (log_leaving_message) writechat_status("left", 1, 1);

    // clean up readline now state
    rl_unbind_key(RETURN);
    rl_unbind_key(TAB);
    rl_callback_handler_remove();

    /* being a good citizen, we close chatlog file handle,
     * but other closures will be handled by atexit() handler
     * registered above and by kernel itself
     */
    fd_close(fds.fd_chatlog);

    // finally we clean up the screen.
    rl_set_prompt("");
    rl_clear_message();
    rl_redisplay();

    return 0;
}
