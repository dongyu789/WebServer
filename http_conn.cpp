#include "http_conn.h"

//定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

//网站的根目录
const char* doc_root = "/home/hufeiyu/webserver/resources";



int http_conn::m_epollfd = -1;       //所有socket上的事件都被注册到同一个epoll对象中
int http_conn::m_user_count = 0;    //统计用户的数量


//设置文件描述符非阻塞
int setnonblocking(int fd) {
    int old_flag = fcntl(fd, F_GETFL);
    int new_flag = old_flag | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_flag);
}

//向epoll中添加需要监听的文件描述符
void addfd(int epollfd, int fd, bool one_shot) {
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLRDHUP;//EPOLLRDHUP链接断开会发生，异常断开会在底层自动处理，不需要用read的返回值判断了

    if(one_shot) {
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);

    //设置文件描述符非阻塞
    setnonblocking(fd);
    
}

//从epoll中移除监听的文件描述符
void removefd(int epollfd, int fd) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//修改epoll中监听的文件描述符,重置socket上EPOLLONESHOT事件，以确保下一次可读时，EPOLLIN事件可以被触发
void modfd(int epollfd, int fd, int ev) {
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

//初始化链接
void http_conn::init(int sockfd, const struct sockaddr_in &addr) {
    m_sockfd = sockfd;
    m_address = addr;

    //端口复用
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    //添加到epoll对象中
    addfd(m_epollfd, sockfd, true);
    m_user_count++;//总用户数加一

    init();
}

//这里的成员初始化十分重要，每次处理完一个消息，都要进行一次初始化
void http_conn::init() {
    bytes_to_send = 0;
    bytes_have_send = 0;

    m_check_state = CHECK_STATE_REQUESTLINE;    //初始状态为解析首行
    m_checked_index = 0;    //当前已经解析到了位置
    m_start_line = 0;
    m_read_idx = 0;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_linger = false;
    m_host = 0;
    m_content_length = 0;
    m_write_idx = 0;

    memset(m_read_buf, 0, sizeof(m_read_buf));
    memset(m_write_buf, 0, WRITE_BUFFER_SIZE);
    memset(m_real_file, 0, FILENAME_LEN);
}

//关闭链接
void http_conn::close_conn() {
    if(m_sockfd != -1) {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;     //关闭一个链接，客户数量减一
    }
}

bool http_conn::read() {
    if(m_read_idx >= READ_BUFFER_SIZE) {
        return false;//缓冲区满了
    }
    
    //读取到的字节
    int bytes_read = 0;
    while(true) {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if(bytes_read == -1) {
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                //没有数据直接退出
                break;
            } else {
                return false;
            }
        } else if(bytes_read == 0) {
            //对方关闭链接
            return false;
        }

        m_read_idx += bytes_read;
    }

    printf("读取到了数据 \n%s\n", m_read_buf);

    return true;
} 

//主状态机,注意这个返回值的命名空间
http_conn::HTTP_CODE http_conn::process_read() {

    //定义初始状态
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;

    char *text = 0;
    while(((m_check_state == CHECK_STATE_CONTENT)&&(line_status == LINE_OK))||(line_status = parse_line()) == LINE_OK) {
        //解析到了一行完整的数据，或者解析到了请求体，也是完整的数据

        text = get_line();//在这里，m_start_line会比m_check_index慢一行，刚好给text读取
        
        m_start_line = m_checked_index;
        printf("got 1 http line :\n%s\n", text);

        switch(m_check_state) {
            case CHECK_STATE_REQUESTLINE:
            {
                ret = parse_request_line(text);
                if(ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                } 
                break;
            }
            case CHECK_STATE_HEADER:
            {
                ret = parse_headers(text);
                if(ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                } else if(ret == GET_REQUEST) {
                    //遇到请求头最后的换行，视为请求完整，所以要在这里判断
                    return do_request();//去解析具体内容
                }
                break;
            }
            case CHECK_STATE_CONTENT:
            {
                ret = parse_content(text);
                if(ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                } else if(ret == GET_REQUEST) {
                    return do_request();
                }
                line_status = LINE_OPEN;
                break;
            }
            default:
            {
                return INTERNAL_ERROR;
            }
        }
    }
    return NO_REQUEST;
}


