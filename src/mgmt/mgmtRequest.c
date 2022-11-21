#include "mgmtRequest.h"
#include "../passwordDissector.h"
#include "mgmtCmdParser.h"
#include "../logger.h"
#include "../users.h"
#include "mgmt.h"
#include "mgmtCmdParser.h"
#include "../logging/metrics.h"

void mgmtRequestReadInit(const unsigned state, TSelectorKey* key){
    log(DEBUG, "[Mgmt req read] init at socket fd %d", key->fd);
    TMgmtClient* data = GET_ATTACHMENT(key);
    initMgmtCmdParser(&data->client.cmdParser);
}
unsigned mgmtRequestRead(TSelectorKey* key){
    log(DEBUG, "[Mgmt req read] read at socket fd %d", key->fd);
    TMgmtClient* data = GET_ATTACHMENT(key);

    size_t readLimit;    // how many bytes can be stored in the buffer
    ssize_t readCount;   // how many bytes where read from the client socket
    uint8_t* readBuffer; // here are going to be stored the bytes read from the client

    readBuffer = buffer_write_ptr(&data->readBuffer, &readLimit);
    readCount = recv(key->fd, readBuffer, readLimit, 0);
    log(DEBUG, "[Mgmt req read]  %ld bytes from client %d", readCount, key->fd);
    if (readCount <= 0) {
        return MGMT_ERROR;
    }

    buffer_write_adv(&data->readBuffer, readCount);
    mgmtCmdParse(&data->client.cmdParser, &data->readBuffer);
    if (hasMgmtCmdReadEnded(&data->client.cmdParser)) {
        if (selector_set_interest_key(key, OP_WRITE) != SELECTOR_SUCCESS || fillMgmtCmdAnswer(&data->client.cmdParser, &data->writeBuffer)) {
            return MGMT_ERROR;
        }
        data->cmd = data->client.cmdParser.cmd;
        return MGMT_REQUEST_WRITE;
    }
    return MGMT_REQUEST_READ;
}

static void handleUserCmdResponse(buffer * buffer) {
    size_t size;
    uint8_t * ptr = buffer_write_ptr(buffer, &size);
    char * s = "users response\r\n";
    int len = strlen(s);
    strcpy((char *)ptr, s);
    
    // Esto se tiene que poder escribir si o si porque es un buffer que recien inicializamos
    buffer_write_adv(buffer, len);
}

static void handleAddUserCmdResponse(buffer * buffer, TMgmtParser* p){

    size_t size;
    uint8_t * ptr = buffer_write_ptr(buffer, &size);    
    char * username = p->args[0].string;
    char * password = p->args[1].string;
    int role = p->args[2].byte;

    static char* successMessage = "+OK user successfully added\n";
    static char* userAlreadyExistMessage = "-ERR user already exists\n";
    static char* credentialsTooLong = "-ERR credentials too long\n";
    static char* limitReachedMessage = "-ERR users limit reached\n";
    static char* noMemoryMessage = "-ERR no memory\n";
    static char* badRoleMessage = "-ERR role doesn't exist\n";
    static char* unkownErrorMessage = "-ERR can't add user, try again\n";
    char* toReturn = NULL;

    if(role < 0 || role > 1){
        toReturn = badRoleMessage;
    }

    if(toReturn == NULL){
        int status = usersCreate(username, password, false, role, false);

        switch(status){
            case EUSER_OK:
                toReturn = successMessage;
                break;
            case EUSER_ALREADYEXISTS:
                toReturn = userAlreadyExistMessage;
                break;
            case EUSER_CREDTOOLONG:
                toReturn = credentialsTooLong;
                break;
            case EUSER_LIMITREACHED:
                toReturn = limitReachedMessage;
                break;
            case EUSER_NOMEMORY:
                toReturn = noMemoryMessage;
                break;
            default:
                toReturn = unkownErrorMessage;
        }
    }

    strcpy((char *)ptr, toReturn);
    buffer_write_adv(buffer, strlen(toReturn));
}

static void handleDeleteUserCmdResponse(buffer * buffer, TMgmtParser* p){
    
    size_t size;
    uint8_t * ptr = buffer_write_ptr(buffer, &size);    
    char * username = p->args[0].string;

    static char* wrongUsernameMessage = "-ERR user doesn't exist";
    static char* badOperationMessage = "-ERR cannot delete user because you are the last sysadmin\n";
    static char* successMessage = "+OK user successfully deleted";
    static char* unkownErrorMessage = "-ERR can't delete user, try again";
    char* toReturn;

    int status = usersDelete(username);

    switch(status){
        case EUSER_OK:
            toReturn = successMessage;
            break;
        case EUSER_WRONGUSERNAME:
            toReturn = wrongUsernameMessage;
            break;
        case EUSER_BADOPERATION:
            toReturn = badOperationMessage;
            break;
        default:
            toReturn = unkownErrorMessage;
    }

    strcpy((char *)ptr, toReturn);
    buffer_write_adv(buffer, strlen(toReturn));
}

static void handleGetDissectorStatusCmdResponse(buffer * buffer) {
    size_t size;
    uint8_t * ptr = buffer_write_ptr(buffer, &size);
    static char * on = "+OK. Password dissector is: on\n";
    static char * off = "+OK. Password dissector is: off\n";
    int len;
    if(isPDissectorOn()){
        len = strlen(on);
        strcpy((char *)ptr, on);
    }else{
        len = strlen(off);
        strcpy((char *)ptr, off);
    }
    buffer_write_adv(buffer, len);
}

