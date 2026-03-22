#include <asm-generic/socket.h>
#include<iostream>
#include<cstring>
#include<sys/socket.h>
#include<netinet/in.h>
#include<unistd.h>

const int PORT = 6379;
const int BACKLOG = 10;

int main(){
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd<0){
        std::cerr <<"Failed to create socket\n";
        return 1;
    }
    std::cout<<"Socket created, fd - "<<server_fd<<"\n";

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (sockaddr*)&address, sizeof(address)) < 0 ){
        std::cerr << "Bind failed\n";
        return 1;
    }
    std::cout<<"Bound to port "<< PORT <<"\n";

    if (listen(server_fd, BACKLOG)<0){
        std::cerr<<"Listen failed\n";
        return 1;
    }

    std::cout<<"Listening... waiting for a client\n";

    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);

    int client_fd = accept(server_fd, (sockaddr*)&client_addr, &client_len);
    if (client_fd < 0){
        std::cerr<<"Accept failed\n";
        return 1;
    }
    std::cout<<"Client connected! client_fd = "<<client_fd<<"\n";

    const char* response = "+PONG\r\n";
    send(client_fd, response, strlen(response), 0);
    std::cout<<"Sent: +PONG\\r\\n\n";

    close(client_fd);
    close(server_fd);
    std::cout<<"Done.\n";
    return 0;
}