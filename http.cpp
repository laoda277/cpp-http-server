#include <iostream>
#include <string>
#include <cstring>
#include <cerrno>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdlib>  
#include <climits>   
#include <cctype>
#include <chrono>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <mutex>
#include "ThreadPool.h"
#include "EpollEngine.h"
#include "MimeTypes.h"
#include "FileCache.h"
class SimpleHTTPServer{
   private:
   int port;
   bool running;
   bool initialized;
   bool shutDown_ = false;
   std::unordered_map<int, std::string> connBuffers_;
   std::unordered_map<int, std::string> outBuffers_; //要发送的数据
   std::mutex connMutex_;
   std::unordered_map<int, std::chrono::steady_clock::time_point> lastActive_;
   std::unordered_set<int> inFlight_;
   std::chrono::steady_clock::time_point lastSweep_;
   static constexpr std::chrono::seconds kIdleTimeout{30};
   std::string dirRoot = "./www";
   ThreadPool threadPool;
   EpollEngine epollEngine;
   MimeTypes mimeTypes;
   FileCache fileCache;


   enum class ReadStatus {Request, WaitMore, Closed, Error, TooLarge}; //enum class强类型枚举 用于处理魔法数字 ReadStatus::访问
   
   ReadStatus readRequest(int clientSocket,std::string& connBuffer, std::string& request){ //将缓冲区内容写到request中 返回值表示读的状态
       const size_t kMaxTotal = 8192;
       char chunk[4096];

       while(true){
         size_t pos = connBuffer.find("\r\n\r\n");
         if(pos != std::string::npos){
            request = connBuffer.substr(0, pos+4);
            connBuffer.erase(0, pos+4);
            return ReadStatus::Request;
         }

         if(connBuffer.size() >= kMaxTotal) return ReadStatus::TooLarge;

         ssize_t n;
         do{
            n = recv(clientSocket, chunk, sizeof(chunk), 0);
         }while(n<0 && errno == EINTR);

         if(n < 0){
            if(errno == EAGAIN || errno == EWOULDBLOCK)
            return ReadStatus::WaitMore;
         return ReadStatus::Error;
         }
         if(n == 0){
            return ReadStatus::Closed;
         }
         connBuffer.append(chunk, (size_t)n);      
       }
   }

   std::string extractVersion(const std::string& request){
      size_t lineEnd = request.find("\r\n");
      std::string line = request.substr(0, lineEnd);

      size_t sp = line.rfind("/");
      if(sp == std::string::npos) return "";
      return line.substr(sp+1);
   }

