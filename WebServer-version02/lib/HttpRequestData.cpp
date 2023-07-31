#include "HttpRequestData.h"
#include "util.h"
#include "epoll.h"
#include "_cmpublic.h"
#include "log.h"

// #include <opencv/cv.h>
// #include <opencv2/core/core.hpp>
// #include <opencv2/highgui/highgui.hpp>
// #include <opencv2/opencv.hpp>
// using namespace cv;

// test
using namespace std;

CLogFile logfile;

// pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t MutexLockGuard::lock = PTHREAD_MUTEX_INITIALIZER;

// 静态成员类外初始化
pthread_mutex_t MimeType::lock = PTHREAD_MUTEX_INITIALIZER;
std::unordered_map<std::string, std::string> MimeType::mime;

// 定义请求的格式
std::string MimeType::getMime(const std::string &suffix)
{
    if (mime.size() == 0)
    {
        pthread_mutex_lock(&lock);
        if (mime.size() == 0)
        {
            mime[".html"] = "text/html";
            mime[".avi"] = "video/x-msvideo";
            mime[".bmp"] = "image/bmp";
            mime[".c"] = "text/plain";
            mime[".doc"] = "application/msword";
            mime[".gif"] = "image/gif";
            mime[".gz"] = "application/x-gzip";
            mime[".htm"] = "text/html";
            mime[".ico"] = "application/x-ico";
            mime[".jpg"] = "image/jpeg";
            mime[".png"] = "image/png";
            mime[".txt"] = "text/plain";
            mime[".mp3"] = "audio/mp3";
            mime["default"] = "text/html";
        }
        pthread_mutex_unlock(&lock);
    }
    if (mime.find(suffix) == mime.end())
        return mime["default"];
    else
        return mime[suffix];
}

// 保存过期时间列表为小根堆
priority_queue<shared_ptr<mytimer>, std::deque<shared_ptr<mytimer>>, timerCmp> myTimerQueue;

// 监听描述符构造函数
requestData::requestData():
    now_read_pos(0), 
    state(STATE_PARSE_URI), 
    h_state(h_start),
    keep_alive(true), 
    againTimes(0)
{}

// 连接描述符构造函数
requestData::requestData(int _epollfd, int _fd, std::string addr_IP, std::string _path): 
    now_read_pos(0), 
    state(STATE_PARSE_URI),
    h_state(h_start),
    keep_alive(true), 
    againTimes(0), 
    path(_path),
    fd(_fd), 
    IP(addr_IP), 
    epollfd(_epollfd)
{}

// 析构函数
requestData::~requestData()
{
    // cout << "~requestData()" << endl;
    // struct epoll_event ev;
    // // 超时的一定都是读请求，没有"被动"写。
    // ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
    // ev.data.ptr = (void *)this;
    // epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, &ev);
    // if (timer != NULL)
    // {
    //     timer->clearReq();
    //     timer = NULL;
    // }
    close(fd);
}

// 为requestData添加计时
void requestData::addTimer(shared_ptr<mytimer> mtimer)
{
    // if (timer == NULL)
    timer = mtimer;
}

// 获取fd
int requestData::getFd()
{
    return fd;
}

// 设置fd
void requestData::setFd(int _fd)
{
    fd = _fd;
}

// 重置requestData
void requestData::reset()
{
    againTimes = 0;
    content.clear();
    file_name.clear();
    path.clear();
    now_read_pos = 0;
    state = STATE_PARSE_URI;
    h_state = h_start;
    headers.clear();
    keep_alive = true;
    // weak_ptr：use_count()返回与weak_ptr共享的shared_ptr数量
    // expired()若use_count()为0返回true，对象已死
    // lock()若expired()为true返回一个空的shared_ptr，否则返回一个指向对象的shared_ptr
    if (timer.lock())
    {
        shared_ptr<mytimer> my_timer(timer.lock());
        my_timer->clearReq();
        timer.reset();
    }
}

void requestData::seperateTimer()
{
    if (timer.lock())
    {
        shared_ptr<mytimer> my_timer(timer.lock());
        my_timer->clearReq();
        // timer制空
        timer.reset();
    }
}

