#ifndef HTTP_CONN_H
#define HTTP_CONN_H
#include <sys/epoll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include "locker.h" 
#include <sys/uio.h>
#include <iostream>

class http_conn {
public:

    //http请求方法，我们只支持get
    enum METHOD {GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT};

    /*
        解析客户端请求时，主状态机的状态
        CHECK_STATE_REQUESTLINE:当前正在分析请求行
        CHECK_STATE_HEADER:当前正在分析请求头
        CHECK_STATE_CONTENT:当前正在分析请求体

    */
    enum CHECK_STATE {CHECK_STATE_REQUESTLINE = 0, CHECK_STATE_HEADER, CHECK_STATE_CONTENT};

    //从状态机的三种可能的状态，即行读取的状态，分别表示
    //1 读取到一个完整的行， 2 行出错， 3 数据行不完整
    enum LINE_STATUS {LINE_OK = 0, LINE_BAD, LINE_OPEN};

    /*
        服务器处理http请求的可能结果，报文解析的结果
        NO_REQUEST  : 请求报文不完整，需要继续读取客户端数据
        GET_REQUEST :   表示获得了一个完整的客户请求
        BAD_REQUEST :   表示客户请求语法错误
        NO_RESOURCE :   表示服务器没有资源
        FORBIDDEN_REQUEST   :   表示客户对资源没有足够的访问权限
        FILE_REQUEST    :   文件请求，获取文件成功
        INTERNAL_ERROR  :   表示服务器内部错误
        CLOSED_CONNECTION   :   表示客户端已经关闭链接了
    */
    enum HTTP_CODE {NO_REQUEST, GET_REQUEST, BAD_REQUEST, NO_RESOURCE, FORBIDDEN_REQUEST, FILE_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION};

    static int m_epollfd;       //所有socket上的事件都被注册到同一个epoll对象中
    static int m_user_count;    //统计用户的数量

    static const int READ_BUFFER_SIZE = 2048;   //读缓冲区大小
    static const int WRITE_BUFFER_SIZE = 1024;  //写缓冲区大小
    static const int FILENAME_LEN = 200;        //文件名的最大长度

public:
    http_conn() {}
    ~http_conn() {}
    void init();//初始化链接其余的信息

public:
    void process();//处理客户端的请求，解析http请求
    void init(int sockfd, const struct sockaddr_in &addr);//初始化新链接
    void close_conn();//关闭链接
    bool read(); //非阻塞读数据
    bool write();//非阻塞写数据

private:
    //进入解析中，也会修改状态机的状态，让其进入下一个状态
    HTTP_CODE process_read();   //解析http请求

    // 下面这一组函数被process_read调用以分析HTTP请求
    HTTP_CODE parse_request_line(char *text);   //解析http请求首行
    HTTP_CODE parse_headers(char *text);   //解析http请求头
    HTTP_CODE parse_content(char *text);   //解析http请求体
    HTTP_CODE do_request();
    char * get_line() {return m_read_buf + m_start_line; }
    LINE_STATUS parse_line();   //解析一行

private:
    bool process_write(HTTP_CODE ret);  //填充HTTP应答

    //这一组函数被process_write调用填充HTTP应答
    void unmap();
    bool add_response(const char * format, ...);
    bool add_content(const char *content);
    bool add_content_type();
    bool add_status_line(int status, const char* title);
    bool add_headers(int content_length);
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();




private:

    char *m_url;        //请求目标文件的文件名
    char *m_version;    //协议版本，只支持http1.1
    METHOD m_method;    //请求方法
    char *m_host;       //主机名
    int m_sockfd;   //这个HTTP链接的socket
    struct sockaddr_in m_address;  //socket地址

    char m_read_buf[READ_BUFFER_SIZE];  //读缓冲区
    int m_read_idx; //标识读缓冲区中以及读入的客户端数据的最后一个字节的下一个下标，这个位置是第一个空位置
    int m_checked_index;    //我们当前正在分析的字符在读缓冲区的位置
    int m_start_line;       //当前正在解析的行的起始位置

    CHECK_STATE m_check_state;  //主状态机当前所处的状态

    char m_real_file[FILENAME_LEN];     //客户请求的目标文件的完整路径，其内容等于doc_root + m_url,doc_root是网站根目录
    int m_content_length;               //HTTP请求的消息总长度
    bool m_linger;                      //HTTP是否要求保持链接

    char m_write_buf[WRITE_BUFFER_SIZE];    //写缓冲区
    int m_write_idx;    //写缓冲区待发送的字节下标
    char *m_file_address;   //客户请求的目标文件被mmap到内存中的起始位置
    struct stat m_file_stat;    //目标文件的状态，通过它我们可以判断文件是否存在，是否为目录，是否可读，并获取文件大小等信息
    struct iovec m_iv[2];       //我们将采用writev来执行写操作，所以定义下面两个成员，其中m_iv_count表示被写内存块的数量
    int m_iv_count;

    int bytes_to_send;  //将要发送的字节数
    int bytes_have_send;    //已经发送的字节数

};


#endif