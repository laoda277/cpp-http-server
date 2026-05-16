#pragma once

#include <functional>

class EpollEngine{

public:

    EpollEngine();
    ~EpollEngine();

    using ClientHandler = std::function<void (int)>;
    bool init(int port, int backlog = 128);
    bool run(const ClientHandler& onClientAccepted);
    bool stop();
    
    int listeningSocket() const;


private:

    int serverSocket_;
    int epollFd_;
    bool running_;

    bool setNonBlocking(int fd);
    bool createListeningSocket(int port, int backlog);
    bool setupEpoll();
    bool cleanup();

};