#ifndef _CONNECTION_POOL_
#define _CONNECTION_POOL_

#include <stdio.h>
#include <list>
#include <mysql/mysql.h>
#include <error.h>
#include <string.h>
#include <iostream>
#include <string>
#include "../lock/locker.h"

using namespace std;

class connection_pool
{
public:
    MYSQL *GetConnection();             //获得数据库连接
    bool ReleaseConnection(MYSQL *conn);//释放连接
    int GetFreeConn();                  //获取连接
    void DestroyPool();                 //销毁所有连接

    //局部静态变量单例模式
    static connection_pool *GetInstance();
    
    //初始化连接池
    void init(string url, string User, string PassWord,
              string DataBaseName, int Port, unsigned int MaxConn);
    connection_pool();
    ~connection_pool();

private:
    unsigned int MaxConn;   //最大连接数
    unsigned int CurConn;   //当前已使用的连接数
    unsigned int FreeConn;  //当前空闲的连接数

private:
    locker lock;            //连接池的锁
    list<MYSQL *> connList; //连接池
    sem reserve;            //连接池信号量

private:
    string url;         //主机地址
    string Port;        //数据库端口号
    string User;        //登陆数据库用户名
    string PassWord;    //登陆数据库密码
    string DatabaseName;//使用数据库名
    
};

//将数据库连接的获取与释放通过RAII机制封装，避免手动释放
class connectionRAII
{
public:
    connectionRAII(MYSQL **con, connection_pool *connPool);
    ~connectionRAII();
private:
    MYSQL *conRAII;
    connection_pool *poolRAII;
};
#endif

