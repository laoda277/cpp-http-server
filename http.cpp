#include <iostream>
#include <string>
#include <cstring>
#include <cerrno>
#include <sys/socket.h>
#include <unistd.h>
#include <fstream>
#include <sstream>
#include <algorithm>
#include "线程池.h"
#include "EpollEngine.h"
class SimpleHTTPServer{
   private:
   int port;
   bool running;
   ThreadPool threadPool;
   EpollEngine epollEngine
   
   ;
   std::string readRequest(int clientSocket){
       const size_t kMaxTotal = 8192;
       const size_t kRecvChunk = 4096;
       std::string buf;
       buf.reserve(512);
       char chunk[kRecvChunk];

       while (buf.size() < kMaxTotal) {
         const size_t room=kMaxTotal-buf.size();
         const size_t toRead = kRecvChunk < room ? kRecvChunk: room;

         ssize_t n;

         do{
            n=recv(clientSocket,chunk,toRead,0);
         }while(n<0&&errno==EINTR);

         if(n<0){
            return "";
         }
         if(n==0){
            break;
         }
         buf.append(chunk,(size_t)n);
         if(buf.find("\r\n\r\n")!=std::string::npos){
            break;
         }


       }
       return buf;
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
       else{
         statusMessage="Unknown";
       }

       response<<"HTTP/1.1 "<<statusCode<<" "<<statusMessage<<"\r\n";
       response<<"Content-Type: "<<contentType<<"\r\n";
       response<<"Content-length: "<<content.length()<<"\r\n";
       response<<"Connection: close\r\n";
       response<<"\r\n";
       response<<content;

       return response.str();

      }
      std::string create404Response(){
         std::string content="<html><body><h1>404 Not Found</h1></body></html>";
         return createResponse(content,"text/html",404);
      }
      
      void handleClient(int clientSocket) {
         std::string request=readRequest(clientSocket);
         if(request.empty()){
            close(clientSocket);
            return;
         }

         std::string path=extractPath(request);

         std::cout<<"收到请求"<<path<<std::endl;

         std::string response;

         if(path=="/"||path=="/index.html"){
            std::string content=readHTMLFile("/index.html");
            if(content.empty()) response=createResponse(getDefaultPage());
            else{
               response=createResponse(content);
            }
         }

         else if(path=="/about"){
            response=createResponse(getAboutPage());
         }

         else if(path=="/api/status"){
            response=createResponse(getStatusResponse(),"application/json");

         }

         else{
            response= create404Response();
         }

         send(clientSocket,response.c_str(),response.length(),0);
         
         close(clientSocket);
         

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




      std::string getDefaultPage() {
        return R"(
<!DOCTYPE html>
<html>
<head>
    <title>C++ 简单HTTP服务器</title>
    <meta charset="UTF-8">
    <style>
        body { font-family: Arial, sans-serif; margin: 40px; }
        h1 { color: #333; }
        .links { margin: 20px 0; }
        .links a { margin-right: 15px; color: #0066cc; text-decoration: none; }
        .links a:hover { text-decoration: underline; }
    </style>
</head>
<body>
    <h1>🎉 欢迎使用C++ HTTP服务器！</h1>
    <p>这是一个用C++编写的简单HTTP服务器，适合新手学习网络编程。</p>

    <div class="links">
        <a href="/about">关于</a>
        <a href="/api/status">API状态</a>
    </div>

    <h2>功能特性：</h2>
    <ul>
        <li>基础HTTP/1.1协议支持</li>
        <li>简单的路由处理</li>
        <li>静态HTML文件服务</li>
        <li>JSON API接口</li>
        <li>多客户端并发支持</li>
    </ul>
</body>
</html>
        )";
    }

    // 关于页面
    std::string getAboutPage() {
        return R"(
<!DOCTYPE html>
<html>
<head>
    <title>关于 - C++ HTTP服务器</title>
    <meta charset="UTF-8">
    <style>
        body { font-family: Arial, sans-serif; margin: 40px; }
        a { color: #0066cc; text-decoration: none; }
        a:hover { text-decoration: underline; }
    </style>
</head>
<body>
    <h1>关于这个项目</h1>
    <p>这是一个用C++编写的简单HTTP服务器，专为新手入门设计。</p>

    <h2>项目特点：</h2>
    <ul>
        <li>代码简洁易懂，约200行</li>
        <li>使用标准C++库，无需额外依赖</li>
        <li>支持基本的HTTP GET请求</li>
        <li>包含路由处理功能</li>
        <li>多线程支持</li>
    </ul>

    <p><a href="/">← 返回首页</a></p>
</body>
</html>
        )";
    }

    // API状态响应
    std::string getStatusResponse() {
        return R"({
    "status": "running",
    "server": "C++ Simple HTTP Server",
    "version": "1.0.0",
    "message": "服务器正常运行中"
})";
    }

public:
    SimpleHTTPServer(int port) : 
               port(port), 
               running(false), 
               threadPool(std::max<size_t> (2, std::thread::hardware_concurrency())) {}


    bool start() {
      if(!epollEngine.init(port, 10)){
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
    









