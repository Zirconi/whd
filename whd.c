#include "headerc.h"
#include "whd.h"
#include "rio/rio.h"

pthread_mutex_t ALOCK = PTHREAD_MUTEX_INITIALIZER;

void fake_main(void)
{
    signal(SIGPIPE, SIG_IGN);
    int listenfd, err, i;
    pid_t pid;//, contact[WORKER];
    struct sockaddr_in servaddr;

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons((getuid() == 0) ? 80 : 8080);

    if((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        FATAL("FAKE_MAIN.SOCKET");

    if((err = bind(listenfd, (SA *)&servaddr, sizeof(servaddr))) < 0)
        FATAL("FAKE_MAIN.BIND");

    if((err = listen(listenfd, 1024)) < 0)
        FATAL("FAKE_MAIN.LISTEN");

    for(i = 0; i < WORKER; ++i)
    {
        if((pid = fork()) == 0)
            start_worker(listenfd);
        else if(pid < 0)
            //contact[i] = pid;
            FATAL("FAKE_MAIN.FORK");
    }

    for(;;)
    {
        //TODO
        pause();
    }
}

void start_worker(int listenfd)
{
    struct sockaddr_in cliaddr;
    struct epoll_event ev, events[MAXLEN];
    socklen_t clilen;
    int connfd, efd, nfds, n;
    char buf[MAXLEN], content[MAXLEN];
    char *rstline = (char *)malloc(MAXLEN);
    char *method = (char *)malloc(MAXLEN);
    char *uri = (char *)malloc(MAXLEN);
    char *protocol = (char *)malloc(MAXLEN);
    char *filename = (char *)malloc(MAXLEN);
    char *filetype = (char *)malloc(MAXLEN);
    char *response = (char *)malloc(MAXLEN);


    if((efd = epoll_create(MAXLEN)) < 0)
        FATAL("WORKER.EPOLL_CREATE");

    ev.data.fd = listenfd;
    ev.events = EPOLLIN; //EPOLLET
    if(epoll_ctl(efd, EPOLL_CTL_ADD, listenfd, &ev) < 0)
        FATAL("WORKER.EPOLL_CTL");


    for(;;)
    {
        clilen = sizeof(cliaddr);

        //epoll_wait Thundering Herd?
        //EINTR?
    again:
        if((nfds = epoll_wait(efd, events, MAXLEN, -1)) < 0)
        {
            if(errno == EINTR)
                goto again;
            else
                FATAL("WORKER.EPOLL_WAIT");
        }

        for(n = 0; n < nfds; ++n)
        {
            if(events[n].data.fd == listenfd) 
            {
                pthread_mutex_lock(&ALOCK);
                if((connfd = accept(listenfd, (SA *)&cliaddr, &clilen)) < 0)
                    FATAL("WORKER.ACCEPT");
                pthread_mutex_unlock(&ALOCK);

                snprintf(content, MAXLEN, "Client : %s\n", inet_ntop(AF_INET, &clilen, buf, clilen));
                //syslog(LOG_INFO, content);

                setnonblocking(connfd);

                ev.data.fd = connfd;
                ev.events = EPOLLIN | EPOLLET;

                if(epoll_ctl(efd, EPOLL_CTL_ADD, connfd, &ev) < 0)
                    FATAL("WORKER.EPOLL_CTL");
            }
            else
            {
                process(events[n].data.fd, efd, rstline, method, uri, protocol, filename, filetype, response);              
            }
        }
    }
    free(rstline);
    free(method);
    free(uri);
    free(protocol);
    free(filename);
    free(filetype);
    free(response);
}



void process(int fd, int efd, char *rstline, char *method, char *uri, char *protocol, char *filename, char *filetype, char *response)
{
    rio_t rio;
    int err;
    struct stat sbuf;
    char *file;
    rio_readinitb(&rio, fd);

    if((err = rio_readlineb(&rio, rstline, MAXLEN)) <= 0)
    {
        if((err = epoll_ctl(efd, EPOLL_CTL_DEL, fd, NULL)) < 0)
            FATAL("PROCESS.EPOLL_CTL");
        close(fd);
    }
    else
    {
        sscanf(rstline, "%s %s %s", method, uri, protocol);
        if(strcasecmp(method, "GET"))
            //TEST
            ERR(fd, efd);
        if(uri[0] != '/')
            ERR(fd, efd);
        process_rstheader(&rio);
        parse_uri(uri, filename, filetype);
        if(get_fileinfo(filename) < 0)
            call_403(fd);
        else
        {
            lstat(filename, &sbuf);
            err = open(filename, O_RDONLY, 0);
            file = mmap(0 ,sbuf.st_size, PROT_READ, MAP_PRIVATE, err, 0);
            close(err);
            sprintf(response, "HTTP/1.1 200 OK\r\n");
            sprintf(response, "%sContent-length: %lu\r\n", response, sbuf.st_size);
            sprintf(response, "%sContent-type: %s\r\n\r\n", response, filetype);
            rio_writen(fd, response, strlen(response));
            rio_writen(fd, file, sbuf.st_size);
            munmap(file, sbuf.st_size);
        }
    }
}

void ERR(int fd, int efd)
{
    char re[] = "HTTP/1.1 403 Not Found\r\nContent-length: 14\r\nContent-type: text/html\r\n\r\n<p>ILLEGAL</p>";
    rio_writen(fd, re, strlen(re));
    if(epoll_ctl(efd, EPOLL_CTL_DEL, fd, NULL) < 0)
        FATAL("ERR.EPOLL_CTL");
    close(fd);
}

void call_403(int fd)
{
    char re[] = "HTTP/1.1 403 Not Found\r\nContent-length: 16\r\nContent-type: text/html\r\n\r\n<p>NOT FOUND</p>";
    rio_writen(fd, re, strlen(re));
}

void parse_uri(char *uri, char *filename, char *filetype)
{
    strcpy(filename, ".");
    strcat(filename, uri);
    if(uri[strlen(uri) - 1] == '/')
        strcat(filename, "index.html");

    get_filetype(filename, filetype);
}

void get_filetype(char *filename, char *filetype)
{
    if(strcasestr(filename, "html"))
        strcpy(filetype, "text/html");
    else if(strcasestr(filename, "jpg"))
        strcpy(filetype, "image/jpeg");
    else if(strcasestr(filename, "png"))
        strcpy(filetype, "image/png");
    else if(strcasestr(filename, "gif"))
        strcpy(filetype, "image/gif");
    else
        strcpy(filetype, "texp/plain");
}

void process_rstheader(rio_t *rp)
{
    char buf[MAXLEN];
    rio_readlineb(rp, buf, MAXLEN);
    while(strcmp(buf, "\r\n"))
        rio_readlineb(rp, buf, MAXLEN);
}

int get_fileinfo(char *filename)
{
    struct stat buf;
    if(lstat(filename, &buf) < 0)
        return -1;
    if((S_ISREG(buf.st_mode)) && (S_IRUSR & buf.st_mode))
        return 0;
    else
        return -1;
}

int setnonblocking(int fd)
{
    if((fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK)) < 0)
        return -1;
    return 0;
}