   bool clientWantsClose(const std::string& request){
      std::string lower = request;
      std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c){
         return std::tolower(c);
      });
      size_t p = lower.find("\r\nconnection:");
      if(p != std::string::npos) {
         size_t e = lower.find("\r\n", p+2);

         if(e != std::string::npos){
            std::string val = lower.substr(p+2, e-(p+2));
            if(val.find("close") != std::string::npos) return true;
            if(val.find("keep-alive") != std::string::npos)  return false;
         }
      }
      return extractVersion(request) == "1.0";
      
   }


      std::string extractPath(const std::string& request){
      size_t start=request.find(" ");
      if(start==std::string::npos) return"/";

      size_t end=request.find(" ",start+1);
      if(end==std::string::npos)  return "/";

      return request.substr(start+1,end-start-1);
      } 


      std::string createResponse(const std::string& content,const std::string& contentType="text/html",int statusCode=200, bool keepAlive = true){
       std::ostringstream response;
       std::string statusMessage;
       if(statusCode==200){
         statusMessage="OK";
       }
       else if(statusCode==404){
         statusMessage="Not Found";
       }
       else if(statusCode==500){
         statusMessage="Internal Server Error";
       }
       else{
         statusMessage="Unknown";
       }

       response<<"HTTP/1.1 "<<statusCode<<" "<<statusMessage<<"\r\n";
       response<<"Content-Type: "<<contentType<<"\r\n";
       response<<"Content-length: "<<content.length()<<"\r\n";
       response<<"Connection: " << (keepAlive? "keep-alive": "close") << "\r\n";
       response<<"\r\n";
       response<<content;

       return response.str();

      }
      
      bool processRequest(int clientSocket, const std::string& request){  //处理请求
         bool keep = !clientWantsClose(request); //表示是否还保持长连接
         std::string userPath = extractPath(request);    
         if(userPath == "/" || userPath .empty()){
            userPath = "/DefaultPage.html";
         }
         if(userPath == "/about") {
            userPath = "/AboutPage.html";
         }
         else if(userPath == "/api/status") {
            userPath = "/Status.json";
         }
         char resolved[PATH_MAX];
         if(realpath((dirRoot + userPath).c_str(), resolved) == nullptr){ //转为绝对路径
            bool alive = send404(clientSocket, keep);
            return keep && alive; //前者是协议方面的是否关闭 后者是连接是否存活方面但连接为断开时默认值等于前者
         }
         std::string path = resolved;
         
         if(path.compare(0, dirRoot.size(), dirRoot) != 0 || (path.size() > dirRoot.size() && path[dirRoot.size()] != '/')){
            bool alive = send404(clientSocket, keep);
            return keep && alive;
         }
         
         else{
            std::string type = getMimeType(path);
            std::string content = readHTMLFile(path);
         if(content.empty()){
            bool alive = send404(clientSocket, keep);
            return keep && alive;
         }
         std::string response = createResponse(content, type, 200, keep); 
         bool alive = sendResponse(clientSocket, response);
         return keep && alive; //同样sendResponse返回值表示连接是否存活 但中间没有send404这一层
         }  
      }

      void handleOnce(int clientSocket){
         std::string buf;
         {
            std::lock_guard<std::mutex> lk(connMutex_);
            buf = std::move(connBuffers_[clientSocket]);
            lastActive_[clientSocket] = std::chrono::steady_clock::now();
            inFlight_.insert(clientSocket);
         }

         while(true){
            std::string request;
            ReadStatus st = readRequest(clientSocket, buf, request);

            if(st == ReadStatus::Request) {
               bool alive = true; //表示这个链接还能不能保持工作
               try{
                  alive = processRequest(clientSocket, request);
               }catch(...){
                  std::cerr<<"服务器出现异常"<<std::endl;
                  send500(clientSocket);
                  alive = false; 
               }
               if(!alive) {
                  closeConnection(clientSocket);
                  return;
               }
               continue;
            }
            if(st == ReadStatus::WaitMore) break;
            if(st == ReadStatus::TooLarge) send500(clientSocket);
            closeConnection(clientSocket);
            return;
         }
         {
            std::lock_guard<std::mutex> lk(connMutex_);
            connBuffers_[clientSocket] = std::move(buf);
         }
         finishOnce(clientSocket);

}

      void handleWritable(int fd){
         std::string out;
         {
            std::lock_guard<std::mutex> lk(connMutex_);
            lastActive_[fd] = std::chrono::steady_clock::now();
            inFlight_.insert(fd);
            auto it = outBuffers_.find(fd);
            if(it != outBuffers_.end()){
               out = std::move(it -> second);
               outBuffers_.erase(it);
            }
         }

         const char* data = out.data();
         size_t toSend = out.size();
         if(!trySend(fd, data, toSend)){
            closeConnection(fd);
            return;
         }
         if(toSend > 0){
            std::lock_guard<std::mutex> lk(connMutex_);
            outBuffers_[fd].append(data, toSend);
         }
         finishOnce(fd);
      }


        void finishOnce(int fd){
         bool hasPending = false;
         {
            std::lock_guard<std::mutex> lk(connMutex_);
            inFlight_.erase(fd);
            hasPending = outBuffers_.count(fd) >0;
         }
         epollEngine.armConnection(fd, hasPending? EPOLLOUT:EPOLLIN);
        }
      


      void closeConnection(int fd){
         epollEngine.removeConnection(fd);
         {
            std::lock_guard<std::mutex> lk(connMutex_);
            connBuffers_.erase(fd);
            outBuffers_.erase(fd);
            lastActive_.erase(fd);
            inFlight_.erase(fd);
         }
         close(fd);
      }

      [[nodiscard]] bool trySend(int fd, const char*& data, size_t& toSend){ //尝试进行发送
         while(toSend > 0){
            ssize_t sent = send(fd, data, toSend, 0);
            if(sent < 0){
               if(errno == EINTR) continue;  //暂时中断
               if(errno == EAGAIN || errno == EWOULDBLOCK) return true; //两个为同一个意思 表示缓冲区已满或者没有新数据
               return false; //连接已经断开
            }
            data += sent;
            toSend -= sent;
         }
         return true;
      }

      bool sendResponse(int clientSocket, const std::string& response){
         const char* data = response.data(); //从第几个数据开始发送
         size_t toSend = response.size(); //还需要发送多少数据

         if(!trySend(clientSocket, data, toSend)){ //尝试发送一次，成功执行下一个if
            return false;
         }
         if(toSend > 0){
            std::lock_guard<std::mutex> lk(connMutex_);
            outBuffers_[clientSocket].append(data, toSend); //还没发送完，把剩余数据放回缓冲区
         }
         return true;
      }

      void sweepIdleConnection(){
         auto now = std::chrono::steady_clock::now();
         {
            std::lock_guard<std::mutex> lk(connMutex_);
            if(now - lastSweep_ < std::chrono::seconds(1)) return;
            lastSweep_ =  now;
         }
         std::vector<int> victims;
         {
            std::lock_guard<std::mutex> lk(connMutex_);
            for(const auto&[fd, t] : lastActive_){
               if(!inFlight_.count(fd) && now-t > kIdleTimeout)
                  victims.push_back(fd);
            }  
         }
         for(int fd: victims) closeConnection(fd);
      }

      void shutdownServer() {
         if(shutDown_) return;
         shutDown_ = true;

         threadPool.drain();

         std::vector<int> fds;
         {
            std::lock_guard<std::mutex> lk(connMutex_);
            fds.reserve(lastActive_.size());
            for(const auto &[fd, t] : lastActive_){
               fds.push_back(fd);
            }
         }
         for(int fd : fds){
            closeConnection(fd);
         }
      }

      //存活状态由sendResponse上传到send404与500  
      bool send404(int clientSocket, bool keepAlive = true){ //发送错误码并返回链接存活状态
         std::string path =  dirRoot + "/404Response.html";
         std::string content = readHTMLFile(path);
         if(content.empty()){
            content =  "<html><body><h1>404 Not Found</h1></body></html>";
         }
         std::string response = createResponse(content, "text/html", 404, keepAlive);
         std::cerr << "Sending 404 Not Found response" << std::endl;
         return sendResponse(clientSocket, response); 
      }

      bool send500(int clientSocket, bool keepAlive = false){
         std::string path = dirRoot + "/500Response.html";
         std::string content = readHTMLFile(path);
         if(content.empty()){
            content =  "<html><body><h1>500 Internal Server Error</h1></body></html>";
         }
         std::string response = createResponse(content, "text/html", 500, keepAlive);
         std::cerr << "Sending 500 error response" << std::endl;
         return sendResponse(clientSocket, response);
      }

       std::string getMimeType(const std::string& path){
         size_t pos = path.find_last_of('.');
          if(pos == std::string::npos) return "application/octet-stream";
          std::string ext = path.substr(pos + 1);
          std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){
            return std::tolower(c);
          });
          return mimeTypes.getType(ext);

          }

       std::string readHTMLFile(const std::string& filename){
         auto cached = fileCache.get(filename);
         if(cached) return *cached;

         std::ifstream file(filename);
         if(!file.is_open()){
            return "";
         }
            std::stringstream buffer;
            buffer <<file.rdbuf();
            fileCache.put(filename, buffer.str());
            return buffer.str();    
      }


