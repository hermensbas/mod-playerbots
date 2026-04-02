/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "PlayerbotCommandServer.h"

#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/thread/thread.hpp>
#include <cstdlib>
#include <future>
#include "RandomPlayerbotMgr.h"
#include "PlayerbotWorldThreadProcessor.h"

#include "IoContext.h"

using boost::asio::ip::tcp;
typedef boost::shared_ptr<tcp::socket> socket_ptr;

namespace
{
class RemoteCommandOperation final : public PlayerbotOperation
{
public:
    RemoteCommandOperation(std::string request, std::shared_ptr<std::promise<std::string>> resultPromise)
        : _request(std::move(request)), _resultPromise(std::move(resultPromise))
    {
    }

    bool Execute() override
    {
        if (!_resultPromise)
            return false;

        try
        {
            _resultPromise->set_value(RandomPlayerbotMgr::instance().HandleRemoteCommand(_request));
            return true;
        }
        catch (std::exception const& e)
        {
            _resultPromise->set_value(std::string("remote command failed: ") + e.what());
            return false;
        }
        catch (...)
        {
            _resultPromise->set_value("remote command failed: unknown exception");
            return false;
        }
    }

    uint32 GetPriority() const override { return 20; }
    std::string GetName() const override { return "RemoteCommand"; }

private:
    std::string _request;
    std::shared_ptr<std::promise<std::string>> _resultPromise;
};

std::string DispatchRemoteCommand(std::string const& request)
{
    auto resultPromise = std::make_shared<std::promise<std::string>>();
    std::future<std::string> resultFuture = resultPromise->get_future();

    if (!PlayerbotWorldThreadProcessor::instance().QueueOperation(
            std::make_unique<RemoteCommandOperation>(request, resultPromise)))
    {
        return "remote command queue is full";
    }

    if (resultFuture.wait_for(std::chrono::seconds(10)) != std::future_status::ready)
        return "remote command timed out waiting for world thread";

    return resultFuture.get();
}
}

bool ReadLine(socket_ptr sock, std::string* buffer, std::string* line)
{
    // Do the real reading from fd until buffer has '\n'.
    std::string::iterator pos;
    while ((pos = find(buffer->begin(), buffer->end(), '\n')) == buffer->end())
    {
        char buf[1025];
        boost::system::error_code error;
        size_t n = sock->read_some(boost::asio::buffer(buf), error);
        if (n == -1 || error == boost::asio::error::eof)
            return false;
        else if (error)
            throw boost::system::system_error(error);  // Some other error.

        buf[n] = 0;
        *buffer += buf;
    }

    *line = std::string(buffer->begin(), pos);
    *buffer = std::string(pos + 1, buffer->end());
    return true;
}

void session(socket_ptr sock)
{
    try
    {
        std::string buffer, request;
        while (ReadLine(sock, &buffer, &request))
        {
            std::string const response = DispatchRemoteCommand(request) + "\n";
            boost::asio::write(*sock, boost::asio::buffer(response.c_str(), response.size()));
            request = "";
        }
    }
    catch (std::exception& e)
    {
        LOG_ERROR("playerbots", "{}", e.what());
    }
}

void server(Acore::Asio::IoContext& io_service, short port)
{
    tcp::acceptor a(io_service, tcp::endpoint(tcp::v4(), port));
    for (;;)
    {
        socket_ptr sock(new tcp::socket(io_service));
        a.accept(*sock);
        boost::thread t(boost::bind(session, sock));
    }
}

void Run()
{
    if (!sPlayerbotAIConfig.commandServerPort)
    {
        return;
    }

    std::ostringstream s;
    s << "Starting Playerbots Command Server on port " << sPlayerbotAIConfig.commandServerPort;
    LOG_INFO("playerbots", "{}", s.str().c_str());

    try
    {
        Acore::Asio::IoContext io_service;
        server(io_service, sPlayerbotAIConfig.commandServerPort);
    }

    catch (std::exception& e)
    {
        LOG_ERROR("playerbots", "{}", e.what());
    }
}

void PlayerbotCommandServer::Start()
{
    std::thread serverThread(Run);
    serverThread.detach();
}
