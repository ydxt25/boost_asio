#include <assert.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <exception>
#include <sstream>
#include <iostream>
#include <string>
#include <deque>
#include <set>
#include "boost/asio.hpp"
#include "boost/thread.hpp"
#include "boost/bind.hpp"
#include "boost/shared_ptr.hpp"
#include "boost/enable_shared_from_this.hpp"
#include "boost/thread/thread.hpp"
#include "boost/date_time/posix_time/posix_time.hpp"
#include "boost/atomic.hpp"
#include "boost/program_options/options_description.hpp"
#include "boost/program_options/parsers.hpp"
#include "boost/program_options/variables_map.hpp"
#include "boost/log/common.hpp"
#include "boost/log/core.hpp"
#include "boost/log/sinks.hpp"
#include "boost/log/attributes.hpp"
#include "boost/log/expressions.hpp"
#include "boost/log/trivial.hpp"
#include "boost/log/utility/setup/common_attributes.hpp"
#include "boost/log/utility/exception_handler.hpp"
#include "boost/log/support/date_time.hpp"

namespace {
// 定义全局唯一的source, 使用内置的severity_level作为日志级别, 使用severity_logger_mt提供多线程安全
// 的日志source.
BOOST_LOG_INLINE_GLOBAL_LOGGER_DEFAULT(global_logger_src, 
    boost::log::sources::severity_logger_mt<boost::log::trivial::severity_level>);

// 定义输出日志的宏, 调用BOOST_LOG_FUNCTION来提供name_scope属性的值, 并通过BOOST_LOG_SEV向全局source输出带级别的
// 日志行, 日志流向: 
// global_logger_src(过滤输出日志级别范围) -> core -> synchronous_sink(frontend,过滤特定级别) -> text_file_backend
#define LOG(level) BOOST_LOG_FUNCTION();BOOST_LOG_SEV(global_logger_src::get(), boost::log::trivial::level)

boost::shared_ptr<boost::log::sinks::text_file_backend> BuildSinkBackend(const std::string& log_dir, const std::string& sink_name) {
  boost::shared_ptr<boost::log::sinks::text_file_backend> backend = boost::make_shared<boost::log::sinks::text_file_backend>(
        boost::log::keywords::file_name = log_dir + "/echo_client." + sink_name + ".%Y%m%d.%H%M.%N.log",
        boost::log::keywords::rotation_size = 1024ULL * 1024 * 1024,// 每1GB切换一个文件
        boost::log::keywords::open_mode = std::ios::app, // 打开文件采用追加写
        boost::log::keywords::auto_flush = true // 每行日志立即刷到磁盘
    );
  try {
    backend->set_file_collector(boost::log::sinks::file::make_collector(
          boost::log::keywords::target = log_dir + "/" + sink_name, // 切换后的日志mv到此目录下
          boost::log::keywords::max_size = 20ULL * 1024 * 1024 * 1024 // 目录下日志总大小不超过20GB,否则会淘汰最老的文件.
          )
        );
    backend->scan_for_files(); // 扫描目录下已有文件,以便递增文件序号以及做日志文件回收.
  } catch (std::exception& except) {
    // 可能因为目录权限原因失败,我们只打印一条警告并继续, 一旦用户恢复目录权限, boost.log会立即恢复工作.
    std::cerr << except.what() << std::endl; 
  }
  return backend;
}
pid_t gettid() {
  return syscall(SYS_gettid);
}
void InitLogging(bool open_debug, const std::string& log_dir) {
  // 获取core, 以便向其注册sink
  boost::shared_ptr<boost::log::core> core = boost::log::core::get();
  // 添加通用属性(时间,进程ID,线程ID)
  // 注: boost::log::add_common_attributes();函数添加的进程ID和线程ID是16进制的,我决定自定义linux订制的版本.
  core->add_global_attribute("TimeStamp", boost::log::attributes::local_clock());
  core->add_global_attribute("ProcessID", boost::log::attributes::make_function(&getpid));
  core->add_global_attribute("ThreadID", boost::log::attributes::make_function(&gettid));
  // 添加作用域属性（函数名,源文件名,行号)
  core->add_global_attribute("Scope", boost::log::attributes::named_scope());
  // 忽略所有log库可能抛出的异常
  core->set_exception_handler(boost::log::make_exception_suppressor());
  // 关闭调试日志
  if (!open_debug) {
    core->set_filter(boost::log::expressions::attr<boost::log::trivial::severity_level>("Severity") >= boost::log::trivial::info);
  }
  typedef boost::log::sinks::synchronous_sink<boost::log::sinks::text_file_backend> sync_sink_frontend;
  // 构造3个sink:
  // 1,severity<=debug级别的输出到sink_trace_debug
  boost::log::formatter scope_formatter = boost::log::expressions::stream << 
    "[" << boost::log::expressions::format_date_time<boost::posix_time::ptime>("TimeStamp", "%Y-%m-%d %H:%M:%S") << 
    "] [" << boost::log::expressions::attr<pid_t>("ProcessID") << "-" << boost::log::expressions::attr<pid_t>("ThreadID") << 
    "] [" << boost::log::expressions::attr<boost::log::trivial::severity_level>("Severity") <<
    "] " << boost::log::expressions::format_named_scope("Scope", boost::log::keywords::format = "%c[%F:%l] ", 
        boost::log::keywords::depth = 1) << boost::log::expressions::smessage;
  boost::shared_ptr<boost::log::sinks::text_file_backend> sink_trace_debug_backend = BuildSinkBackend(log_dir, "trace_debug");
  boost::shared_ptr<sync_sink_frontend> sink_trace_debug_frontend(new sync_sink_frontend(sink_trace_debug_backend));
  sink_trace_debug_frontend->set_filter(
      boost::log::expressions::attr<boost::log::trivial::severity_level>("Severity") <= boost::log::trivial::debug && 
      boost::log::expressions::attr<boost::log::trivial::severity_level>("Severity") >= boost::log::trivial::trace);
  sink_trace_debug_frontend->set_formatter(scope_formatter);
  core->add_sink(sink_trace_debug_frontend);
  // 2,debug<severity<=warning级别到sink_info_warning
  boost::log::formatter non_scope_formatter = boost::log::expressions::stream << 
    "[" << boost::log::expressions::format_date_time<boost::posix_time::ptime>("TimeStamp", "%Y-%m-%d %H:%M:%S") <<
    "] [" << boost::log::expressions::attr<pid_t>("ProcessID") << "-" << boost::log::expressions::attr<pid_t>("ThreadID") << 
    "] [" << boost::log::expressions::attr<boost::log::trivial::severity_level>("Severity") <<
    "] " << boost::log::expressions::smessage;
  boost::shared_ptr<boost::log::sinks::text_file_backend> sink_info_warning_backend = BuildSinkBackend(log_dir, "info_warning");
  boost::shared_ptr<sync_sink_frontend> sink_info_warning_frontend(new sync_sink_frontend(sink_info_warning_backend));
  sink_info_warning_frontend->set_filter(
      boost::log::expressions::attr<boost::log::trivial::severity_level>("Severity") <= boost::log::trivial::warning && 
      boost::log::expressions::attr<boost::log::trivial::severity_level>("Severity") > boost::log::trivial::debug);
  sink_info_warning_frontend->set_formatter(non_scope_formatter);
  core->add_sink(sink_info_warning_frontend);
  // 3,warning<severity<=fatal级别输出到sink_error_fatal
  boost::shared_ptr<boost::log::sinks::text_file_backend> sink_error_fatal_backend = BuildSinkBackend(log_dir, "error_fatal");
  boost::shared_ptr<sync_sink_frontend> sink_error_fatal_frontend(new sync_sink_frontend(sink_info_warning_backend));
  sink_error_fatal_frontend->set_filter(
      boost::log::expressions::attr<boost::log::trivial::severity_level>("Severity") <= boost::log::trivial::fatal && 
      boost::log::expressions::attr<boost::log::trivial::severity_level>("Severity") > boost::log::trivial::warning);
  sink_error_fatal_frontend->set_formatter(non_scope_formatter);
  core->add_sink(sink_error_fatal_frontend);
}

class EchoClient;

typedef boost::shared_ptr<EchoClient> EchoClientPtr;
typedef boost::shared_ptr<boost::asio::io_service> IOServicePtr;
typedef boost::shared_ptr<boost::asio::ip::tcp::socket> SocketPtr;
typedef boost::shared_ptr<boost::asio::ip::tcp::resolver> ResolverPtr;
class Connection;
typedef boost::shared_ptr<Connection> ConnPtr;
typedef boost::shared_ptr<std::string> StringPtr;
typedef boost::shared_ptr<boost::asio::deadline_timer> TimerPtr;

// 准则1:
// 一个Socket永远不要调用async_read/async_write超过1次,可以参考boost doc:
// This operation is implemented in terms of zero or more calls to the stream's async_write_some function, and is known as a composed operation. The program must ensure that the stream performs no other write operations (such as async_write, the stream's async_write_some function, or any other composed operations that perform writes) until this operation completes.
// 也就是一定要前一个async操作完成再发起下一个!!

// 准则2:
// 操作1个socket, 在多线程条件下一定要加锁处理, 一把大锁解决一切问题, 其他用法都是非线程安全的.
// 也就是说同步close/async_read/async_write/async_connect这四个函数调用即可.

class Connection : public boost::enable_shared_from_this<Connection> {
public:
  enum ConnStatus {
    kResolving = 0, // 异步解析域名
    kResolveError, // 异步解析域名失败
    kConnecting, // 异步建立连接
    kConnected,
    kError,
    kClosed,
  };
  Connection(IOServicePtr io_service, const std::string& host, unsigned short port)
    : status_(kResolving), io_service_(io_service), host_(host), port_(port) {
  }
  ~Connection() {
    // 可以在这里将write_queue中的待发消息进行重试等逻辑处理
    LOG(trace) << __FUNCTION__;
  }
  void Start() {
    std::ostringstream str_port;
    str_port << port_;
    boost::asio::ip::tcp::resolver::query query(boost::asio::ip::tcp::v4(), host_, str_port.str());
    resolver_.reset(new boost::asio::ip::tcp::resolver(*io_service_));
    resolver_->async_resolve(query, boost::bind(&Connection::ResolveHandler, shared_from_this(), _1, _2));
  }
  void Close() { // 重复的调用socket的close没有问题, 但不能并发调用close(假设Close接口暴露给用户,是有这种需求的).
    ConnStatus cur_status = status_.exchange(kClosed);
    if (cur_status != kClosed) { // 即便重复调用socket的close是没有问题的, 但是这里也保证Close只能被调用一次.
      if (cur_status != kResolving && cur_status != kResolveError) { // 除了解析域名状态外, 其他状态的socket都已经open了.
        boost::lock_guard<boost::mutex> guard(socket_mutex_);
        boost::system::error_code errcode;
        assert(socket_->close(errcode) == boost::system::errc::success);
      }
    }
  }
  void EchoMsg(StringPtr msg) {
    LOG(debug) << "EchoMsg: " << *msg;
    boost::lock_guard<boost::mutex> guard(socket_mutex_);
    write_queue_.push_back(msg);
    if (write_queue_.size() == 1 && status_.load() == kConnected) {
      async_write(*socket_, boost::asio::buffer(*msg), boost::bind(&Connection::WriteHandler, shared_from_this(), _1, _2));
    }
  }
  ConnStatus status() { return status_.load(); }
private:
  void ResolveHandler(const boost::system::error_code& error, boost::asio::ip::tcp::resolver::iterator iterator) {
    // Resolved
    LOG(trace) << __FUNCTION__;
    if (!error) { // 解析成功, 选择一个IP发起async_connect.
      boost::asio::ip::tcp::resolver::iterator end;
      if (iterator != end) {
        const boost::asio::ip::tcp::endpoint& endpoint = *iterator;
        socket_.reset(new boost::asio::ip::tcp::socket(*io_service_));
        boost::lock_guard<boost::mutex> guard(socket_mutex_);
        ConnStatus expected = kResolving;
        if (!status_.compare_exchange_strong(expected, kConnecting)) {
          LOG(debug) << "ResolveHandler, Status Is Not Resolving(always kClosed) While Resolved.";
          return;
        }
        socket_->async_connect(endpoint, boost::bind(&Connection::ConnectHandler, shared_from_this(), _1));
        return;
      }
    } else if (error == boost::asio::error::operation_aborted) { // 虽然代码里不会cancel resolver, 但依旧实现这个逻辑以便允许上层对超时解析进行cancel.
      LOG(trace) << "Connection ResolveHandler Canceled.";
      return;
    }
    // 没有解析到IP或者解析发生了错误, 设置连接为错误状态.
    ConnStatus expected = kResolving;
    if (status_.compare_exchange_strong(expected, kResolveError)) {
      LOG(warning) << "ResolveHandler Error.";
    }
  }
  void ConnectHandler(const boost::system::error_code& error) {
    if (!error) { // 连接成功, 发起读取
      boost::lock_guard<boost::mutex> guard(socket_mutex_);
      ConnStatus expected = kConnecting;
      if (!status_.compare_exchange_strong(expected, kConnected)) {
        LOG(debug) << "ConnectHandler, Status Is Not Connecting(always kClosed) While Connected.";
        return;
      }
      socket_->async_receive(boost::asio::buffer(msgbuf_, sizeof(msgbuf_)), boost::bind(&Connection::ReadHandler, shared_from_this(), _1, _2));
      if (write_queue_.size()) {
        StringPtr next_msg = write_queue_.front();
        // async_write保证数据全部写完回调.
        async_write(*socket_, boost::asio::buffer(*next_msg), boost::bind(&Connection::WriteHandler, shared_from_this(), _1, _2));
      }
    } else if (error == boost::asio::error::operation_aborted) {
      LOG(trace) << "Connection ConnectHandler Canceled.";
    } else {
      ConnStatus expected = kConnecting;
      if (status_.compare_exchange_strong(expected, kError)) {
        LOG(warning) << "ConnectHandler Error.";
      }
    }
  }
  void ReadHandler(const boost::system::error_code& error, std::size_t bytes_transferred) {
    if (!error) { // 没有发生错误(包含被取消), 那么发起下一次读取.
      // 该函数读到一些数据就会返回, 正好适用于这里的echo逻辑. 如果希望读取指定长度完成前不返回, 使用async_read.
      StringPtr msg(new std::string(msgbuf_, bytes_transferred));
      {
        boost::lock_guard<boost::mutex> guard(socket_mutex_);
        socket_->async_receive(boost::asio::buffer(msgbuf_, sizeof(msgbuf_)), boost::bind(&Connection::ReadHandler, shared_from_this(), _1, _2));
      }
      // printf("%.*s", (int)bytes_transferred, msgbuf_);
      // 这里展示一下如何在多线程asio下正确的使用async_write有序的发送echo, 并且待发送消息队列以便在socket失效时有机会发送消息重发.
      EchoMsg(msg);
    } else if (error == boost::asio::error::operation_aborted) {
      LOG(trace) << "Connection ReadHandler Canceled.";
    } else {
      ConnStatus expected = kConnected;
      if (status_.compare_exchange_strong(expected, kError)) {
        LOG(warning) << "ReadHandler Error.";
      }
    }
  }
  void WriteHandler(const boost::system::error_code& error, std::size_t bytes_transferred) {
    if (!error) {
      boost::lock_guard<boost::mutex> guard(socket_mutex_);
      write_queue_.pop_front();
      if (write_queue_.size()) {
        StringPtr next_msg = write_queue_.front();
        // async_write保证数据全部写完回调.
        async_write(*socket_, boost::asio::buffer(*next_msg), boost::bind(&Connection::WriteHandler, shared_from_this(), _1, _2));
      }
    } else if (error == boost::asio::error::operation_aborted) {
      LOG(trace) << "Connection WriteHandler Canceled.";
    } else {
      ConnStatus expected = kConnected;
      if (status_.compare_exchange_strong(expected, kError)) {
        LOG(warning) << "WriteHandler Error.";
      }
    }
  }
  std::deque<StringPtr> write_queue_;
  boost::mutex socket_mutex_;
  boost::atomic<ConnStatus> status_;
  char msgbuf_[1024 * 16];
  SocketPtr socket_;
  IOServicePtr io_service_;
  ResolverPtr resolver_;
  std::string host_;
  unsigned short port_;
};

class EchoClient : public boost::enable_shared_from_this<EchoClient> {
public:
  EchoClient(IOServicePtr io_service, const std::string& host, unsigned short port, uint32_t concurrent)
    : host_(host), port_(port), concurrent_(concurrent), stopped_(false), io_service_(io_service) {
  }
  ~EchoClient() {
    // 在Stop后主线程释放引用计数, 等待io_service处理完剩余事件后析构, 可以Close掉所有Socket并释放引用计数.
    LOG(trace) << __FUNCTION__;
    boost::lock_guard<boost::mutex> guard(conn_set_mutex_);
    for (ConnSetIter iter = conn_set_.begin(); iter != conn_set_.end(); ++iter) {
      (*iter)->Close();
    }
  }
  bool Start() {
    boost::lock_guard<boost::mutex> guard(conn_set_mutex_);
    for (uint32_t i = 0; i < concurrent_; ++i) {
      ConnPtr cli_conn = AddNewConnection(host_, port_);
      TimerPtr socket_timer(new boost::asio::deadline_timer(*io_service_));
      socket_timer->expires_from_now(boost::posix_time::seconds(1));
      socket_timer->async_wait(boost::bind(&EchoClient::CheckSocketStatus, shared_from_this(), cli_conn, socket_timer, _1));
    }
    return true;
  }
  void Stop() {
    stopped_.store(true);
  }
private:
  ConnPtr AddNewConnection(const std::string& host, unsigned short port) {
    ConnPtr new_conn(new Connection(io_service_, host, port));
    new_conn->Start();
    new_conn->EchoMsg(StringPtr(new std::string("Hello Asio.\n")));
    conn_set_.insert(new_conn);
    return new_conn;
  }
  void CheckSocketStatus(ConnPtr conn, TimerPtr socket_timer, const boost::system::error_code& error) {
    // 1, EchoClient已经被Stop调用, 那么尽快停止timer释放掉对EchoClient的引用计数, 让EchoClient析构结束服务。
    // 2, 判断conn->status()==kError/kResolveError则Close连接并从ConnSet中移除, 重麓唇ㄐ铝�.
    // 3, 判断conn->status()==kClosed则从ConnSet中移除.(将来用户可以获取SocketPtr并随时调用Close)
    // 4, 连接正常, 继续发起下一次timer.
    boost::lock_guard<boost::mutex> guard(conn_set_mutex_);
    ConnSetIter iter = conn_set_.find(conn);
    assert(iter != conn_set_.end());
    if (stopped_.load()) {
      // case 1
      //LOG(debug) << "case 1";
      return;
    } else if (conn->status() == Connection::kError || conn->status() == Connection::kResolveError) { // case 2
      //LOG(debug) << "case 2";
      conn->Close();
      conn_set_.erase(conn); 
      conn = AddNewConnection(host_, port_);
    } else if (conn->status() == Connection::kClosed) {// case 3
      //LOG(debug) << "case 3";
      conn_set_.erase(conn); 
      conn = AddNewConnection(host_, port_);
    }
    //LOG(debug) << "case 4"; // case 4
    socket_timer->expires_from_now(boost::posix_time::seconds(1));
    socket_timer->async_wait(boost::bind(&EchoClient::CheckSocketStatus, shared_from_this(), conn, socket_timer, _1));
  }
  typedef std::set<ConnPtr> ConnSet;
  typedef ConnSet::iterator ConnSetIter;
  std::string host_;
  unsigned short port_;
  uint32_t concurrent_;
  boost::atomic<bool> stopped_;
  boost::mutex conn_set_mutex_;
  ConnSet conn_set_;
  IOServicePtr io_service_;
};

volatile sig_atomic_t g_shutdown_client = 0;
void ShutdownClientHandler(int signo) {
  g_shutdown_client = 1;
}
void SetupSignalHandler() {
  sigset_t sigset;
  sigfillset(&sigset);
  sigdelset(&sigset, SIGTERM);
  sigdelset(&sigset, SIGINT);
  sigprocmask(SIG_SETMASK, &sigset, NULL);

  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_handler = ShutdownClientHandler;
  sigaction(SIGINT, &act, NULL);
  sigaction(SIGTERM, &act, NULL);
}
void AsioThreadMain(IOServicePtr io_service) {
  // 多线程调用这个io_service跑leader-follower模型
  io_service->run();
}
bool ParseCommands(int argc, char** argv, boost::program_options::variables_map* options) {
  boost::program_options::options_description desc("Usage");
  desc.add_options()
      ("help", "show how to use this program")
      ("thread,t", boost::program_options::value<uint32_t>()->default_value(12), "number of threads of asio")
      ("host,h", boost::program_options::value<std::string>()->required(), "the tcp host client connects to")
      ("port,p", boost::program_options::value<unsigned short>()->required(), "the tcp port client connects to")
      ("concurrent,n", boost::program_options::value<uint32_t>()->default_value(1), "the number of connections to server")
      ("config,c", boost::program_options::value<std::string>(), "read config from file")
      ("log,l", boost::program_options::value<std::string>()->default_value("./clientlog"), "the directory to write log")
      ("debug,d", "open debug mode for logging");
  try {
    // 优先命令行
    boost::program_options::store(boost::program_options::parse_command_line(argc, argv, desc), *options);
    if (options->count("help")) {
      std::cerr << desc << std::endl;
      return false;
    }
    if (options->count("config")) { // 配置文件作为补充
      std::string cfile = (*options)["config"].as<std::string>();
      boost::program_options::store(boost::program_options::parse_config_file<char>(cfile.c_str(), desc), *options);
    }
    boost::program_options::notify(*options); // 最终触发参数校验
  } catch (std::exception& except) {
    std::cerr << except.what() << std::endl;
    std::cerr << desc << std::endl;
    return false;
  }
  return true;
}
}

int main(int argc, char** argv) {
  boost::program_options::variables_map options;
  if (!ParseCommands(argc, argv, &options)) {
    return -1;
  }
  InitLogging(options.count("debug"), options["log"].as<std::string>());
  
  SetupSignalHandler();

  IOServicePtr io_service(new boost::asio::io_service());

  std::string host = options["host"].as<std::string>();
  unsigned short port = options["port"].as<unsigned short>();
  uint32_t concurrent = options["concurrent"].as<uint32_t>();

  EchoClientPtr echo_client(new EchoClient(io_service, host, port, concurrent));
  if (!echo_client->Start()) {
    return -1;
  }
  uint32_t thread_num = options["thread"].as<uint32_t>();
  boost::thread_group asio_threads;
  for (uint32_t i = 0; i < thread_num; ++i) {
    asio_threads.create_thread(boost::bind(AsioThreadMain, io_service));
  }

  while (!g_shutdown_client) {
    sleep(1);
  }
  echo_client->Stop(); // 关闭客户端
  echo_client.reset();   // 释放引用计数, 让echo_client析构.
  asio_threads.join_all(); // 等待asio自然退出
  LOG(info) << "Stopped.. ..";
  return 0;
}
