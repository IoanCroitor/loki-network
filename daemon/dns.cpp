#include <getopt.h>
#include <signal.h>
#include <stdio.h> /* fprintf, printf */
#ifndef _MSC_VER
#include <unistd.h>
#endif

#include <llarp.h>
#include <llarp/logic.h>
#include "dns_iptracker.hpp"
#include "dnsd.hpp"
#include "ev.hpp"
#include "llarp/net.hpp"
#include "logger.hpp"

#include <algorithm>  // for std::generate_n
#include <thread>     // for multithreaded version
#include <vector>

// keep this once jeff reenables concurrency
#ifdef _MSC_VER
extern "C" void
SetThreadName(DWORD dwThreadID, LPCSTR szThreadName);
#endif

#ifdef _WIN32
#define uint UINT
#endif

#if(__FreeBSD__) || (__OpenBSD__) || (__NetBSD__)
#include <pthread_np.h>
#endif

struct llarp_main *ctx = 0;
bool done              = false;

void
handle_signal(int sig)
{
  printf("got SIGINT\n");
  done = true;
}

std::string const default_chars =
    "abcdefghijklmnaoqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890";

#include <random>

std::string
random_string(size_t len = 15, std::string const &allowed_chars = default_chars)
{
  std::mt19937_64 gen{std::random_device()()};

  std::uniform_int_distribution< size_t > dist{0, allowed_chars.length() - 1};

  std::string ret;

  std::generate_n(std::back_inserter(ret), len,
                  [&] { return allowed_chars[dist(gen)]; });
  return ret;
}

/*
 /// check_online_request hook definition
 typedef void (*check_query_request_hook_func)(struct check_query_request *);

 struct check_query_request
 {
 bool done;
 ///hook
 check_query_request_hook_func hook;
 };

 void
 llarp_dnsd_checkQuery_resolved(struct check_query_request *request)
 {
 }
 */

struct check_query_simple_request
{
  const struct sockaddr *from;
  dnsd_question_request *request;
};

std::map< std::string, struct dnsd_query_hook_response * >
loki_tld_lookup_cache;

void
llarp_dnsd_checkQuery(void *u, uint64_t orig, uint64_t left)
{
  if(left)
    return;
  // struct check_query_request *request = static_cast< struct
  // check_query_request * >(u);
  struct check_query_simple_request *qr =
      static_cast< struct check_query_simple_request * >(u);

  // we do have result
  // if so send that
  // else
  // if we have a free private ip, send that
  struct dns_pointer *free_private = dns_iptracker_get_free();
  if(free_private)
  {
    // make a dnsd_query_hook_response for the cache
    dnsd_query_hook_response *response = new dnsd_query_hook_response;
    response->dontLookUp               = true;
    response->dontSendResponse         = false;
    response->returnThis               = free_private->hostResult;
    llarp::LogInfo("Saving ", qr->request->question.name);
    loki_tld_lookup_cache[qr->request->question.name] = response;
    writesend_dnss_response(free_private->hostResult, qr->from, qr->request);
    return;
  }
  // else
  llarp::LogInfo("Sending cname to delay");
  writecname_dnss_response(
      random_string(32, "abcdefghijklmnopqrstuvwxyz") + "bob.loki", qr->from,
      qr->request);
  delete qr;
}

dnsd_query_hook_response *
hookChecker(std::string name, const struct sockaddr *from,
            struct dnsd_question_request *request)
{
  dnsd_query_hook_response *response = new dnsd_query_hook_response;
  response->dontLookUp               = false;
  response->dontSendResponse         = false;
  response->returnThis               = nullptr;
  llarp::LogInfo("Hooked ", name);
  std::string lName = name;
  std::transform(lName.begin(), lName.end(), lName.begin(), ::tolower);

  // FIXME: probably should just read the last 5 bytes
  if(lName.find(".loki") != std::string::npos)
  {
    llarp::LogInfo("Detect Loki Lookup");
    auto cache_check = loki_tld_lookup_cache.find(lName);
    if(cache_check != loki_tld_lookup_cache.end())
    {
      // was in cache
      llarp::LogInfo("Could reuse address from LokiLookupCache");
      // FIXME: avoid the allocation if you could
      delete response;
      return cache_check->second;
    }
    // check_query_request *query_request = new check_query_request;
    // query_request->hook = &llarp_dnsd_checkQuery_resolved;
    check_query_simple_request *qr = new check_query_simple_request;
    qr->from                       = from;
    qr->request                    = request;
    // nslookup on osx is about 5 sec before a retry
    llarp_logic_call_later(request->context->logic,
                           {5, qr, &llarp_dnsd_checkQuery});
    response->dontSendResponse = true;
  }
  // cast your context->user;
  return response;
}

struct dns_relay_config
{
  std::string upstream_host;
  uint16_t upstream_port;
};

