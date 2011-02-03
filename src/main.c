#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/poll.h>

#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <ifaddrs.h>

#include <time.h>

#include "turnclient.h"
#include "sockaddr_util.h"


#define PORT "5799"
//#define  TEST_THREAD_CTX 1
#define MAXBUFLEN 1024

static const uint32_t TEST_THREAD_CTX = 1;


struct listenConfig{
    int stunCtx;
    int sockfd;
    char* user;
    char* pass;


};

static bool getLocalInterFaceAddrs(struct sockaddr *addr, char *iface, int ai_family){
    struct ifaddrs *ifaddr, *ifa;
    int family, s;
    char host[NI_MAXHOST];
    
    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        exit(EXIT_FAILURE);
    }
    
    /* Walk through linked list, maintaining head pointer so we
       can free list later */
    
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL)
            continue;
        
        if (sockaddr_isAddrLoopBack(ifa->ifa_addr))
            continue;

        if( strcmp(iface, ifa->ifa_name)!=0 )
            continue;

        if (ai_family == AF_INET6 ){
            if (sockaddr_isAddrLinkLocal(ifa->ifa_addr))
                continue;
        }
            

        family = ifa->ifa_addr->sa_family;
                        
        if (family == ai_family) {
                        
            s = getnameinfo(ifa->ifa_addr,
                            (family == AF_INET) ? sizeof(struct sockaddr_in) :
                            sizeof(struct sockaddr_in6),
                            host, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);
            if (s != 0) {
                printf("getnameinfo() failed: %s\n", gai_strerror(s));
                exit(EXIT_FAILURE);
            }
        }
    }
    freeifaddrs(ifaddr);

    if (sockaddr_initFromString(addr, host))
        return true;

    return false;
}

static int createLocalUDPSocket(int ai_family, struct sockaddr * localIp){
    int sockfd;
    
    int rv;
    int yes = 1;
    struct addrinfo hints, *ai, *p;
    char addr[SOCKADDR_MAX_STRLEN];


    sockaddr_toString(localIp, addr, sizeof addr, false);


    // get us a socket and bind it
    memset(&hints, 0, sizeof hints);
    hints.ai_family = ai_family;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_NUMERICHOST | AI_ADDRCONFIG;
        
        
    if ((rv = getaddrinfo(addr, PORT, &hints, &ai)) != 0) {
        fprintf(stderr, "selectserver: %s ('%s')\n", gai_strerror(rv), addr);
        exit(1);
    }
    


    for (p = ai; p != NULL; p = p->ai_next) {
        
        sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sockfd < 0) { 
            printf("Unable to open socket\n");
            continue;
        }
        
        // lose the pesky "address already in use" error message
        setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));
        if (sockaddr_isAddrAny(p->ai_addr) ){
            printf("Ignoring any\n");
            continue;
            
        }
        if (bind(sockfd, p->ai_addr, p->ai_addrlen) < 0) {
            printf("Bind failed\n");
            close(sockfd);
            continue;
        }
        
        if (localIp != NULL){ 
            sockaddr_copy(localIp, p->ai_addr);
                
            printf("Bound to: '%s'\n",
                   sockaddr_toString(localIp, addr, sizeof(addr), true));
                
        }
        
        break;
    }
    //printf("Soket open: %i\n", sockfd);
    return sockfd;
}





static int sendRawUDP(int sockfd, 
                      const void *buf, 
                      size_t len, 
                      struct sockaddr * p, 
                      socklen_t t){
    
    int numbytes;
    char addr[256];
    int rv;

    struct pollfd ufds[1];


    ufds[0].fd = sockfd;
    ufds[0].events = POLLOUT;

    rv = poll(ufds, 1, 3500);
    
    if (rv == -1) {
        perror("poll"); // error occurred in poll()
    } else if (rv == 0) {
        printf("Timeout occurred!  Not possible to send for 3.5 seconds.\n");
    } else {
        // check for events on s1:
        if (ufds[0].revents & POLLOUT) {
            
            numbytes = sendto(sockfd, buf, len, 0,
                              p , t); 
                        
            sockaddr_toString(p, addr, 256, true); 
            //printf("Sending Raw (To: '%s'(%i), Bytes:%i/%i  )\n", addr, sockfd, numbytes, (int)len);
            
            return numbytes;
        }
    }
    
    return -1;
}


static int SendRawStun(int sockfd, 
                       uint8_t *buf, 
                       int len, 
                       struct sockaddr *addr,
                       socklen_t t,
                       void *userdata){

    return  sendRawUDP(sockfd, buf, len, addr, t);

}


/* Callback for management info  */
static void  PrintTurnInfo(TurnInfoCategory_T category, char *ErrStr)
{
    //fprintf(stderr, "%s\n", ErrStr);
}


static void TurnStatusCallBack(void *ctx, TurnCallBackData_T *retData)
{
    char addr[SOCKADDR_MAX_STRLEN];
    printf("Got TURN status callback (%i)\n", retData->turnResult);
    if ( retData->turnResult == TurnResult_AllocOk ){
        printf("Active TURN server: '%s'\n",
               sockaddr_toString((struct sockaddr *)&retData->TurnResultData.AllocResp.activeTurnServerAddr,
                                 addr,
                                 sizeof(addr),
                                 true));

        printf("RFLX addr: '%s'\n",
               sockaddr_toString((struct sockaddr *)&retData->TurnResultData.AllocResp.rflxAddr,
                                 addr,
                                 sizeof(addr),
                                 true));

        printf("RELAY addr: '%s'\n",
               sockaddr_toString((struct sockaddr *)&retData->TurnResultData.AllocResp.relAddr,
                                 addr,
                                 sizeof(addr),
                                 true));

        retData->TurnResultData.AllocResp;
    }

}


