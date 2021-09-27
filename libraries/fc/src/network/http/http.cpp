#include <fc/network/http/http.hpp>
#include <fc/thread/thread.hpp>
#include <fc/network/tcp_socket.hpp>
#include <fc/io/sstream.hpp>
#include <fc/network/ip.hpp>
#include <fc/io/stdio.hpp>
#include <fc/log/logger.hpp>

#include <string>

namespace fc { namespace http {

  namespace detail {
    class http_server_impl
    {
    public:
      http_server_impl()
      {}
    };

    class http_tls_server_impl
    {
    public:
      http_tls_server_impl( const std::string& server_pem, const std::string& ssl_password )
      {}
    };

    class http_client_impl
    {
    public:
      http_client_impl()
      {}
    };

    class http_tls_client_impl
    {
    public:
      http_tls_client_impl( const std::string& ca_filename )
      {}
    };
  } // namespace detail

  http_server::http_server()
      : server(), my( new detail::http_server_impl() ) {}
  http_server::~http_server() {}

  void http_server::on_connection( const on_connection_handler& handler )
  {}
  void http_server::listen( uint16_t port )
  {}
  void http_server::listen( const fc::ip::endpoint& ep )
  {}
  void http_server::start_accept()
  {}


  http_tls_server::http_tls_server( const std::string& server_pem, const std::string& ssl_password )
    : server( server_pem, ssl_password ), my( new detail::http_tls_server_impl(server_pem, ssl_password) ) {}
  http_tls_server::~http_tls_server() {}

  void http_tls_server::on_connection( const on_connection_handler& handler )
  {}
  void http_tls_server::listen( uint16_t port )
  {}
  void http_tls_server::listen( const fc::ip::endpoint& ep )
  {}
  void http_tls_server::start_accept()
  {}


  http_client::http_client()
    : client(), my( new detail::http_client_impl() ) {}
  http_client::~http_client() {}

  connection_ptr http_client::connect( const std::string& uri )
  { try {
    FC_ASSERT( uri.substr(0,5) == "http:" );

    return nullptr;
  } FC_CAPTURE_AND_RETHROW( (uri) )}


  http_tls_client::http_tls_client( const std::string& ca_filename )
    : client( ca_filename ), my( new detail::http_tls_client_impl( ca_filename ) ) {}
  http_tls_client::~http_tls_client() {}

  connection_ptr http_tls_client::connect( const std::string& uri )
  { try {
    FC_ASSERT( uri.substr(0,6) == "https:" );

    return nullptr;
  } FC_CAPTURE_AND_RETHROW( (uri) ) }

} } // fc::http