void
dns_iter_config(llarp_config_iterator *itr, const char *section,
                const char *key, const char *val)
{
  dns_relay_config *config = (dns_relay_config *)itr->user;
  if(!strcmp(section, "dns"))
  {
    if(!strcmp(key, "upstream-server"))
    {
      config->upstream_host = strdup(val);
      llarp::LogDebug("Config file setting dns server to ",
                      config->upstream_host);
    }
    if(!strcmp(key, "upstream-port"))
    {
      config->upstream_port = atoi(val);
      llarp::LogDebug("Config file setting dns server port to ",
                      config->upstream_port);
    }
  }
}

int
main(int argc, char *argv[])
{
  int code = 1;
  llarp::LogInfo("Starting up server");

  const char *conffname = handleBaseCmdLineArgs(argc, argv);
  dns_relay_config dnsr_config;
  dnsr_config.upstream_host = "8.8.8.8";
  dnsr_config.upstream_port = 53;
  llarp_config *config_reader;
  llarp_new_config(&config_reader);
  // ctx      = llarp_main_init(conffname, multiThreaded);

  if(llarp_load_config(config_reader, conffname))
  {
    llarp_free_config(&config_reader);
    llarp::LogError("failed to load config file ", conffname);
    return false;
  }
  llarp_config_iterator iter;
  iter.user  = &dnsr_config;
  iter.visit = &dns_iter_config;
  llarp_config_iter(config_reader, &iter);
  llarp::LogInfo("config [", conffname, "] loaded");

  const uint16_t server_port = 1053;

  dns_iptracker_init();

  // llarp::SetLogLevel(llarp::eLogDebug);

  if(1)
  {
    // libev version
    llarp_ev_loop *netloop   = nullptr;
    llarp_threadpool *worker = nullptr;
    llarp_logic *logic       = nullptr;

    llarp_ev_loop_alloc(&netloop);  // set up netio worker
    worker = llarp_init_same_process_threadpool();
    logic  = llarp_init_single_process_logic(worker);  // set up logic worker

    // configure main netloop
    struct dnsd_context dnsd;
    if(!llarp_dnsd_init(&dnsd, netloop, logic, "*", server_port,
                        (const char *)dnsr_config.upstream_host.c_str(),
                        dnsr_config.upstream_port))
    {
      // llarp::LogError("failed to initialize dns subsystem");
      llarp::LogError("Couldnt init dns daemon");
      return 0;
    }
    // Configure intercept
    dnsd.intercept = &hookChecker;

    llarp::LogInfo("singlethread start");
    llarp_ev_loop_run_single_process(netloop, worker, logic);
    llarp::LogInfo("singlethread end");

    llarp_ev_loop_free(&netloop);
  }
  else
  {
    // need this for timer stuff
    llarp_threadpool *worker = nullptr;
    llarp_logic *logic       = nullptr;
    worker                   = llarp_init_same_process_threadpool();
    logic = llarp_init_single_process_logic(worker);  // set up logic worker

    // configure main netloop
    struct dnsd_context dnsd;
    if(!llarp_dnsd_init(&dnsd, nullptr, logic, "*", server_port,
                        (const char *)dnsr_config.upstream_host.c_str(),
                        dnsr_config.upstream_port))
    {
      // llarp::LogError("failed to initialize dns subsystem");
      llarp::LogError("Couldnt init dns daemon");
      return 0;
    }
    // Configure intercept
    dnsd.intercept = &hookChecker;

    struct sockaddr_in m_address;
    int m_sockfd;

    m_sockfd                  = socket(AF_INET, SOCK_DGRAM, 0);
    m_address.sin_family      = AF_INET;
    m_address.sin_addr.s_addr = INADDR_ANY;
    m_address.sin_port        = htons(1053);
    int rbind                 = bind(m_sockfd, (struct sockaddr *)&m_address,
                     sizeof(struct sockaddr_in));

    if(rbind != 0)
    {
      llarp::LogError("Could not bind: ", strerror(errno));
      return 0;
    }

    const size_t BUFFER_SIZE = 1024;
    char buffer[BUFFER_SIZE];  // 1024 is buffer size
    struct sockaddr_in clientAddress;
    socklen_t addrLen = sizeof(struct sockaddr_in);

    struct timeval tv;
    tv.tv_sec  = 0;
    tv.tv_usec = 100 * 1000;  // 1 sec
#ifndef _WIN32
    if(setsockopt(m_sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
#else
    if(setsockopt(m_sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv,
                  sizeof(tv))
       < 0)
#endif
    {
      perror("Error");
    }

    signal(SIGINT, handle_signal);
    while(!done)
    {
      // sigint quits after next packet
      int nbytes = recvfrom(m_sockfd, buffer, BUFFER_SIZE, 0,
                            (struct sockaddr *)&clientAddress, &addrLen);
      if(nbytes == -1)
        continue;
      llarp::LogInfo("Received Bytes ", nbytes);

      raw_handle_recvfrom(&m_sockfd, (const struct sockaddr *)&clientAddress,
                          buffer, nbytes);
    }
  }

  return code;
}