void *tickTurn(void *ptr){
    struct timespec timer;
    struct timespec remaining;
    uint32_t  *ctx = (uint32_t *)ptr;
    
    timer.tv_sec = 0;
    timer.tv_nsec = 50000000;
    
    for(;;){
        nanosleep(&timer, &remaining);               
        TurnClient_HandleTick(*ctx);
    }
}


void *stunListen(void *ptr){
    struct pollfd ufds[1];
    struct listenConfig *config = (struct listenConfig *)ptr;
    struct sockaddr_storage their_addr;
    char buf[MAXBUFLEN];
    socklen_t addr_len;
    int rv;
    int numbytes;
    bool isMsSTUN;

    ufds[0].fd = config->sockfd;
    ufds[0].events = POLLIN | POLLPRI; // check for normal or out-of-band
                    
    addr_len = sizeof their_addr;

    while(1){
        rv = poll(ufds, 1, -1);
        
        if (rv == -1) {
            perror("poll"); // error occurred in poll()
        } else if (rv == 0) {
            printf("Timeout occurred! (Should not happen)\n");
        } else {
            // check for events on s1:
            if (ufds[0].revents & POLLIN) {
            
                if ((numbytes = recvfrom(config->sockfd, buf, MAXBUFLEN-1 , 0,
                                         (struct sockaddr *)&their_addr, &addr_len)) == -1) {
                    perror("recvfrom");
                    exit(1);
                }
                
                if ( stunlib_isStunMsg(buf, numbytes, &isMsSTUN) ){
                    StunMessage msg;
                                
                    stunlib_DecodeMessage(buf,
                                          numbytes,
                                          &msg,
                                          NULL,
                                          false,
                                          false);

                    
                            
                    TurnClient_HandleIncResp(TEST_THREAD_CTX,
                                             config->stunCtx, 
                                             &msg,
                                             buf);
                }
            }
        }
    }
}

int main(int argc, char *argv[])
{
    int sockfd_4, sockfd_6, sockfd;
        

    
    int stunCtx;
    struct sockaddr_storage ss_addr;
    struct sockaddr_storage localIp4, localIp6;
    static TurnCallBackData_T TurnCbData;

    struct listenConfig listenConfig;
  

    pthread_t turnTickThread;
    pthread_t listenThread;
    
        
    char realm[256];
    
    
    


    if (argc != 5) {
        fprintf(stderr,"usage: testice  iface [ip:port] user pass\n");
        exit(1);
    }
    

    if (!getLocalInterFaceAddrs((struct sockaddr *)&localIp4, argv[1], AF_INET) ){
        fprintf(stderr,"Unable to find local IPv4 interface addresses\n");
    }else{
        sockfd_4 = createLocalUDPSocket(AF_INET, (struct sockaddr *)&localIp4);
    }

    if (!getLocalInterFaceAddrs((struct sockaddr *)&localIp6, argv[1], AF_INET6) ){
        fprintf(stderr,"Unable to find local IPv6 interface addresses\n");
    }else{
        sockfd_6 = createLocalUDPSocket(AF_INET6, (struct sockaddr *)&localIp6);
    }
    

    
    //


    //Turn setup
    TurnClient_Init(TEST_THREAD_CTX, 50, 50, PrintTurnInfo, false, "TestIce");

    sockaddr_initFromString((struct sockaddr *)&ss_addr, 
                            argv[2]);

   

    if(ss_addr.ss_family == AF_INET){
        printf("Using IPv4 Socket\n");
        //make sure the port is set
        if (((struct sockaddr_in *)&ss_addr)->sin_port == 0 ) {
            ((struct sockaddr_in *)&ss_addr)->sin_port = htons(3478);
        }
        sockfd = sockfd_4;

    }else if(ss_addr.ss_family == AF_INET6){
        printf("Using IPv6 Socket\n");
        //make sure the port is set
        if (((struct sockaddr_in6 *)&ss_addr)->sin6_port == 0 ) {
            ((struct sockaddr_in6 *)&ss_addr)->sin6_port = htons(3478);
        }
        sockfd = sockfd_6;
    }

    
    stunCtx = TurnClient_startAllocateTransaction(TEST_THREAD_CTX,
                                                  NULL,
                                                  (struct sockaddr *)&ss_addr,
                                                  argv[3],
                                                  argv[4],
                                                  sockfd,                       /* socket */
                                                  SendRawStun,             /* send func */
                                                  NULL,  /* timeout list */
                                                  TurnStatusCallBack,
                                                  &TurnCbData,
                                                  false);
    
    pthread_create( &turnTickThread, NULL, tickTurn, (void*) &TEST_THREAD_CTX);

    listenConfig.stunCtx = stunCtx;
    listenConfig.sockfd = sockfd;
    listenConfig.user = argv[3];
    listenConfig.pass = argv[4];



    pthread_create( &listenThread,   NULL, stunListen, (void*) &listenConfig);

    //stunListen(&listenConfig);
    pthread_join( &listenThread, NULL);
    
    while(1)
        sleep(2);
    
    close(sockfd);
    
    return 0;
}