// 事件处理函数
void requestData::handleRequest()
{
    char buff[MAX_BUFF];
    bool isError = false;

    // 此处循环保证边沿触发一次性读取完，continue来保证读取完
    while (true)
    {
        int read_num = readn(fd, buff, MAX_BUFF);
        if (read_num < 0)
        {
            // perror("1");
            isError = true;
            break;
        }
        else if (read_num == 0)
        {
            // 非阻塞模式第一次没有读到，或者对端连接已断开会返回0
            // 有请求出现但是读不到数据，可能是Request Aborted，或者来自网络的数据没有达到等原因
            perror("read_num == 0");
            // 非阻塞模式第一次没有读到
            if (errno == EAGAIN)
            {
                if (againTimes > AGAIN_MAX_TIMES)
                    isError = true;
                else
                    ++againTimes;
            }
            else if (errno != 0)
                isError = true;
            break;
        }
        string now_read(buff, buff + read_num);
        content += now_read;

        if (state == STATE_PARSE_URI)
        {
            int flag = this->parse_URI();
            if (flag == PARSE_URI_AGAIN)
            {
                continue;
            }
            else if (flag == PARSE_URI_ERROR)
            {
                // perror("2");
                isError = true;
                break;
            }
        }
        if (state == STATE_PARSE_HEADERS)
        {
            int flag = this->parse_Headers();
            if (flag == PARSE_HEADER_AGAIN)
            {
                continue;
            }
            else if (flag == PARSE_HEADER_ERROR)
            {
                // perror("3");
                isError = true;
                break;
            }
            // 一般POST请求在空行后带请求数据；GET请求不带请求数据，携带在URI中
            if (method == METHOD_POST)
            {
                state = STATE_RECV_BODY;
            }
            else
            {
                state = STATE_ANALYSIS;
            }
        }
        // POST请求
        if (state == STATE_RECV_BODY)
        {
            int content_length = -1;
            if (headers.find("Content-length") != headers.end())
            {
                content_length = stoi(headers["Content-length"]);
            }
            else
            {
                isError = true;
                break;
            }
            // 数据部分本次未读完
            if (content.size() < content_length)
                continue;
            state = STATE_ANALYSIS;
        }
        if (state == STATE_ANALYSIS)
        {
            int flag = this->analysisRequest();
            if (flag < 0)
            {
                isError = true;
                break;
            }
            else if (flag == ANALYSIS_SUCCESS)
            {

                state = STATE_FINISH;
                break;
            }
            else
            {
                isError = true;
                break;
            }
        }
    }

    if (isError)
    {
        MutexLockGuard_LOG();
        logfile.Write("客户端(%s)HTTP解析错误!\n", IP.c_str());
        // delete this;
        Epoll::epoll_del(fd, EPOLLIN | EPOLLET | EPOLLONESHOT);
        return;
    }
    // 加入epoll继续
    if (state == STATE_FINISH)
    {
        if (keep_alive)
        {
            MutexLockGuard_LOG();
            logfile.Write("客户端(%s)HTTP解析成功!\n", IP.c_str());
            this->reset();
        }
        else
        {
            // delete this;
            Epoll::epoll_del(fd, EPOLLIN | EPOLLET | EPOLLONESHOT);
            return;
        }
    }
    // 一定要先加时间信息，否则可能会出现刚加进去，下个in触发来了，然后分离失败后，又加入队列，最后超时被删，
    // 然后正在线程中进行的任务出错，double free错误

    // 新增时间信息
    // pthread_mutex_lock(&qlock);
    // 使用shared_from_this()函数，不是用this，因为这样会造成2个非共享的share_ptr指向同一个对象，
    // 未增加引用计数导对象被析构两次
    shared_ptr<mytimer> mtimer(new mytimer(shared_from_this(), EPOLL_WAIT_TIME));
    this->addTimer(mtimer);
    {
        MutexLockGuard();
        myTimerQueue.push(mtimer);
    }
    // pthread_mutex_unlock(&qlock);

    __uint32_t _epo_event = EPOLLIN | EPOLLET | EPOLLONESHOT;
    int ret = Epoll::epoll_mod(fd, shared_from_this(), _epo_event);
    if (ret < 0)
    {
        // 返回错误处理
        MutexLockGuard();
        logfile.Write("epoll mod failed\n");
        // delete this;
        return;
    }
}

