#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "lock/locker.h"
#include "threadpool/threadpool.h"
#include "http_conn/http_conn.h"
#include "timer/lst_timer.h"
#include "./log/log.h"

#define MAX_FD 65536
#define MAX_EVENT_NUMBER 10000
//时间间隔
#define TIMESLOT 5

#define SYNSQL

#define SYNLOG //同步写日志
//#define ASYNLOG       //异步写日志

//http_conn.cpp中定义，改变链接属性
extern int addfd( int epollfd, int fd, bool one_shot );
extern int removefd( int epollfd, int fd );
extern int setnonblocking(int fd);

//定时器相关参数
static int pipefd[2];
static sort_timer_lst timer_lst;
static int epollfd = 0;

//信号处理函数
void sig_handler(int sig)
{
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char*)&msg, 1 ,0);
    errno = save_errno;
}

//设置信号函数
void addsig( int sig, void( handler )(int), bool restart = true )
{
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = handler;
    if( restart )
    {
        sa.sa_flags |= SA_RESTART;
    }
    sigfillset( &sa.sa_mask );
    assert( sigaction( sig, &sa, NULL ) != -1 );
}

//定时处理任务，重新定时不断触发SIGALAM
void timer_handler()
{
    //tick处理到期定时器
    timer_lst.tick();
    alarm(TIMESLOT);
}

//定时器回调函数，删除连接资源
void cb_func(client_data *user_data)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    http_conn::m_user_count--;
    LOG_INFO("close fd %d", user_data->sockfd);
    Log::get_instance()->flush();
}
void show_error( int connfd, const char* info )
{
    printf( "%s", info );
    send( connfd, info, strlen( info ), 0 );
    close( connfd );
}


int main( int argc, char* argv[] )
{
    //初始化日志模型
    #ifdef ASYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 8); //异步日志模型
#endif

#ifdef SYNLOG
    Log::get_instance()->init("ServerLog", 2000, 800000, 0); //同步日志模型
#endif
    if( argc <= 2 )
    {
        printf( "usage: %s ip_address port_number\n", basename( argv[0] ) );
        return 1;
    }
    const char* ip = argv[1];
    int port = atoi( argv[2] );

    addsig( SIGPIPE, SIG_IGN );

    //创建数据库连接池,有8条连接
    connection_pool *connPool = connection_pool::GetInstance();
    connPool->init("localhost", "root", "041698", "yourdb", 3306, 8);


    threadpool< http_conn >* pool = NULL;
    try
    {
        pool = new threadpool< http_conn >(connPool);
    }
    catch( ... )
    {
        return 1;
    }

    http_conn* users = new http_conn[ MAX_FD ];
    assert( users );
    
#ifdef SYNSQL
    //初始化数据库读取表，将数据存入map
    users->initmysql_result(connPool);
#endif

    int listenfd = socket( PF_INET, SOCK_STREAM, 0 );
    assert( listenfd >= 0 );
    // struct linger tmp = { 1, 0 };
    //setsockopt( listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof( tmp ) );

    int ret = 0;
    struct sockaddr_in address;
    bzero( &address, sizeof( address ) );
    address.sin_family = AF_INET;
    inet_pton( AF_INET, ip, &address.sin_addr );
    address.sin_port = htons( port );

    int flag = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    ret = bind( listenfd, ( struct sockaddr* )&address, sizeof( address ) );
    assert( ret >= 0 );

    ret = listen( listenfd, 5 );
    assert( ret >= 0 );

    epoll_event events[ MAX_EVENT_NUMBER ];
    epollfd = epoll_create( 5 );
    assert( epollfd != -1 );
    addfd( epollfd, listenfd, false );
    http_conn::m_epollfd = epollfd;

    //创建管道
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    setnonblocking(pipefd[1]);
    addfd(epollfd, pipefd[0], false);

    //添加信号
    addsig(SIGALRM, sig_handler, false);
    addsig(SIGTERM, sig_handler, false);
    bool stop_server = false;

    //创建连接资源类
    client_data *users_timer = new client_data[MAX_FD];

    bool timeout = false;
    alarm(TIMESLOT);

    while( !stop_server )
    {
        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1 );
        if ( ( number < 0 ) && ( errno != EINTR ) )
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }

        for ( int i = 0; i < number; i++ )
        {
            int sockfd = events[i].data.fd;
            if( sockfd == listenfd )
            {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof( client_address );
                int connfd = accept( listenfd, ( struct sockaddr* )&client_address, &client_addrlength );
                if ( connfd < 0 )
                {
                    LOG_ERROR("%s:errno is:%d", "accept error", errno);
                    continue;
                }
                if( http_conn::m_user_count >= MAX_FD )
                {
                    show_error( connfd, "Internal server busy" );
                    LOG_ERROR("%s", "Internal server busy");
                    continue;
                }
                
                users[connfd].init( connfd, client_address );
                //初始化client_data数据，创建定时器，设置时间、函数、绑定
                //数据，添加定时器到链表
                users_timer[connfd].address = client_address;
                users_timer[connfd].sockfd = connfd;
                util_timer* timer = new util_timer;
                timer->user_data = &users_timer[connfd];
                timer->cb_func = cb_func;
                time_t cur = time(NULL);
                timer->expire = cur + 3 * TIMESLOT;
                users_timer[connfd].timer = timer;
                timer_lst.add_timer(timer);
                
            }
            else if( events[i].events & ( EPOLLRDHUP | EPOLLHUP | EPOLLERR ) )
            {
                //服务器关闭连接，移除相应定时器
                util_timer* timer = users_timer[sockfd].timer;
                timer->cb_func(&users_timer[sockfd]);
                if(timer)
                    timer_lst.del_timer(timer);
            }
            //处理管道的信号
            else if((sockfd == pipefd[0]) && events[i].events & EPOLLIN)
            {
                int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if(ret == -1)
                    continue;
                else if(ret == 0)
                    continue;
                else
                {
                    for(int i = 0; i < ret; ++i)
                    {
                        switch(signals[i])
                        {
                        case SIGALRM:
                            {
                                timeout = true;
                                break;
                            }
                        case SIGTERM:
                            {
                                stop_server = true;
                            }
                        }
                    }
                }
            }
            else if( events[i].events & EPOLLIN )
            {
                //获取该连接的定时器
                util_timer* timer = users_timer[sockfd].timer;
                if( users[sockfd].read() )
                {
                    LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();
                    pool->append( users + sockfd );
                    //延长定时器并调整
                    if(timer)
                    {
                        time_t cur = time(NULL);
                        time_t expire = cur + 3 * TIMESLOT;
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }
                }
                else
                {
                    timer->cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
            }
            else if( events[i].events & EPOLLOUT )
            {
                util_timer *timer = users_timer[sockfd].timer;
                if( users[sockfd].write() )
                {
                    LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
                        timer_lst.adjust_timer(timer);
                    }
                }
                else
                {
                    timer->cb_func(&users_timer[sockfd]);
                    if(timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
            }
            
        }
        if(timeout)
        {
            timer_handler();
            timeout = false;
        }
    }

    close( epollfd );
    close( listenfd );
    close(pipefd[1]);
    close(pipefd[0]);
    delete [] users;
    delete [] users_timer;
    delete pool;
    return 0;
}

