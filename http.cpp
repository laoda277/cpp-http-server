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
#include "ThreadPool.h"
#include "EpollEngine.h"
#include "MimeTypes.h"
class SimpleHTTPServer{
   private:
   int port;
   bool running;
   bool initialized;
   std::string dirRoot = "./www";
   ThreadPool threadPool;
   EpollEngine epollEngine;
   MimeTypes mimeTypes;

   class SocketCloser{
         public:
            int closerSocket;
            SocketCloser(int clientSocket):closerSocket(clientSocket){ }

            ~SocketCloser(){
            if(closerSocket >= 0){
               close(closerSocket);
               }
            }

            SocketCloser(const SocketCloser&) = delete;
            SocketCloser& operator = (SocketCloser&) = delete;
         };

   std::string readRequest(int clientSocket,std::string& connBuffer){
       const size_t kMaxTotal = 8192;
       char chunk[4096];

       while(true){
         size_t pos = connBuffer.find("\r\n\r\n");
         if(pos != std::string::npos){
            std::string request = connBuffer.substr(0, pos+4);
            connBuffer.erase(0, pos+4);
            return request;
         }

         if(connBuffer.size() >= kMaxTotal) return "";

         ssize_t n;
         do{
            n = recv(clientSocket, chunk, sizeof(chunk), 0);
         }while(n<0 && errno == EINTR);

         if(n < 0){
            return "";
         }
         if(n == 0){
            return "";
         }
         connBuffer.append(chunk, (size_t)n);      
       }

       
   }
      std::string extractPath(const std::string& request){
      size_t start=request.find(" ");
      if(start==std::string::npos) return"/";

      size_t end=request.find(" ",start+1);
      if(end==std::string::npos)  return "/";

      return request.substr(start+1,end-start-1);


      } 
      std::string createResponse(const std::string& content,const std::string& contentType="text/html",int statusCode=200){
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
       response<<"Connection: keep-alive\r\n";
       response<<"\r\n";
       response<<content;

       return response.str();

      }
      
      void handleClient(int clientSocket) {
         try{

         SocketCloser socketCloser(clientSocket);

         struct timeval timeout = {30, 0};
         setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

         std::string connBuffer;
         while(true){
         std::string request=readRequest(clientSocket, connBuffer);
         if(request.empty()){
            break;
         }
      

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
            send404(clientSocket);
            return;
         }
         std::string path = resolved;
         

         if(path.compare(0, dirRoot.size(), dirRoot) != 0 || (path.size() > dirRoot.size() && path[dirRoot.size()] != '/')){
            send404(clientSocket);
            return;
         }
         
         else{
            std::string type = getMimeType(path);
            std::string content = readHTMLFile(path);
         if(content.empty()){
            send404(clientSocket);
            return;
         }
         std::string response = createResponse(content, type, 200); 
         sendResponse(clientSocket, response); 
         }   
      }
      }    
      
      catch(...){
         std::cerr<<"服务器出现异常"<<std::endl;
         send500(clientSocket);
      }

      }

      void sendResponse(int clientSocket, const std::string& response){
         const char* data = response.data();
         size_t toSend = response.size();

         while(toSend > 0){
            ssize_t sent = send(clientSocket, data, toSend, 0);
            if(sent < 0){
               if(errno == EINTR) continue;
               break;
            }
            data += sent;
            toSend -= sent;
         }
      }

      void send404(int clientSocket){
         std::string path =  dirRoot + "/404Response.html";
         std::string content = readHTMLFile(path);
         if(content.empty()){
            content =  "<html><body><h1>404 Not Found</h1></body></html>";
         }
         std::string response = createResponse(content, "text/html", 404);
         std::cerr << "Sending 404 Not Found response" << std::endl;
         sendResponse(clientSocket, response);
      }

      void send500(int clientSocket){
         std::string path = dirRoot + "/500Response.html";
         std::string content = readHTMLFile(path);
         if(content.empty()){
            content =  "<html><body><h1>500 Internal Server Error</h1></body></html>";
         }
         std::string response = createResponse(content, "text/html", 500);
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
      if(!epollEngine.init(port, 10)||!initialized){
         std::cerr<<"服务器初始化失败"<<std::endl;
         return false;
      }

      running = true;
      std::cout<<"服务器启动成功!"<<std::endl;
      std::cout<<"访问地址：http://localhost:"<<port<<std::endl;
      std::cout<<"按ctrl+c停止服务器"<<std::endl;

      return epollEngine.run([this] (int clientSocket){
             threadPool.enqueue([this, clientSocket](){
            this->handleClient(clientSocket);
         });
      });

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