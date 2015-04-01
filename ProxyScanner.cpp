#include "ProxyScanner.hpp"
#include "fetcher/Fetcher.hpp"
#include "utility/net_utility.h"
#include "boost/bind.hpp"
#include <httpparser/TUtility.hpp>
#include "httpparser/URI.hpp"
#include "httpparser/HttpFetchProtocal.hpp"
#include "log/log.h"

struct Proxy
{
    enum State
    {
        SCAN_IDLE,
        SCAN_HTTP,
        SCAN_HTTPS,
    } state_;
    std::string ip_;
    uint16_t port_;
    unsigned char data_changed_:1;
    unsigned char http_enable_ :1;
    unsigned char https_enable_:1;
    unsigned char is_foreign   :1;
    unsigned request_cnt_;
    time_t request_time_;

    Proxy(std::string ip, uint16_t port):
        state_(SCAN_IDLE), ip_(ip), 
        port_(port), data_changed_(0),
        http_enable_(0), https_enable_(0),
        request_cnt_(0), request_time_(0) 
    {
    }
    ~Proxy()
    {
    }
    std::string ToString() const
    {
        char buf[1024];
        snprintf(buf, 1024, "%s:%hu", ip_.c_str(), port_);
        return buf;
    }
    struct sockaddr * AcquireSockAddr() const
    {
        return get_sockaddr_in(ip_.c_str(), port_);
        //struct sockaddr * dst_addr = get_sockaddr_in(ip_.c_str(), port_);
        //char tmp_buf[100] = {0};
        //uint16_t tmp_port = 0;
        //get_addr_string(dst_addr_, tmp_buf, 100, tmp_port);
        //LOG_INFO("1=== request %s %hu %p\n", tmp_buf, tmp_port, dst_addr_);
    }
};

inline time_t current_time_ms()
{
    timeval tv; 
    gettimeofday(&tv, NULL);
    return (tv.tv_sec*1000000 + tv.tv_usec) / 1000;
}

time_t ProxyScanner::__remain_time(time_t last_time, time_t cur_time, time_t interval)
{ 
    if(last_time + interval > cur_time)
        return last_time + interval - cur_time;
    return 0;
}

void ProxyScanner::__load_offset_file()
{
    if(!offset_file_)
        return;
    FILE *fid = fopen(offset_file_, "r");
    int ret = 0;
    if(fid)
    {
        ret = fscanf(fid, "%d %d %d %d", offset_, offset_ + 1,
            offset_ + 2, offset_ + 3);
        fclose(fid);
    }
    if(ret != 4)
        memcpy(offset_, low_range_, sizeof(low_range_));
}

void ProxyScanner::__save_offset_file()
{
    if(!offset_file_)
        return;
    FILE *fid = fopen(offset_file_, "w");
    assert(fid);
    fprintf(fid, "%d %d %d %d", offset_[0], offset_[1],
        offset_[2], offset_[3]);
    fclose(fid);
    offset_save_time_ = current_time_ms();
}

ProxyScanner::ProxyScanner(Fetcher::Params fetch_params, 
    const char* offset_file,  time_t offset_save_sec, 
    const char* eth_name):
    offset_file_(NULL), 
    offset_save_interval_(offset_save_sec*1000),
    offset_save_time_(0), port_idx_(0), 
    local_addr_(NULL), validate_time_(0),
    validate_interval_(DEFAULT_VALIDATE_INTERVAL_SEC*1000),
    scan_time_(0), 
    scan_interval_(DEFAULT_SCAN_INTERVAL_SEC*1000),
    stopped_(false)
{
    if(offset_file)
        offset_file_ = strdup(offset_file);
    low_range_[0] = low_range_[1] = 0;
    low_range_[2] = low_range_[3] = 0;
    high_range_[0] = high_range_[1] = 255;
    high_range_[2] = high_range_[3] = 255;
    __load_offset_file();
    uint16_t scan_ports[] = {80, 8080, 3128, 8118, 808};
    scan_port_.assign(scan_ports, scan_ports + sizeof(scan_ports)/sizeof(*scan_ports));
    memcpy(&params_, &fetch_params, sizeof(fetch_params));
    assert(offset_file);
    SetHttpTryUrl("http://www.baidu.com/img/baidu_jgylogo3.gif", 705);
    SetHttpsTryUrl("https://www.baidu.com/img/baidu_jgylogo3.gif", 705);
    fetcher_.reset(new ThreadingFetcher(this));
    fetcher_->SetResultCallback(boost::bind(&ProxyScanner::ProcessResult, this, _1));
    ThreadingFetcher::RequestGenerator req_generator =
        boost::bind(&ProxyScanner::RequestGenerator, this, _1, _2);
    fetcher_->SetRequestGenerator(req_generator);
    if(eth_name)
    {
        local_addr_ = (struct sockaddr*)malloc(sizeof(struct sockaddr));
        memset(local_addr_, 0, sizeof(struct sockaddr)); 
        struct in_addr * p_addr = &((struct sockaddr_in*)local_addr_)->sin_addr;
        assert(getifaddr(AF_INET, 0, eth_name, p_addr) == 0);
        ((struct sockaddr_in*)local_addr_)->sin_family = AF_INET; 
    }
    update_itr_ = proxy_set_.end();
}

