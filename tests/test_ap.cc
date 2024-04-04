#include "sylar/iomanager.h"
#include "sylar/log.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <stack>
#include <assert.h>
static int sock_listen_fd = -1;
void test_accept();

// auto logger = SYLAR_LOG_NAME("system");
auto logger = SYLAR_LOG_ROOT();

void error(char *msg) {
  perror(msg);
  printf("erreur...\n");
  exit(1);
}

// 重新监听
void watch_io_read() {
  sylar::IOManager::GetThis()->addEvent(sock_listen_fd, sylar::IOManager::READ,
                                        test_accept);
}
void test_accept() {
  struct sockaddr_in addr; // maybe sockaddr_un;
  memset
  (&addr, 0, sizeof(addr));
  socklen_t len = sizeof(addr);
  int fd = accept(sock_listen_fd, (struct sockaddr *)&addr, &len);
  if (fd < 0) {
    std::cout << "fd = " << fd << "accept false" << std::endl;
  } else {
    SYLAR_LOG_DEBUG(logger) << "accpet new client";
    // 处理请求
    // fcntl(fd, F_SETFL, O_NONBLOCK);
    sylar::IOManager::GetThis()->addEvent(fd, sylar::IOManager::READ, [fd]() {
      char buffer[1024];
      memset(buffer, 0, sizeof(buffer));
      while (true) {
        int ret = recv(fd, buffer, sizeof(buffer), 0);
        if (ret > 0) {
          ret = send(fd, buffer, ret, 0);
        }

        // if(ret <= 0){
        //   if(errno == EAGAIN){
        //     SYLAR_LOG_DEBUG(logger) << strerror(errno);
        //   }
        //   close(fd);
        //   break;
        // }
        if(ret == 0){
          close(fd);
          break;
        }
        if (ret < 0) {
          if (errno == EAGAIN)
            continue;
          close(fd);
          break;
        }
      }
    });
  }
  sylar::IOManager::GetThis()->schedule(watch_io_read);
}
void test_iomanager() {
  int portno = 8080;
  struct sockaddr_in server_addr;
  // socklen_t client_len = sizeof(client_addr);
  // setup socket
  sock_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (sock_listen_fd < 0) {
    error(const_cast<char*>("Error creating socket..\n"));
  }
  int yes = 1;
  // lose the pesky "address already in use" error message
  setsockopt(sock_listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  memset((char *)&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(portno);
  server_addr.sin_addr.s_addr = INADDR_ANY;
  // bind socket and listen for connections
  if (bind(sock_listen_fd, (struct sockaddr *)&server_addr,
           sizeof(server_addr)) < 0)
    error(const_cast<char*>("Error binding socket..\n"));
  if (listen(sock_listen_fd, 1024) < 0) {
    error(const_cast<char*>("Error listening..\n"));
  }
  printf("epoll echo server listening for connections on port: %d\n", portno);
  fcntl(sock_listen_fd, F_SETFL, O_NONBLOCK);
  sylar::IOManager iom;
  iom.addEvent(sock_listen_fd, sylar::IOManager::READ, test_accept);
}
int main(int argc, char *argv[]) {
  test_iomanager();
  return 0;
}