// 解析URI：确定属性filename,HTTPversion,method
int requestData::parse_URI()
{
    // str引用content，改变str就是改变content
    string &str = content;
    // 读到完整的请求行再开始解析请求
    int pos = str.find('\r', now_read_pos);
    // 没有找到说明此次读取请求头包含不完全
    if (pos < 0)
    {
        return PARSE_URI_AGAIN;
    }
    // 去掉请求行所占的空间，节省空间
    string request_line = str.substr(0, pos);
    if (str.size() > pos + 1)
        str = str.substr(pos + 1);
    else
        str.clear();
    // Method
    pos = request_line.find("GET");
    if (pos < 0)
    {
        pos = request_line.find("POST");
        if (pos < 0)
        {
            return PARSE_URI_ERROR;
        }
        else
        {
            method = METHOD_POST;
        }
    }
    else
    {
        method = METHOD_GET;
    }
    // printf("method = %d\n", method);
    //  filename
    pos = request_line.find("/", pos);
    if (pos < 0)
    {
        return PARSE_URI_ERROR;
    }
    else
    {
        int _pos = request_line.find(' ', pos);
        if (_pos < 0)
            return PARSE_URI_ERROR;
        else
        {
            if (_pos - pos > 1)
            {
                file_name = request_line.substr(pos, _pos - pos);
                int __pos = file_name.find('?');
                if (__pos >= 0)
                {
                    file_name = file_name.substr(0, __pos);
                }
            }

            else
                file_name = "../doc/index.html";
        }
        pos = _pos;
    }
    // cout << "file_name: ----------" << file_name << endl;
    //  HTTP 版本号
    pos = request_line.find("/", pos);
    if (pos < 0)
    {
        return PARSE_URI_ERROR;
    }
    else
    {
        if (request_line.size() - pos <= 3)
        {
            return PARSE_URI_ERROR;
        }
        else
        {
            string ver = request_line.substr(pos + 1, 3);
            if (ver == "1.0")
                HTTPversion = HTTP_10;
            else if (ver == "1.1")
                HTTPversion = HTTP_11;
            else
                return PARSE_URI_ERROR;
        }
    }
    state = STATE_PARSE_HEADERS;
    return PARSE_URI_SUCCESS;
}

// 解析请求头
int requestData::parse_Headers()
{
    string &str = content;
    int key_start = -1, key_end = -1, value_start = -1, value_end = -1;
    int now_read_line_begin = 0;
    bool notFinish = true;
    for (int i = 0; i < str.size() && notFinish; ++i)
    {
        switch (h_state)
        {
        case h_start:
        {
            if (str[i] == '\n' || str[i] == '\r')
                break;
            h_state = h_key;
            key_start = i;
            now_read_line_begin = i;
            break;
        }
        case h_key:
        {
            if (str[i] == ':')
            {
                key_end = i;
                if (key_end - key_start <= 0)
                    return PARSE_HEADER_ERROR;
                h_state = h_colon;
            }
            else if (str[i] == '\n' || str[i] == '\r')
                return PARSE_HEADER_ERROR;
            break;
        }
        case h_colon:
        {
            if (str[i] == ' ')
            {
                h_state = h_spaces_after_colon;
            }
            else
                return PARSE_HEADER_ERROR;
            break;
        }
        case h_spaces_after_colon:
        {
            h_state = h_value;
            value_start = i;
            break;
        }
        case h_value:
        {
            if (str[i] == '\r')
            {
                h_state = h_CR;
                value_end = i;
                if (value_end - value_start <= 0)
                    return PARSE_HEADER_ERROR;
            }
            else if (i - value_start > 255)
                return PARSE_HEADER_ERROR;
            break;
        }
        case h_CR:
        {
            if (str[i] == '\n')
            {
                h_state = h_LF;
                string key(str.begin() + key_start, str.begin() + key_end);
                string value(str.begin() + value_start, str.begin() + value_end);
                headers[key] = value;
                now_read_line_begin = i;
            }
            else
                return PARSE_HEADER_ERROR;
            break;
        }
        case h_LF:
        {
            if (str[i] == '\r')
            {
                h_state = h_end_CR;
            }
            else
            {
                key_start = i;
                h_state = h_key;
            }
            break;
        }
        case h_end_CR:
        {
            if (str[i] == '\n')
            {
                h_state = h_end_LF;
            }
            else
                return PARSE_HEADER_ERROR;
            break;
        }
        // 要么后面还有请求体，要么刚好读完空行\n
        case h_end_LF:
        {
            notFinish = false;
            key_start = i;
            now_read_line_begin = i;
            break;
        }
        }
    }
    if (h_state == h_end_LF)
    {
        str = str.substr(now_read_line_begin);
        return PARSE_HEADER_SUCCESS;
    }
    // 没读完头
    str = str.substr(now_read_line_begin);
    return PARSE_HEADER_AGAIN;
}