void ProxyScanner::SetHttpTryUrl(std::string try_url, size_t page_size)
{
    try_http_uri_ = new URI();
    if(!UriParse(try_url.c_str(), try_url.size(), *try_http_uri_)
        || !HttpUriNormalize(*try_http_uri_))
    {
        assert(false);
    }
    try_http_size_ = page_size;
}

void ProxyScanner::SetHttpsTryUrl(std::string try_url, size_t page_size)
{
    try_https_uri_ = new URI();
    if(!UriParse(try_url.c_str(), try_url.size(), *try_https_uri_)
        || !HttpUriNormalize(*try_https_uri_))
    {
        assert(false);
    }
    try_https_size_ = page_size;
}

void ProxyScanner::SetScanPort(const std::vector<uint16_t>& scan_port)
{
    scan_port_ = scan_port;
}

RawFetcherRequest ProxyScanner::CreateFetcherRequest(Proxy* proxy)
{
    assert(proxy->state_ != Proxy::SCAN_IDLE);
    proxy->request_time_ = current_time_ms();
    URI * uri = NULL;
    if(proxy->state_ == Proxy::SCAN_HTTPS)
        uri = try_https_uri_;
    else
        uri = try_http_uri_;
    FetchAddress fetch_address;
    fetch_address.remote_addr = proxy->AcquireSockAddr();
    fetch_address.remote_addrlen = sizeof(sockaddr);
    fetch_address.local_addr  = local_addr_;
    fetch_address.local_addrlen = local_addr_ ?sizeof(sockaddr):0;
    RawFetcherRequest request;
    request.conn = fetcher_->CreateConnection(
		str2protocal(uri->Scheme()), AF_INET,
		SOCK_STREAM, 0, fetch_address);
    free(fetch_address.remote_addr);
    //char tmp_buf[100] = {0};
    //uint16_t port = 0;
    //assert(get_addr_string(fetch_address.remote_addr, tmp_buf, 100, port)); 
    //LOG_INFO("request %s %hu %p\n", tmp_buf, port, fetch_address.remote_addr);
    request.context = (void*)proxy;
    return request;
}