static void handleSetDissectorStatusCmdResponse(buffer * buffer, TMgmtParser* p){
    size_t size;
    uint8_t turnOn = p->args[0].byte;                              // OFF = 0 : ON = 1

    uint8_t * ptr = buffer_write_ptr(buffer, &size);
    static char * on = "+OK. Dissector status: on\n";
    static char * off = "+OK. Dissector status: off\n";
    int len;
    if(!turnOn){
        turnOffPDissector();
        len = strlen(off);
        strcpy((char *)ptr, off);
    } else {
        turnOnPDissector();
        len = strlen(on);
        strcpy((char *)ptr, on);
    }
    buffer_write_adv(buffer,len);
}

static int copyMetric(int idx, uint8_t * buff, char * metricString, size_t metricValue){

    char aux[256];
    int len = strlen(metricString);

    memcpy(buff + idx, metricString, len);
    idx += len;
    snprintf(aux, sizeof(aux), "%ld", metricValue);
    memcpy(buff + idx, aux, 1);
    idx+= strlen(aux);
    memcpy(buff + idx, "\n", 1);
    idx ++;

    return idx;
 }

 static void handleStatisticsCmdResponse(buffer * buffer){

    TMetricsSnapshot* metrics = calloc(1, sizeof(TMetricsSnapshot));
    getMetricsSnapshot(metrics);

    static char* successMessage = "+OK. Showing stats:\n";
    int sucLength = strlen(successMessage);

    static char* connectionCount = "CONC:";
    static char* maxConcurrmetrics = "MCONC:";
    static char* totalBytesRecv = "TBRECV:";
    static char* totalBytesSent = "TBSENT";
    static char* totalConnectionCount = "TCON:";
 
    uint8_t statistics[512];
    memcpy(statistics, successMessage, sucLength);
    int idx = sucLength;

    idx = copyMetric(idx, statistics, connectionCount, metrics->currentConnectionCount);
    idx = copyMetric(idx, statistics, maxConcurrmetrics, metrics->maxConcurrentConnections);
    idx = copyMetric(idx, statistics, totalBytesRecv, metrics->totalBytesReceived);
    idx = copyMetric(idx, statistics, totalBytesSent, metrics->totalBytesSent);
    idx = copyMetric(idx, statistics, totalConnectionCount, metrics->totalConnectionCount);
    statistics[idx] = 0;

    size_t size;
    uint8_t* ptr = buffer_write_ptr(buffer, &size);
    strcpy((char*)ptr, (char*)statistics);
    buffer_write_adv(buffer, strlen((char *) statistics));
    free(metrics);
 }

void mgmtRequestWriteInit(const unsigned int st, TSelectorKey* key) {
    TMgmtClient * data = GET_ATTACHMENT(key);
    buffer_init(&(data->responseBuffer), MGMT_BUFFER_SIZE, data->responseRawBuffer);
    
    if (data->cmd == MGMT_CMD_USERS) {
        handleUserCmdResponse(&data->responseBuffer);
    } else if(data->cmd == MGMT_CMD_ADD_USER) {
       handleAddUserCmdResponse(&data->responseBuffer, &data->client.cmdParser);
    } else if(data->cmd == MGMT_CMD_DELETE_USER){
        handleDeleteUserCmdResponse(&data->responseBuffer, &data->client.cmdParser);
    } else if(data->cmd == MGMT_CMD_GET_DISSECTOR){
        handleGetDissectorStatusCmdResponse(&data->responseBuffer);
    } else if(data->cmd == MGMT_CMD_SET_DISSECTOR){
        handleSetDissectorStatusCmdResponse(&data->responseBuffer, &data->client.cmdParser);
    } else if(data->cmd == MGMT_CMD_STATISTICS){
        handleStatisticsCmdResponse(&data->responseBuffer);
    } else {
        // TO DO
    }
}

unsigned mgmtRequestWrite(TSelectorKey* key){
    log(DEBUG, "[Mgmt req write] send at fd %d", key->fd);
    TMgmtClient* data = GET_ATTACHMENT(key);

    size_t writeLimit;    // how many bytes we want to send
    ssize_t writeCount = 0;   // how many bytes where written
    uint8_t* writeBuffer; // buffer that stores the data to be sended

 // new
    writeBuffer = buffer_read_ptr(&data->responseBuffer, &writeLimit);
    writeCount = send(key->fd, writeBuffer, writeLimit, MSG_NOSIGNAL);
    log(DEBUG, "[Mgmt req write] sent %ld bytes", writeCount);

// ----

    // writeBuffer = buffer_read_ptr(&data->writeBuffer, &writeLimit);
    // writeCount = send(key->fd, writeBuffer, writeLimit, MSG_NOSIGNAL);

    if (writeCount < 0) {
        log(LOG_ERROR, "[Mgmt req write] send() at fd %d", key->fd);
        return MGMT_ERROR;
    }
    if (writeCount == 0) {
        log(LOG_ERROR, "[Mgmt req write] Failed to send(), client closed connection unexpectedly at fd %d", key->fd);
        return MGMT_ERROR;
    }
    log(DEBUG, "[Mgmt req write]  %ld bytes to client %d", writeCount, key->fd);
    buffer_read_adv(&data->responseBuffer, writeCount);

    if (buffer_can_read(&data->responseBuffer)) {
        return MGMT_REQUEST_WRITE;
    }

    if (hasMgmtCmdErrors(&data->client.cmdParser)) {
        return MGMT_ERROR;
    }
    return MGMT_DONE;
}