// HTTP响应
int requestData::analysisRequest()
{
    if (method == METHOD_POST)
    {
        // get content
        char header[MAX_BUFF];
        sprintf(header, "HTTP/1.1 %d %s\r\n", 200, "OK");
        // 如果收到的 Connection: keep-alive
        // 浏览器发送的HTTP报文默认是keep-alive，所以可能会省略Connection: keep-alive，所以构造函数默认keep-alive为true
        if (headers.find("Connection") != headers.end())
        {
            if (headers["Connection"] == "keep-alive") {
                sprintf(header, "%sConnection: keep-alive\r\n", header);
                sprintf(header, "%sKeep-Alive: timeout=%d\r\n", header, EPOLL_WAIT_TIME);
            }
            else {
                keep_alive = false;
                sprintf(header, "%sConnection: close\r\n", header);
            }
        }
        // cout << "content=" << content << endl;
        //  test char*
        char *send_content = "I have receiced this.";

        sprintf(header, "%sContent-length: %zu\r\n", header, strlen(send_content));
        sprintf(header, "%s\r\n", header);
        size_t send_len = (size_t)writen(fd, header, strlen(header));
        if (send_len != strlen(header))
        {
            // perror("Send header failed");
            return ANALYSIS_ERROR;
        }

        send_len = (size_t)writen(fd, send_content, strlen(send_content));
        if (send_len != strlen(send_content))
        {
            // perror("Send content failed");
            return ANALYSIS_ERROR;
        }
        // cout << "content size ==" << content.size() << endl;
        // 保存发送方数据到vector
        // vector<char> data(content.begin(), content.end());
        // // opencv函数：将vector中内容读到Mat矩阵中
        // Mat test = imdecode(data, CV_LOAD_IMAGE_ANYDEPTH | CV_LOAD_IMAGE_ANYCOLOR);
        // // 保存到指定的文件receive.bmp
        // imwrite("receive.bmp", test);
        return ANALYSIS_SUCCESS;
    }
    else if (method == METHOD_GET)
    {
        char header[MAX_BUFF];
        sprintf(header, "HTTP/1.1 %d %s\r\n", 200, "OK");
        // 如果收到的 Connection: keep-alive
        // 浏览器发送的HTTP报文默认是keep-alive，所以可能会省略Connection: keep-alive，所以构造函数默认keep-alive为true
        if (headers.find("Connection") != headers.end())
        {
            if (headers["Connection"] == "keep-alive") {
                sprintf(header, "%sConnection: keep-alive\r\n", header);
                sprintf(header, "%sKeep-Alive: timeout=%d\r\n", header, EPOLL_WAIT_TIME);
            }
            else {
                keep_alive = false;
                sprintf(header, "%sConnection: close\r\n", header);
            }
        }
        int dot_pos = file_name.find('.');
        const char *filetype;
        if (dot_pos < 0)
            filetype = MimeType::getMime("default").c_str();
        else
            filetype = MimeType::getMime(file_name.substr(dot_pos)).c_str();
        // 此结构体描述文件的信息
        struct stat sbuf;
        // stat函数获取文件信息保存到sbuf中
        if (stat(file_name.c_str(), &sbuf) < 0)
        {
            handleError(fd, 404, "Not Found!");
            return ANALYSIS_ERROR;
        }

        sprintf(header, "%sContent-type: %s\r\n", header, filetype);
        // 通过Content-length返回文件大小
        sprintf(header, "%sContent-length: %ld\r\n", header, sbuf.st_size);

        sprintf(header, "%s\r\n", header);
        size_t send_len = (size_t)writen(fd, header, strlen(header));
        if (send_len != strlen(header))
        {
            // perror("Send header failed");
            return ANALYSIS_ERROR;
        }
        // 打开文件，O_RDONLY只读打开
        int src_fd = open(file_name.c_str(), O_RDONLY, 0);
        // mmap函数类似于read与write，只不过减少了用户态到核心态的拷贝，直接映射到核心态
        // 映射区域起始地址NULL(自动分配)；大小(一般为4KB整数倍)；映射区域自己权限(PROT_READ)可读权限;
        // 映射标志位(MAP_PRIVATE)对映射区的写入操作只反映到缓存区中不会真正写入到文件；文件描述符；偏移量
        // 返回映射起始地址
        char *src_addr = static_cast<char *>(mmap(NULL, sbuf.st_size, PROT_READ, MAP_PRIVATE, src_fd, 0));
        close(src_fd);

        // 发送文件并校验完整性
        send_len = writen(fd, src_addr, sbuf.st_size);
        if (send_len != sbuf.st_size)
        {
            // perror("Send file failed");
            return ANALYSIS_ERROR;
        }
        // 删除映射
        munmap(src_addr, sbuf.st_size);
        return ANALYSIS_SUCCESS;
    }
    else
        return ANALYSIS_ERROR;
}

