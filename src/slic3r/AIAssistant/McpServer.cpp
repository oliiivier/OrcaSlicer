#include "McpServer.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#endif

namespace Slic3r { namespace AIAssistant {

namespace asio  = boost::asio;
namespace beast = boost::beast;
namespace http  = beast::http;
using tcp       = asio::ip::tcp;
using nlohmann::json;

static const char* MCP_PATH            = "/mcp";
static const char* LATEST_PROTOCOL     = "2025-06-18";
static const char* SUPPORTED_PROTOCOLS[] = { "2025-11-25", "2025-06-18", "2025-03-26" };

// JSON-RPC error codes
static constexpr int PARSE_ERROR      = -32700;
static constexpr int INVALID_REQUEST  = -32600;
static constexpr int METHOD_NOT_FOUND = -32601;
static constexpr int INVALID_PARAMS   = -32602;
static constexpr int INTERNAL_ERROR   = -32603;

// Child processes (WebKit's web processes, started after the server) must not
// inherit the server sockets: they would keep the port in use after a stop.
template<class Socket> static void not_inherited(Socket& socket)
{
#ifdef _WIN32
    ::SetHandleInformation(reinterpret_cast<HANDLE>(socket.native_handle()), HANDLE_FLAG_INHERIT, 0);
#else
    ::fcntl(socket.native_handle(), F_SETFD, FD_CLOEXEC);
#endif
}

static json rpc_error(const json& id, int code, const std::string& message)
{
    return { { "jsonrpc", "2.0" }, { "id", id }, { "error", { { "code", code }, { "message", message } } } };
}

static json rpc_result(const json& id, json result)
{
    return { { "jsonrpc", "2.0" }, { "id", id }, { "result", std::move(result) } };
}

class McpSession;

struct McpServer::Impl
{
    // Declared first so it is destroyed last: pending handlers own sessions bound to it.
    asio::io_context ioc;
    tcp::acceptor    acceptor { ioc };
    std::thread      thread;

    void do_accept(McpServer& server);
};

// One keep-alive HTTP connection.
class McpSession : public std::enable_shared_from_this<McpSession>
{
public:
    McpSession(tcp::socket socket, McpServer& server) : m_stream(std::move(socket)), m_server(server) {}

    void run() { do_read(); }

private:
    using Request  = http::request<http::string_body>;
    using Response = http::response<http::string_body>;

    void do_read()
    {
        m_req = {};
        m_stream.expires_after(std::chrono::minutes(10));
        http::async_read(m_stream, m_buffer, m_req, [self = shared_from_this()](beast::error_code ec, std::size_t) {
            self->on_read(ec);
        });
    }

    void on_read(beast::error_code ec)
    {
        if (ec) {
            if (ec != http::error::end_of_stream && ec != beast::error::timeout && ec != asio::error::operation_aborted)
                BOOST_LOG_TRIVIAL(debug) << "MCP: read error: " << ec.message();
            close();
            return;
        }

        auto res = std::make_shared<Response>(handle(m_req));
        http::async_write(m_stream, *res, [self = shared_from_this(), res](beast::error_code ec, std::size_t) {
            if (ec || !res->keep_alive())
                self->close();
            else
                self->do_read();
        });
    }

    void close()
    {
        beast::error_code ec;
        m_stream.socket().shutdown(tcp::socket::shutdown_send, ec);
    }

    Response make_response(http::status status, std::string body = {}, const char* content_type = "application/json")
    {
        Response res { status, m_req.version() };
        res.keep_alive(m_req.keep_alive());
        if (!body.empty())
            res.set(http::field::content_type, content_type);
        res.body() = std::move(body);
        res.prepare_payload();
        return res;
    }

    // Guards against DNS rebinding: browsers always send Origin, and the Host
    // header of a rebound request carries the attacker's domain.
    bool is_local_request() const
    {
        auto is_local = [this](beast::string_view sv, bool with_scheme) {
            const std::string value(sv);
            const std::string port = ":" + std::to_string(m_server.port());
            for (const char* host : { "127.0.0.1", "localhost", "[::1]" }) {
                const std::string base = std::string(with_scheme ? "http://" : "") + host;
                if (value == base || value == base + port)
                    return true;
            }
            return false;
        };

        const auto origin = m_req.find(http::field::origin);
        if (origin != m_req.end() && !is_local(origin->value(), true))
            return false;
        const auto host = m_req.find(http::field::host);
        return host == m_req.end() || is_local(host->value(), false);
    }

    Response handle(const Request& req)
    {
        if (req.target() != MCP_PATH)
            return make_response(http::status::not_found);
        if (!is_local_request())
            return make_response(http::status::forbidden);
        if (std::string(req[http::field::authorization]) != "Bearer " + m_server.m_token)
            return make_response(http::status::unauthorized);

        if (req.method() != http::verb::post) {
            // No server-initiated SSE stream and no sessions to delete.
            auto res = make_response(http::status::method_not_allowed);
            res.set(http::field::allow, "POST");
            return res;
        }

        json msg;
        try {
            msg = json::parse(req.body());
        } catch (const json::exception& e) {
            return make_response(http::status::bad_request, rpc_error(nullptr, PARSE_ERROR, e.what()).dump());
        }
        if (!msg.is_object())
            return make_response(http::status::bad_request,
                                 rpc_error(nullptr, INVALID_REQUEST, "Expected a single JSON-RPC message").dump());

        const json reply = m_server.handle_message(msg);
        if (reply.is_null())
            return make_response(http::status::accepted);
        // ensure_ascii avoids any charset ambiguity on the client side.
        return make_response(http::status::ok, reply.dump(-1, ' ', true));
    }