http_conn::HTTP_CODE http_conn::do_request() {
    // /home/hufeiyu/webserver/resources
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    strncpy(m_real_file + len, m_url, FILENAME_LEN - len -1);
    //获取m_read_file文件的相关信息状态，-1失败0成功
    if(stat(m_real_file, &m_file_stat) < 0) {
        return NO_RESOURCE;
    }

    //判断访问权限
    if(! (m_file_stat.st_mode & S_IROTH)) {
        return FORBIDDEN_REQUEST;
    }

    //判断是否是目录
    if(S_ISDIR(m_file_stat.st_mode)) {
        return BAD_REQUEST;
    }

    //以只读的方式打开文件
    int fd = open(m_real_file, O_RDONLY);
    std::cout << "open : " << m_real_file << std::endl;


    //创建内存映射
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;


}

//对内存映射区执行munmap
void http_conn::unmap() {
    if(m_file_address) {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

//写http响应
bool http_conn::write() {
    int temp = 0;

    if(bytes_to_send == 0) {
        //将要发送的字节为0，。 这一次响应已经结束
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }

    int count = 1;

    while(1) {
        //分散写
        //printf("写了%d次\n", count++);
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if(temp <= -1) {
            //如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间
            //服务器无法立即接收到同一客户的下一个请求，但是可以保持链接的完整性
            if(errno == EAGAIN) {
                //printf("TCP写缓存区满了\n");
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;

        if(bytes_have_send >= m_iv[0].iov_len) {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        } else {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - temp;
        }
        
        if(bytes_to_send <= 0) {
            //没有数据需要发送
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);//继续监听这个用户的epollin事件
            if(m_linger) {
                init();
                return true;
            } else {
                return false;
            }
        }
    }
}

//解析HTTP请求行，获取请求方法，目标URL，HTTP版本
http_conn::HTTP_CODE http_conn::parse_request_line(char *text) {
    //GET /home/sunmaohua HTTP/1.1
    m_url = strpbrk(text, " \t");//获取到第一个是空格或者\t的位置，并且返回一个以这个位置开始的字符串

    //GET\0/home/sunmaohua HTTP/1.1
    *m_url++ = '\0';

    char *method = text;//因为已经有字符串结束符了
    //忽略大小写的比较
    if(strcasecmp(method, "GET") == 0) {
        m_method = GET;
    } else {
        return BAD_REQUEST;
    }

    //  /home/sunmaohua HTTP/1.1
    m_version = strpbrk(m_url, " \t");
    if(!m_version) {
        return BAD_REQUEST;
    }

    //  /home/sunmaohua\0HTTP/1.1
    *m_version++ = '\0';

    if(strcasecmp(m_version, "HTTP/1.1") != 0) {
        return BAD_REQUEST;
    }

    //  http://192.168.2.3:10000/home.html
    if(strncasecmp(m_url, "http://", 7) == 0) {
        m_url += 7;
        m_url = strchr(m_url, '/');//找到/斜线第一次出现的位置
    }

    if(!m_url || m_url[0] != '/') {
        return BAD_REQUEST;
    }

    m_check_state = CHECK_STATE_HEADER;//主状态机状态改变，状态变成检查请求头

    return NO_REQUEST;
}

//解析HTTP的头部信息,有问题，解析到某一个kv ，如何转到下一个
http_conn::HTTP_CODE http_conn::parse_headers(char *text) {
    //遇到空行，表示头部字段解析完毕
    if(text[0] == '\0') {
        //如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体
        //状态机转移到CHECK_STATE_CONTENT状态
        if(m_content_length != 0) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        //否则表示已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    } else if(strncasecmp(text, "Connection:", 11) == 0) {
        //处理Connection头部字段
        text += 11;
        text += strspn(text, " \t");//这里是为了把text移动到Connection:之后有字的位置，忽略掉中间的空格和\t
        if (strcasecmp(text, "keep-alive") == 0) {
            m_linger = true;
        }
    } else if(strncasecmp(text, "Content-Length:", 15) == 0) {
        //处理Content-Length头部字段
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    } else if (strncasecmp(text, "Host:", 5) == 0) {
        //处理Host头部
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    } else {
        printf("opp ! unknow header %s\n", text);
    }
    return NO_REQUEST;
}

//没有真正解析HTTP请求的消息体，只是判断是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text) {
    if(m_read_idx >= (m_content_length + m_checked_index)) {
        text[m_content_length] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}  

//解析一行，判断依据是\r\n,把这两个换成结束符
http_conn::LINE_STATUS http_conn::parse_line(){
    char temp;
    for(; m_checked_index < m_read_idx; m_checked_index++) {
        temp = m_read_buf[m_checked_index];
        if(temp == '\r'){
            if(m_checked_index + 1 == m_read_idx) {
                return LINE_OPEN;//状态机状态改变，没有读取到完整的，如果是完整的话，后面应该是\n
            } else if (m_read_buf[m_checked_index+1] == '\n') {
                m_read_buf[m_checked_index++] = '\0';//把请求数据\r符换成了结束符
                m_read_buf[m_checked_index++] = '\0';//把请求数据\n符换成了结束符
                return LINE_OK;
            } 
            return LINE_BAD;
        } else if(temp == '\n') {
            if(m_checked_index > 1 && m_read_buf[m_checked_index-1] == '\r') {
                m_read_buf[m_checked_index-1] = '\0';
                m_read_buf[m_checked_index++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        
    }


    return LINE_OK;
}


//往写缓冲区中写入待发送的数据
bool http_conn::add_response(const char* format, ...) {
    if(m_write_idx >= WRITE_BUFFER_SIZE) {
        return false;
    }

    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE -1 - m_write_idx, format, arg_list);
    if(len >= (WRITE_BUFFER_SIZE -1 -m_write_idx)) {
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);
    return true;
}

bool http_conn::add_status_line(int status, const char* title) {
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

bool http_conn::add_content_length(int content_len) {
    return add_response("Content-Length: %d\r\n", content_len);
}

bool http_conn::add_linger()
{
    return add_response( "Connection: %s\r\n", ( m_linger == true ) ? "keep-alive" : "close" );
}

bool http_conn::add_blank_line()
{
    return add_response( "%s", "\r\n" );
}

bool http_conn::add_content( const char* content )
{
    return add_response( "%s", content );
}

bool http_conn::add_content_type() {
    return add_response("Content-Type:%s\r\n", "text/html");
}

bool http_conn::add_headers(int content_len) {
    add_content_length(content_len);
    add_content_type();
    add_linger();
    add_blank_line();
}

bool http_conn::process_write(HTTP_CODE ret) {
    std::cout << "read ret = " << ret << std::endl;

    switch(ret) 
    {
        case INTERNAL_ERROR:
            add_status_line(500, error_500_title);
            add_headers(strlen(error_500_form));
            if(! add_content(error_500_form)) {
                return false;
            }
            break;
        case BAD_REQUEST:
            add_status_line( 400, error_400_title );
            add_headers( strlen( error_400_form ) );
            if ( ! add_content( error_400_form ) ) {
                return false;
            }
            break;
        case NO_RESOURCE:
            add_status_line( 404, error_404_title );
            add_headers( strlen( error_404_form ) );
            if ( ! add_content( error_404_form ) ) {
                return false;
            }
            break;
        case FORBIDDEN_REQUEST:
            add_status_line( 403, error_403_title );
            add_headers(strlen( error_403_form));
            if ( ! add_content( error_403_form ) ) {
                return false;
            }
            break;
        case FILE_REQUEST:
            //printf("文件请求：%s\n", m_real_file);
            add_status_line(200, ok_200_title );
            add_headers(m_file_stat.st_size);
            m_iv[ 0 ].iov_base = m_write_buf;
            m_iv[ 0 ].iov_len = m_write_idx;
            m_iv[ 1 ].iov_base = m_file_address;
            m_iv[ 1 ].iov_len = m_file_stat.st_size;
            m_iv_count = 2;

            bytes_to_send = m_write_idx + m_file_stat.st_size;

            return true;
        default:
            printf("write defaule。。\n");
            return false;
    }

    m_iv[ 0 ].iov_base = m_write_buf;
    m_iv[ 0 ].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
    
}

//由线程池中的工作线程调用，这是处理HTTP请求的入口函数
void http_conn::process() {
    //解析HTTP请求
    //printf("执行process\n");
    HTTP_CODE read_ret = process_read();
    if(read_ret == NO_REQUEST) {
        modfd(m_epollfd, m_sockfd, EPOLLIN);//继续监听epollin
        return;
    }

    //生成响应
    bool write_ret = process_write(read_ret);
    if(! write_ret) {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT);
}