// 处理文件GET：URI请求错误
void requestData::handleError(int fd, int err_num, string short_msg)
{
    short_msg = " " + short_msg;
    char send_buff[MAX_BUFF];
    string body_buff, header_buff;
    body_buff += "<html><title>TKeed Error</title>";
    body_buff += "<body bgcolor=\"ffffff\">";
    body_buff += to_string(err_num) + short_msg;
    body_buff += "<hr><em> WH's Web Server</em>\n</body></html>";

    header_buff += "HTTP/1.1 " + to_string(err_num) + short_msg + "\r\n";
    header_buff += "Content-type: text/html\r\n";
    header_buff += "Connection: close\r\n";
    header_buff += "Content-length: " + to_string(body_buff.size()) + "\r\n";
    header_buff += "\r\n";
    sprintf(send_buff, "%s", header_buff.c_str());
    writen(fd, send_buff, strlen(send_buff));
    sprintf(send_buff, "%s", body_buff.c_str());
    writen(fd, send_buff, strlen(send_buff));
}

// 为每一个连接添加一个过期时间
mytimer::mytimer(shared_ptr<requestData> _request_data, int timeout) : deleted(false), request_data(_request_data)
{
    // cout << "mytimer()" << endl;
    struct timeval now;
    gettimeofday(&now, NULL);
    // 以毫秒计
    expired_time = ((now.tv_sec * 1000) + (now.tv_usec / 1000)) + timeout;
}

mytimer::~mytimer()
{
    // cout << "~mytimer()" << endl;
    if (request_data != NULL)
    {
        // // cout << "request_data=" << request_data << endl;
        // delete request_data;
        // request_data = NULL;
        Epoll::epoll_del(request_data->getFd(), EPOLLIN | EPOLLET | EPOLLONESHOT);
    }
}

void mytimer::update(int timeout)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    expired_time = ((now.tv_sec * 1000) + (now.tv_usec / 1000)) + timeout;
}

// 是否有效(没有超时)
bool mytimer::isvalid()
{
    struct timeval now;
    gettimeofday(&now, NULL);
    size_t temp = ((now.tv_sec * 1000) + (now.tv_usec / 1000));
    if (temp < expired_time)
    {
        return true;
    }
    else
    {
        this->setDeleted();
        return false;
    }
}

void mytimer::clearReq()
{
    // 给智能指针制空值
    request_data.reset();
    this->setDeleted();
}

void mytimer::setDeleted()
{
    deleted = true;
}

// 是否设置为删除
bool mytimer::isDeleted() const
{
    return deleted;
}

size_t mytimer::getExpTime() const
{
    return expired_time;
}

// 优先级队列的比较规则
bool timerCmp::operator()(shared_ptr<mytimer> &a, shared_ptr<mytimer> &b) const
{
    return a->getExpTime() > b->getExpTime();
}

// 在构造函数中构造锁
MutexLockGuard::MutexLockGuard()
{
    pthread_mutex_lock(&lock);
}

// 在析构函数中释放锁
MutexLockGuard::~MutexLockGuard()
{
    pthread_mutex_unlock(&lock);
}