void ProxyScanner::GetScanProxyRequest(
    int n, std::vector<RawFetcherRequest>& req_vec)
{
    time_t cur_time = current_time_ms();
    if(offset_[0]  == low_range_[0] && 
        offset_[1] == low_range_[1] &&
        offset_[2] == low_range_[2] &&
        offset_[3] == low_range_[3] &&
        port_idx_ == 0)
    {
        time_t remain = __remain_time(scan_time_, cur_time, scan_interval_);
        if(remain > 0)
        {
            //LOG_INFO("remain %zd\n", remain);
            return;
        }
        LOG_INFO("####### Start Scan ########\n");
        scan_time_ = cur_time;
    }

    for(int i = 0 ; i < n; i++)
    {
        //进位
        if(port_idx_ == scan_port_.size())
        {
            port_idx_ = 0;
            offset_[3]++;
        }
        for(int i = 3; i >= 1; --i)
        {
            if(offset_[i] > high_range_[i])
            {
                offset_[i] = low_range_[i];
                offset_[i-1]++;
            }
        }
        //跳过内网地址
        if(offset_[0] == 10)
        {
            offset_[0] = 11;
            offset_[1] = low_range_[1];
            offset_[2] = low_range_[2];
            offset_[3] = low_range_[3];
        }
        if(offset_[0] == 172 && offset_[1] >= 16 && offset_[1] <= 31)
        {
            offset_[1] = 32;
            offset_[2] = low_range_[2];
            offset_[3] = low_range_[3];
        }
        if(offset_[0] == 192 && offset_[1] == 168)
        {
            offset_[1] = 169;
            offset_[2] = low_range_[2];
            offset_[3] = low_range_[3];
        }
        //检查是否结束
        if(offset_[0] > high_range_[0])
        {
            LOG_INFO("####### End Scan ########\n");
            memcpy(offset_, low_range_, sizeof(low_range_));
            port_idx_ = 0;
            return;
        }
        uint16_t port = scan_port_[port_idx_];
        char ip_str[200];
        snprintf(ip_str, 200, "%d.%d.%d.%d", offset_[0], 
                offset_[1], offset_[2], offset_[3]);
        Proxy * proxy = new Proxy(ip_str, port);
        proxy->state_ = Proxy::SCAN_HTTP;
        ++proxy->request_cnt_; 
        req_vec.push_back(CreateFetcherRequest(proxy));
        ++port_idx_;
    }
}

/*
bool ProxyScanner::GetValidateProxyRequest(Connection* & connection, 
    void* & contex)
{
    time_t cur_time = current_time_ms();
    if(update_itr_ == proxy_set_.end())
    {
        if(__remain_time(validate_time_, cur_time, validate_interval_))
            return false;
    }
    update_itr_ = proxy_set_.begin();
    validate_time_ = cur_time;
    LOG_INFO("####### Start Validate ########\n");
    for(update_itr_ = proxy_set_.begin(); update_itr_ != proxy_set_.end(); ++update_itr_)
    {
        if((*update_itr_)->request_time_ + validate_interval_ <= cur_time)
        {
            (*update_itr_)->state_ = Proxy::SCAN_HTTP;
            ++((*update_itr_)->request_cnt_);
            RawFetcherResult(); 
            PutRequest(*it);
        }
    }
    LOG_INFO("####### End Validate ########\n");
    return __remain_time(validate_time_, current_time_ms(), validate_interval_);
}
*/

void ProxyScanner::HandleProxyDelete(Proxy * proxy)
{
    if(proxy->request_cnt_ > 1)
    {
        proxy_set_.erase(proxy); 
    }
    delete proxy;
}

void ProxyScanner::HandleProxyUpdate(Proxy* proxy)
{
    LOG_INFO("===== %s %u %u %u\n", proxy->ip_.c_str(), proxy->port_,
        proxy->http_enable_, proxy->https_enable_);
    if(proxy->request_cnt_ == 1)
    {
        proxy_set_.insert(proxy); 
    }
}

void ProxyScanner::ProcessResult(const RawFetcherResult& fetch_result)
{
    HttpFetcherResponse *resp = (HttpFetcherResponse *)fetch_result.message;
    Proxy* proxy = (Proxy*)fetch_result.context;
    assert(proxy->state_ != Proxy::SCAN_IDLE);
    fetcher_->CloseConnection(fetch_result.conn);
    fetcher_->FreeConnection(fetch_result.conn);

    if(rand() % 1000 == 0)
    {
        LOG_INFO("errno: %s %s %hu\n", strerror(fetch_result.err_num), 
            proxy->ip_.c_str(), proxy->port_);
        if(__remain_time(offset_save_time_, current_time_ms(), offset_save_interval_))
            __save_offset_file();
    }

    //if(fetch_result.err_num == 0)
    //    LOG_INFO("##### %s %u %zd\n", proxy->ip_.c_str(), proxy->port_,  resp->Body.size());

    //http result
    if(proxy->state_ == Proxy::SCAN_HTTP)
    {
        proxy->data_changed_ = 0;
        if(fetch_result.err_num == 0 && resp->Body.size() == try_http_size_)
        {
            if(!proxy->http_enable_)
                proxy->data_changed_ = 1;
            proxy->http_enable_ = 1;
            proxy->state_ = Proxy::SCAN_HTTPS;
            req_queue_.push(CreateFetcherRequest(proxy));
        }
        else
            HandleProxyDelete(proxy);
        return;
    }

    if(proxy->state_ == Proxy::SCAN_HTTPS)
    {
        if(fetch_result.err_num == 0 && resp->Body.size() == try_https_size_)
        {
            proxy->https_enable_ = 1;
        }
        proxy->state_ = Proxy::SCAN_IDLE;
        if(!proxy->http_enable_ && !proxy->https_enable_)
        {
            HandleProxyDelete(proxy);
            return;
        }
        HandleProxyUpdate(proxy);
    }
}

