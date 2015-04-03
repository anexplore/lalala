#ifndef __PROXY_SCANNER_HPP
#define __PROXY_SCANNER_HPP
#include <string>
#include <vector>
#include <queue>
#include <map>
#include <list>
#include <boost/shared_ptr.hpp>
#include <boost/function.hpp>
#include <boost/bind.hpp>
#include <httpparser/TUtility.hpp>
#include <boost/unordered_set.hpp>
#include "lock/lock.hpp"
#include "utility/net_utility.h"
#include "fetcher/Fetcher.hpp"
#include "shm/ShareHashSet.hpp" 
#include "utility/murmur_hash.h"
#include "jsoncpp/include/json/json.h"
 
struct Proxy
{
    static const unsigned PROXY_SIZE = 50; 
    enum State
    {
        SCAN_IDLE,
        SCAN_HTTP,
        SCAN_HTTPS,
    } state_;
    char ip_[16];
    uint16_t port_;
    unsigned request_cnt_;
    time_t request_time_;
    unsigned char data_changed_:1;
    unsigned char http_enable_ :1;
    unsigned char https_enable_:1;
    unsigned char is_foreign   :1;
    char fill_buf_[PROXY_SIZE - sizeof(state_) - 
        sizeof(ip_) - sizeof(port_) - 
        sizeof(request_cnt_) - sizeof(request_time_) - 1];

    Proxy()
    {
    }

    Proxy(std::string ip, uint16_t port):
        state_(SCAN_IDLE), port_(port), 
        request_cnt_(0),  request_time_(0), 
        data_changed_(0), http_enable_(0), 
        https_enable_(0), is_foreign(0)
    {
        memset(ip_, 0, sizeof(ip_));
        strcpy(ip_, ip.c_str());
        port_ = port;
    }
    ~Proxy()
    {
    }
    std::string ToString() const
    {
        char buf[100];
        snprintf(buf, 100, "%s:%hu", ip_, port_);
        return buf;
    }
    Json::Value ToJson() const
    {
        Json::Value json_val;
        json_val["addr"] = ToString();
        if(https_enable_)
            json_val["https"] = "1";
        char cnt_str[10];
        snprintf(cnt_str, 10, "%u", request_cnt_);
        json_val["avail"] = cnt_str;
        return json_val;
    }
    bool operator == (const Proxy& other)
    {
        return strncmp(ip_, other.ip_, 16) == 0 
            && port_ != other.port_;
    }
    struct sockaddr * AcquireSockAddr() const
    {
        return get_sockaddr_in(ip_, port_);
    }
} __attribute__((packed));

struct HashFunctor
{
    uint64_t operator () (const Proxy& proxy)
    {
        uint64_t val = 0;
        std::string proxy_str = proxy.ToString();
        MurmurHash_x64_64(proxy_str.c_str(), proxy_str.size(), &val);
        return val; 
    }
};

typedef ShareHashSet<Proxy, HashFunctor> ProxySet; 

class ProxyScanner: protected IMessageEvents
{
    static const unsigned DEFAULT_SCAN_INTERVAL_SEC  = 0;
    static const unsigned DEFAULT_VALIDATE_INTERVAL_SEC= 600;

    inline time_t __remain_time(time_t last_time, time_t cur_time, time_t interval); 
    void __load_offset_file();
    void __save_offset_file();     

protected:
    virtual struct RequestData* CreateRequestData(void *);
    virtual void FreeRequestData(struct RequestData *);
    virtual IFetchMessage* CreateFetchResponse(const FetchAddress&, void *);
    virtual void FreeFetchMessage(IFetchMessage *);

    virtual void GetScanProxyRequest(int, std::vector<RawFetcherRequest>&);
    //virtual bool GetValidateProxyRequest(Connection* &, void* &);
    virtual void HandleProxyDelete(Proxy * proxy);
    virtual void HandleProxyUpdate(Proxy* proxy);
    virtual void ProcessResult(const RawFetcherResult&);
    RawFetcherRequest CreateFetcherRequest(Proxy* proxy);
 
public:
    ProxyScanner(ProxySet * proxy_set,
        Fetcher::Params fetch_params,
        const char* eth_name = NULL);
    virtual ~ProxyScanner(){}
    void SetHttpTryUrl(std::string try_url, size_t page_size);
    void SetHttpsTryUrl(std::string try_url, size_t page_size);
    void SetScanPort(const std::vector<uint16_t>& scan_port);
    void SetScanRange(unsigned low_range[4], unsigned high_range[4]); 
    void SetValidateIntervalSeconds(time_t validate_interval_sec);
    void SetScanIntervalSeconds(time_t scan_interval_sec);
    void Stop();
    void RequestGenerator(int n, std::vector<RawFetcherRequest>& req_vec);
    void Start();

protected:
    Fetcher::Params params_;
    const char* offset_file_;
    time_t offset_save_interval_;
    time_t offset_save_time_;
    Fetcher::Params fetcher_params_;
    URI*    try_http_uri_;
    size_t  try_http_size_;
    URI*    try_https_uri_;
    size_t  try_https_size_;
    boost::shared_ptr<ThreadingFetcher> fetcher_;
    std::vector<uint16_t> scan_port_;
    unsigned offset_[4];
    unsigned port_idx_;
    unsigned low_range_[4];
    unsigned high_range_[4];
    sockaddr* local_addr_;
    time_t validate_time_;
    time_t validate_interval_;
    time_t scan_time_;
    time_t scan_interval_; 
    ProxySet *proxy_set_;
    bool stopped_;
    std::queue<RawFetcherRequest> req_queue_;
};

#endif 
