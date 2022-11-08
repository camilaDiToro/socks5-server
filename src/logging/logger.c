#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>

#include "../selector.h"
#include "logger.h"
#include "util.h"

#define DEFAULT_LOG_FOLDER "./log"
#define DEFAULT_LOG_FILE (DEFAULT_LOG_FOLDER "/%02d-%02d-%04d.log")
#define DEFAULT_LOG_FILE_MAXSTRLEN 31

/** The minimum allowed length for the log writing buffer. */
#define LOG_MIN_BUFFER_SIZE 0x1000 // 4 KBs
/** The maximum allowed length for the log writing buffer. */
#define LOG_MAX_BUFFER_SIZE 0x400000 // 4 MBs
/** The amount of bytes to expand the log buffer by when expanding. */
#define LOG_BUFFER_SIZE_GRANULARITY 0x1000 // 4 KBs
/** The maximum length a single print into the log buffer SHOULD require. */
#define LOG_BUFFER_MAX_PRINT_LENGTH 0x200 // 512 bytes

#define LOG_FILE_PERMISSION_BITS 666
#define LOG_FOLDER_PERMISSION_BITS 666
#define LOG_FILE_OPEN_FLAGS (O_WRONLY | O_APPEND | O_CREAT | O_NONBLOCK)

#define LOG_LINE_START "[%02d/%02d/%04d %02d:%02d:%02d] "
#define LOG_PRINTF_START_PARAMS tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec

#define ADDRSTR_BUFLEN 64

/** The buffer where logs are buffered. */
static char* buffer = NULL;
static size_t bufferStart = 0, bufferLength = 0, bufferCapacity = 0;

/** The file descriptor for writing logs to disk, or -1 if we're not doing that. */
static int logFileFd = -1;
static TSelector selector = NULL;

/** The stream for writing logs to, or NULL if we're not doing that. */
static FILE* logStream = NULL;

static void makeBufferSpace(size_t len) {
    // Make enough space in the buffer for the string
    if (bufferLength + bufferStart + len > bufferCapacity) {
        // If the buffer can be compacted to fit this string, do so. Otherwise,
        // we'll have to allocate more memory.
        if (bufferCapacity <= len) {
            memmove(buffer, buffer + bufferStart, bufferLength);
            bufferStart = 0;
        } else if (bufferCapacity < LOG_MAX_BUFFER_SIZE) {
            size_t newBufferCapacity = bufferLength + len;
            newBufferCapacity = (newBufferCapacity + LOG_BUFFER_SIZE_GRANULARITY - 1) / LOG_BUFFER_SIZE_GRANULARITY * LOG_BUFFER_SIZE_GRANULARITY;
            if (newBufferCapacity > LOG_MAX_BUFFER_SIZE)
                newBufferCapacity = LOG_MAX_BUFFER_SIZE;

            // The buffer isn't large enough, let's try to expand it, or at
            // least compact it to make as much space available as possible.
            void* newBuffer = malloc(newBufferCapacity);
            if (newBuffer == NULL) {
                memmove(buffer, buffer + bufferStart, bufferLength);
                bufferStart = 0;
            } else {
                memcpy(newBuffer, buffer + bufferStart, bufferLength);
                free(buffer);
                buffer = newBuffer;
                bufferCapacity = newBufferCapacity;
                bufferStart = 0;
            }
        }
    }
}

static inline void tryFlushBufferToFile() {
    // Try to write everything we have in the buffer. This is nonblocking, so any
    // (or all) remaining bytes will be saved in the buffer and retried later.
    ssize_t written = write(logFileFd, buffer + bufferStart, bufferLength);
    if (written > 0) {
        bufferLength -= written;
        bufferStart = (bufferLength == 0 ? 0 : (bufferStart + written));
    }

    // If there are still remaining bytes to write, leave them in the buffer and retry
    // once the selector says the fd can be written.
    selector_set_interest(selector, logFileFd, bufferLength > 0 ? OP_WRITE : OP_NOOP);
}