void ProxyScanner::SetScanRange(unsigned low_range[4], unsigned high_range[4])
{
    if(high_range_[0] > 255)
        high_range_[0] = 255;
    if(high_range_[1] > 255)
        high_range_[1] = 255;
    if(high_range_[2] > 255)
        high_range_[2] = 255;
    if(high_range_[3] > 255)
        high_range_[3] = 255;
    if(offset_[0] < low_range[0])
        offset_[0] = low_range[0];
    if(offset_[1] < low_range[1])
        offset_[1] = low_range[1];
    if(offset_[2] < low_range[2])
        offset_[2] = low_range[2];
    if(offset_[3] < low_range[3])
        offset_[3] = low_range[3];
    memcpy(low_range_,  low_range,  4*sizeof(low_range[0]));
    memcpy(high_range_, high_range, 4*sizeof(high_range[0]));
}

void ProxyScanner::SetScanIntervalSeconds(time_t scan_interval_sec)
{
    scan_interval_ = scan_interval_sec * 1000; 
}

void ProxyScanner::SetValidateIntervalSeconds(time_t validate_interval_sec)
{
    validate_interval_ = validate_interval_sec * 1000; 
}

void ProxyScanner::RequestGenerator(
    int n, std::vector<RawFetcherRequest>& req_vec)
{
    while(!req_queue_.empty() && (unsigned)n > req_vec.size())
    {
        RawFetcherRequest req = req_queue_.front();
        req_queue_.pop();
        req_vec.push_back(req);
    }
    if((unsigned)n > req_vec.size())
        GetScanProxyRequest((unsigned)n - req_vec.size(), req_vec);
}

void ProxyScanner::Start()
{
    fetcher_->Begin(params_);
    /*
    while(!stopped_)    
    {
        unsigned quota = fetcher_->AvailableQuota();
        for(int i = 0; i < quota; i++)
        {
            Connection * conn = 
            void* contex = NULL;
        }
        ScanProxy();
        ValidateProxy();
        sleep(1);
    }
    */
}

struct RequestData* ProxyScanner::CreateRequestData(void * contex)
{
    Proxy* proxy = (Proxy*)contex;
    HttpFetcherRequest* req = new HttpFetcherRequest();
    req->Clear();
    URI * uri = NULL;
    if(proxy->state_ == Proxy::SCAN_HTTP)
    {
        req->Uri    = try_http_uri_->ToString();
        uri  = try_http_uri_;
    }
    else
    {
        req->Uri    = try_https_uri_->ToString();
        uri  = try_https_uri_;
    }
    req->Method = "GET";
    req->Version= "HTTP/1.1";
    req->Headers.Add("Host", uri->Host());
    req->Headers.Add("Accept", "*/*");
    req->Headers.Add("Accept-Language", "zh-cn");
    req->Headers.Add("Accept-Encoding", "*");
    req->Headers.Add("User-Agent", "Mozilla/5.0 (Windows NT 6.1) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/31.0.1650.63 Sa    fari/537.36 SE 2.X MetaSr 1.0");
    req->Close();
    LOG_INFO("request %s %u.\n", proxy->ip_.c_str(), proxy->port_);
    return req;
}

void ProxyScanner::FreeRequestData(struct RequestData * request_data)
{
    delete (HttpFetcherRequest*)request_data;
}

IFetchMessage* ProxyScanner::CreateFetchResponse(const FetchAddress& address, void * contex)
{
    HttpFetcherResponse* resp = new HttpFetcherResponse(
        address.remote_addr, 
        address.remote_addrlen,
        address.local_addr,
        address.local_addrlen,
        UINT_MAX, UINT_MAX);
    return resp;
}

void ProxyScanner::FreeFetchMessage(IFetchMessage *message)
{
    delete message;
}

void ProxyScanner::Stop()
{
    stopped_ = true;
    fetcher_->End();
}