    beast::tcp_stream  m_stream;
    beast::flat_buffer m_buffer;
    Request            m_req;
    McpServer&         m_server;
};

void McpServer::Impl::do_accept(McpServer& server)
{
    acceptor.async_accept([this, &server](beast::error_code ec, tcp::socket socket) {
        if (ec == asio::error::operation_aborted)
            return;
        if (!ec) {
            not_inherited(socket);
            std::make_shared<McpSession>(std::move(socket), server)->run();
        }
        do_accept(server);
    });
}

McpServer::McpServer(unsigned short port, std::string token, McpToolbox& toolbox)
    : m_port(port), m_token(std::move(token)), m_toolbox(toolbox)
{}

McpServer::~McpServer() { stop(); }

std::string McpServer::url() const { return "http://127.0.0.1:" + std::to_string(m_port) + MCP_PATH; }

bool McpServer::is_running() const { return m_impl != nullptr; }

bool McpServer::start(std::string& error)
{
    if (m_impl)
        return true;
    if (m_token.empty()) {
        error = "empty token";
        return false;
    }

    auto impl = std::make_unique<Impl>();
    try {
        const tcp::endpoint endpoint { asio::ip::make_address("127.0.0.1"), m_port };
        impl->acceptor.open(endpoint.protocol());
        not_inherited(impl->acceptor);
#ifndef _WIN32
        // Rebind right after a stop despite the connections left in TIME_WAIT. Not on
        // Windows, where this option would let another program bind the same port.
        impl->acceptor.set_option(tcp::acceptor::reuse_address(true));
#endif
        impl->acceptor.bind(endpoint);
        impl->acceptor.listen();
    } catch (const boost::system::system_error& e) {
        error = e.code().message();
        return false;
    }

    impl->do_accept(*this);
    impl->thread = std::thread([ioc = &impl->ioc]() { ioc->run(); });

    m_impl = std::move(impl);
    BOOST_LOG_TRIVIAL(info) << "MCP: server listening on " << url();
    return true;
}

// Must not be called while a toolbox call is blocked waiting on the calling thread:
// the toolbox has to be cancelled first (see McpTools::cancel()).
void McpServer::stop()
{
    if (!m_impl)
        return;
    m_impl->ioc.stop();
    if (m_impl->thread.joinable())
        m_impl->thread.join();
    m_impl.reset();
    BOOST_LOG_TRIVIAL(info) << "MCP: server stopped";
}

json McpServer::handle_message(const json& msg)
{
    const bool is_request = msg.contains("id") && msg.contains("method");
    if (!is_request)
        return nullptr; // notification (e.g. notifications/initialized) or client response

    const json&       id     = msg["id"];
    const std::string method = msg["method"].is_string() ? msg["method"].get<std::string>() : std::string();
    const json        params = msg.value("params", json::object());

    try {
        if (method == "initialize") {
            const std::string requested = params.value("protocolVersion", "");
            const bool        supported = std::any_of(std::begin(SUPPORTED_PROTOCOLS), std::end(SUPPORTED_PROTOCOLS),
                                                      [&requested](const char* v) { return requested == v; });
            json capabilities = { { "tools", json::object() } };
            if (!m_toolbox.list_prompts().empty())
                capabilities["prompts"] = json::object();
            json result = { { "protocolVersion", supported ? requested : LATEST_PROTOCOL },
                            { "capabilities", std::move(capabilities) },
                            { "serverInfo", { { "name", "snapmaker-orca" }, { "version", "0.2.0" } } } };
            if (const std::string instructions = m_toolbox.instructions(); !instructions.empty())
                result["instructions"] = instructions;
            return rpc_result(id, std::move(result));
        }
        if (method == "ping")
            return rpc_result(id, json::object());
        if (method == "tools/list")
            return rpc_result(id, { { "tools", m_toolbox.list_tools() } });
        if (method == "tools/call") {
            if (!params.contains("name") || !params["name"].is_string())
                return rpc_error(id, INVALID_PARAMS, "Missing tool name");
            return rpc_result(id, m_toolbox.call_tool(params["name"].get<std::string>(),
                                                      params.value("arguments", json::object())));
        }
        if (method == "prompts/list")
            return rpc_result(id, { { "prompts", m_toolbox.list_prompts() } });
        if (method == "prompts/get") {
            if (!params.contains("name") || !params["name"].is_string())
                return rpc_error(id, INVALID_PARAMS, "Missing prompt name");
            return rpc_result(id, m_toolbox.get_prompt(params["name"].get<std::string>(),
                                                       params.value("arguments", json::object())));
        }
        return rpc_error(id, METHOD_NOT_FOUND, "Method not found: " + method);
    } catch (const std::invalid_argument& e) {
        return rpc_error(id, INVALID_PARAMS, e.what());
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "MCP: " << method << " failed: " << e.what();
        return rpc_error(id, INTERNAL_ERROR, e.what());
    }
}

}} // namespace Slic3r::AIAssistant