#define LOG_PREPRINTPARAMS_MACRO(format)                             \
    {                                                                \
        makeBufferSpace(LOG_BUFFER_MAX_PRINT_LENGTH);                \
        time_t T = time(NULL);                                       \
        struct tm tm = *localtime(&T);                               \
        size_t maxlen = bufferCapacity - bufferLength - bufferStart; \
        int written = snprintf(buffer + bufferStart + bufferLength, maxlen, LOG_LINE_START format "\n", LOG_PRINTF_START_PARAMS,

#define LOG_POSTPRINTPARAMS_MACRO );      \
    return postLogPrint(written, maxlen); \
    }

#define LOG_PRINTF1(format, param1) LOG_PREPRINTPARAMS_MACRO(format) param1 LOG_POSTPRINTPARAMS_MACRO
#define LOG_PRINTF2(format, param1, param2) LOG_PREPRINTPARAMS_MACRO(format) param1, param2 LOG_POSTPRINTPARAMS_MACRO
#define LOG_PRINTF3(format, param1, param2, param3) LOG_PREPRINTPARAMS_MACRO(format) param1, param2, param3 LOG_POSTPRINTPARAMS_MACRO
#define LOG_PRINTF4(format, param1, param2, param3, param4) LOG_PREPRINTPARAMS_MACRO(format) param1, param2, param3, param4 LOG_POSTPRINTPARAMS_MACRO
#define LOG_PRINTF5(format, param1, param2, param3, param4, param5) LOG_PREPRINTPARAMS_MACRO(format) param1, param2, param3, param4, param5 LOG_POSTPRINTPARAMS_MACRO

static int postLogPrint(int written, size_t maxlen) {
    if (written < 0) {
        fprintf(stderr, "Error: snprintf(): %s\n", strerror(errno));
        return -1;
    }

    if (written >= maxlen) {
        fprintf(stderr, "Error: %ld bytes of logs possibly lost. Slow disk?\n", written - maxlen + 1);
        written = maxlen - 1;
    }

    if (logStream != NULL) {
        fprintf(logStream, "%s", buffer + bufferStart + bufferLength);
    }

    if (logFileFd >= 0) {
        bufferLength += written;
        tryFlushBufferToFile();
    }
    return 0;
}

static void fdWriteHandler(TSelectorKey* key) {
    tryFlushBufferToFile();
}

static void fdCloseHandler(TSelectorKey* key) {
    // We will attempt to flush the remaining bytes to the log file and then close it.

    if (bufferLength != 0) {
        // Set the log file to blocking, then try to write the remaining bytes. If any of
        // this fails, just ignore the failure.
        int flags = fcntl(logFileFd, F_GETFD, 0);
        int pepe = fcntl(logFileFd, F_SETFL, flags & (~O_NONBLOCK));
        fprintf(stderr, "Pedro y Pepe: %d y %d\n", flags, pepe); // TODO: remove lol
        ssize_t written = write(logFileFd, buffer, bufferLength);
        if (written > 0) {
            bufferLength -= written;
            bufferStart = (bufferLength == 0 ? 0 : (bufferStart + written));
        }
    }

    close(logFileFd);
    logFileFd = -1;
}

static TFdHandler fdHandler = {
    .handle_read = NULL,
    .handle_write = fdWriteHandler,
    .handle_close = fdCloseHandler,
    .handle_block = NULL};

/** Attempts to open a file for logging. Returns the fd, or -1 if failed. */
static int tryOpenLogfile(const char* logFile, struct tm tm) {
    if (logFile == NULL)
        return -1;

    char logfilebuf[DEFAULT_LOG_FILE_MAXSTRLEN + 1];

    if (logFile[0] == '\0') {
        snprintf(logfilebuf, DEFAULT_LOG_FILE_MAXSTRLEN, DEFAULT_LOG_FILE, tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900);
        logFile = logfilebuf;

        // If the default log folder isn't created, create it.
        mkdir(DEFAULT_LOG_FOLDER, LOG_FOLDER_PERMISSION_BITS);
    }

    int fd = open(logFile, LOG_FILE_OPEN_FLAGS, LOG_FILE_PERMISSION_BITS);
    if (fd < 0) {
        fprintf(stderr, "WARNING: Failed to open logging file at %s. The server will still run, but with logging disabled.", logFile);
        return -1;
    }

    return fd;
}

int logInit(TSelector selectorParam, const char* logFile, FILE* logStreamParam) {
    time_t T = time(NULL);
    struct tm tm = *localtime(&T);

    selector = selectorParam;
    logFileFd = selectorParam == NULL ? -1 : tryOpenLogfile(logFile, tm);
    logStream = logStreamParam;

    if (logFileFd >= 0) {
        selector_register(selector, logFileFd, &fdHandler, OP_NOOP, NULL);
    }

    if (logFileFd >= 0 || logStream != NULL) {
        buffer = malloc(LOG_MIN_BUFFER_SIZE);
        bufferCapacity = LOG_MIN_BUFFER_SIZE;
        bufferLength = 0;
        bufferStart = 0;
        if (buffer == NULL) {
            close(logFileFd);
            logFileFd = -1;
            fprintf(stderr, "WARNING: Failed to malloc a buffer for logging. How do you not have 4KBs?? 😡😡\n");
            return -1;
        }
    }

    return 0;
}

int logFinalize() {
    if (logFileFd >= 0) {
        selector_unregister_fd(selector, logFileFd); // This will also call the TFdHandler's close, and close the file.
        selector = NULL;
    }

    if (buffer != NULL) {
        free(buffer);
        buffer = NULL;
        bufferCapacity = 0;
        bufferLength = 0;
        bufferStart = 0;
    }

    logStream = NULL;
    return 0;
}

int logString(const char* s) {
    LOG_PRINTF1("%s", s);
}

int logServerListening(const struct sockaddr* listenAddress, socklen_t listenAddressLen) {
    char addrBuffer[ADDRSTR_BUFLEN];
    if (listenAddress != NULL)
        printSocketAddress(listenAddress, addrBuffer);

    LOG_PRINTF1("Listening for TCP connections at %s", listenAddress == NULL ? "unknown address" : addrBuffer);
}

int logServerError(const char* err_msg, const char* info) {
    LOG_PRINTF3("Error: %s%s%s", err_msg, info == NULL ? "" : ", ", info == NULL ? "" : info);
}

int logNewClient(int clientId, const struct sockaddr* origin, socklen_t originLength) {
    char addrBuffer[ADDRSTR_BUFLEN];
    printSocketAddress(origin, addrBuffer);
    LOG_PRINTF2("New client connection from %s assigned id %d", addrBuffer, clientId);
}

int logClientDisconnected(int clientId, const char* username, const char* reason) {
    if (username == NULL) {
        LOG_PRINTF3("Client %d (not authenticated) disconnected%s%s", clientId, reason == NULL ? "" : ": ", reason == NULL ? "" : reason);
    } else {
        LOG_PRINTF4("Client %d (authenticated as %s) disconnected%s%s", clientId, username, reason == NULL ? "" : ": ", reason == NULL ? "" : reason);
    }
}

int logClientAuthenticated(int clientId, const char* username, int successful) {
    if (username == NULL) {
        LOG_PRINTF3("Client %d %ssuccessfully authenticated with no authentication method%s", clientId, successful ? "" : "un", successful ? "" : "... what?");
    } else {
        LOG_PRINTF3("Client %d %ssuccessfully authenticated as \"%s\"", clientId, successful ? "" : "un", username);
    }
}

int logClientConnectionRequestAddress(int clientId, const char* username, const struct sockaddr* remote, socklen_t remoteLength) {
    char addrBuffer[ADDRSTR_BUFLEN];
    printSocketAddress(remote, addrBuffer);
    if (username == NULL) {
        LOG_PRINTF2("Client %d (not authenticated) requested to connect to address %s", clientId, addrBuffer);
    } else {
        LOG_PRINTF3("Client %d (authenticated as %s) requested to connect to address %s", clientId, username, addrBuffer);
    }
}

int logClientConnectionRequestDomainname(int clientId, const char* username, const char* domainname) {
    if (username == NULL) {
        LOG_PRINTF2("Client %d (not authenticated) requested to connect to domainname %s", clientId, domainname);
    } else {
        LOG_PRINTF3("Client %d (authenticated as %s) requested to connect to domainname %s", clientId, username, domainname);
    }
}

int logClientConnectionRequestAttempt(int clientId, const char* username, const struct sockaddr* remote, socklen_t remoteLength) {
    char addrBuffer[ADDRSTR_BUFLEN];
    printSocketAddress(remote, addrBuffer);
    if (username == NULL) {
        LOG_PRINTF2("Attempting to connect to %s as requested by client %d (not authenticated)", addrBuffer, clientId);
    } else {
        LOG_PRINTF3("Attempting to connect to %s as requested by client %d (authenticated as %s)", addrBuffer, clientId, username);
    }
}

int logClientConnectionRequestSuccess(int clientId, const char* username, const struct sockaddr* remote, socklen_t remoteLength) {
    char addrBuffer[ADDRSTR_BUFLEN];
    printSocketAddress(remote, addrBuffer);
    if (username == NULL) {
        LOG_PRINTF2("Successfully connected to %s as requested by client %d (not authenticated)", addrBuffer, clientId);
    } else {
        LOG_PRINTF3("Successfully connected to %s as requested by client %d (authenticated as %s)", addrBuffer, clientId, username);
    }
}

int logClientBytesTransfered(int clientId, const char* username, size_t bytesSent, size_t bytesReceived) {
    return 0;
}