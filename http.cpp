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
#include <chrono>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <mutex>
#include "ThreadPool.h"
#include "EpollEngine.h"
#include "MimeTypes.h"
class SimpleHTTPServer{
   private:
   int port;
   bool running;
   bool initialized;
   bool shutDown_ = false;
   std::unordered_map<int, std::string> connBuffers_;
   std::unordered_map<int, std::string> outBuffers_;
   std::mutex connMutex_;
   std::unordered_map<int, std::chrono::steady_clock::time_point> lastActive_;
   std::unordered_set<int> inFlight_;
   std::chrono::steady_clock::time_point lastSweep_;
   static constexpr std::chrono::seconds kIdleTimeout{30};
   std::string dirRoot = "./www";
   ThreadPool threadPool;
   EpollEngine epollEngine;
   MimeTypes mimeTypes;


   enum class ReadStatus {Request, WaitMore, Closed, Error, TooLarge};
   
   ReadStatus readRequest(int clientSocket,std::string& connBuffer, std::string& request){
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

   bool clientWantsClose(const std::string& request){
      size_t p = request.find("Connection:");
      if(p == std::string::npos) return false;
      size_t e = request.find("\r\n", p);
      std::string line = request.substr(p, e == std::string::npos? request.size()-p: e-p);
      std::transform(line.begin(), line.end(), line.begin(), ::tolower);
      return line.find("close") != std::string::npos;
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
      
      bool processRequest(int clientSocket, const std::string& request){
         bool keep = !clientWantsClose(request);
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
         if(realpath((dirRoot + userPath).c_str(), resolved) == nullptr){
            send404(clientSocket, keep);
            return keep;
         }
         std::string path = resolved;
         
         if(path.compare(0, dirRoot.size(), dirRoot) != 0 || (path.size() > dirRoot.size() && path[dirRoot.size()] != '/')){
            send404(clientSocket, keep);
            return keep;
         }
         
         else{
            std::string type = getMimeType(path);
            std::string content = readHTMLFile(path);
         if(content.empty()){
            send404(clientSocket, keep);
            return keep;
         }
         std::string response = createResponse(content, type, 200, keep); 
         sendResponse(clientSocket, response); 
         return keep;   
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
               bool keep = true;
               try{
                  keep = processRequest(clientSocket, request);
               }catch(...){
                  std::cerr<<"服务器出现异常"<<std::endl;
                  send500(clientSocket);
                  keep = false; 
               }
               if(!keep) {
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

      bool trySend(int fd, const char*& data, size_t& toSend){
         while(toSend > 0){
            ssize_t sent = send(fd, data, toSend, 0);
            if(sent < 0){
               if(errno == EINTR) continue;
               if(errno == EAGAIN || errno == EWOULDBLOCK) return true;
               return false;
            }
            data += sent;
            toSend -= sent;
         }
         return true;
      }

      void sendResponse(int clientSocket, const std::string& response){
         const char* data = response.data();
         size_t toSend = response.size();

         trySend(clientSocket, data, toSend);
         if(toSend > 0){
            std::lock_guard<std::mutex> lk(connMutex_);
            outBuffers_[clientSocket].append(data, toSend);
         }
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

      void send404(int clientSocket, bool keepAlive = true){
         std::string path =  dirRoot + "/404Response.html";
         std::string content = readHTMLFile(path);
         if(content.empty()){
            content =  "<html><body><h1>404 Not Found</h1></body></html>";
         }
         std::string response = createResponse(content, "text/html", 404, keepAlive);
         std::cerr << "Sending 404 Not Found response" << std::endl;
         sendResponse(clientSocket, response);
      }

      void send500(int clientSocket, bool keepAlive = false){
         std::string path = dirRoot + "/500Response.html";
         std::string content = readHTMLFile(path);
         if(content.empty()){
            content =  "<html><body><h1>500 Internal Server Error</h1></body></html>";
         }
         std::string response = createResponse(content, "text/html", 500, keepAlive);
         std::cerr << "Sending 500 error response" << std::endl;
         sendResponse(clientSocket, response);
      }

       std::string getMimeType(const std::string& path){
         size_t pos = path.find_last_of('.');
          if(pos == std::string::npos) return "application/octet-stream";
          std::string ext = path.substr(pos + 1);
          std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
          return mimeTypes.getType(ext);

          }

       std::string readHTMLFile(const std::string& filename){

         std::ifstream file(filename);

         if(!file.is_open()){

            return "";
         }

         else{

            std::stringstream buffer;

            buffer <<file.rdbuf();
            return buffer.str();
         }
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