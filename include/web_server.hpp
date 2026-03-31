#ifndef WEB_SERVER_HPP
#define WEB_SERVER_HPP

#include <cstdint>
#include <string>

class PortfolioApiServer
{
private:
    std::string data_directory;
    uint16_t port;

public:
    PortfolioApiServer(std::string data_dir, uint16_t listen_port);

    bool start();
};

#endif