public:
    SimpleHTTPServer(int port) : 
               port(port), 
               running(false), 
               initialized(false),
               threadPool(std::max<size_t> (2, std::thread::hardware_concurrency())) {
                  if(!mimeTypes.loadFromFile("mime.types")){
                     mimeTypes.setDefaults();
                  }

                  char resolved[PATH_MAX];
                  if(realpath(dirRoot.c_str(), resolved) == nullptr){
                     std::cerr<<"文件根目录解析失败："<<dirRoot<<std::endl;
                  }
                  else{
                     dirRoot = resolved;
                     initialized = true;
                  }
               }

                   



    bool start() {
      if(!epollEngine.init(port, 128)||!initialized){
         std::cerr<<"服务器初始化失败"<<std::endl;
         return false;
      }

      running = true;
      std::cout<<"服务器启动成功!"<<std::endl;
      std::cout<<"访问地址：http://localhost:"<<port<<std::endl;
      std::cout<<"按ctrl+c停止服务器"<<std::endl;

      bool ok = epollEngine.run([this] (int clientSocket, uint32_t events){
         if(events & (EPOLLERR | EPOLLHUP)){
            closeConnection(clientSocket);
            return;
         }
         if(events & EPOLLOUT){
            threadPool.enqueue(
               [this, clientSocket]() {
                  this ->handleWritable(clientSocket);
               }
            );
         }
         else{
            threadPool.enqueue(
               [this, clientSocket]() {
                  this-> handleOnce(clientSocket);
               }
            );
         }
             
      }, [this](){sweepIdleConnection();}
   );
      shutdownServer();
      return ok;

    }

    void stop(){
      running = false;
      epollEngine.stop();
    }

    ~SimpleHTTPServer() {
      stop();
    }

   };

   int main(int argc, char* argv[]){
      int port = 8080;
      if(argc > 1){
         port = std::stoi(argv[1]);
      }

      SimpleHTTPServer server(port);

      if( !server.start()){
         std::cerr<<"服务器启动失败"<<std::endl;
         return 1;
      }

      return 0;



   }