#include <fc/network/http/http.hpp>
#include <fc/thread/thread.hpp>
#include <fc/network/tcp_socket.hpp>
#include <fc/io/sstream.hpp>
#include <fc/network/ip.hpp>
#include <fc/io/stdio.hpp>
#include <fc/log/logger.hpp>
#include <fc/network/url.hpp>

#include <boost/system/error_code.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/io_service.hpp>

#include <string>
#include <mutex>
#include <memory>

namespace fc { namespace http {

  namespace detail {
    /// A handle to uniquely identify a connection
    typedef std::weak_ptr< void >                                  connection_hdl;

    /// Called once for every successful http connection attempt
    typedef std::function< void( connection_hdl& ) >               open_handler;

    /// Called once for every successfully established connection after it is no longer capable of sending or receiving new messages
    typedef std::function< void( connection_hdl& ) >               close_handler;

    /// Called once for every unsuccessful WebSocket connection attempt
    typedef std::function< void( connection_hdl& ) >               fail_handler;

    // Message payload type
    typedef std::string                                            message_type;

    /// Called after a new message has been received
    typedef std::function< void( connection_hdl&, message_type ) > message_handler;

    typedef boost::asio::ssl::context                              ssl_context;
    typedef std::shared_ptr< ssl_context >                         ssl_context_ptr;

    /// Called when needed to request a TLS context for the library to use
    typedef std::function< ssl_context_ptr( connection_hdl& ) >    tls_init_handler;

    typedef boost::asio::io_service*                               io_service_ptr;

    class connection_base
    {
    public:
      virtual ~connection_base() = default;

      virtual constexpr bool is_secure()const = 0;

      /// Close the connection
      void close( uint16_t code, const std::string& reason );

    protected:
      /// Handlers mutex
      std::mutex      m_mutex;
    };

    namespace tls {
      class connection : public connection_base
      {
      public:
        typedef boost::asio::ssl::stream< boost::asio::ip::tcp::socket > socket_type;

        virtual ~connection() = default;

        virtual constexpr bool is_secure()const { return true; }

        // Handlers //
        void set_tls_init_handler( tls_init_handler&& _handler )
        {
          std::lock_guard< std::mutex > _guard( m_mutex );
          m_tls_init_handler = _handler;
        }

      protected:
        tls_init_handler m_tls_init_handler;
      };
    } // tls

    namespace unsecure {
      class connection : public connection_base
      {
      public:
        typedef boost::asio::ip::tcp::socket socket_type;

        virtual ~connection() = default;

        virtual constexpr bool is_secure()const { return false; }

      protected:
      };
    } // unsecure

    template< typename ConnectionType >
    class endpoint : public ConnectionType
    {
    public:
      typedef ConnectionType                     connection_type;
      typedef std::shared_ptr< connection_type > connection_ptr;

      /// called after the socket object is created but before it is used
      typedef std::function< void( connection_hdl&, typename ConnectionType::socket_type& ) >
          socket_init_handler;

      virtual ~endpoint() = default;

      /// Retrieves a connection_ptr from a connection_hdl (exception free)
      connection_ptr get_con_from_hdl( connection_hdl& hdl )noexcept;

      /// initialize asio transport with external io_service (exception free)
      void init_asio( io_service_ptr ptr )noexcept;

      /// Sets whether to use the SO_REUSEADDR flag when opening listening sockets
      void set_reuse_addr( bool value );

      /// Check if the endpoint is listening
      bool is_listening()const;

      /// Stop listening
      void stop_listening();

      /// Set up endpoint for listening on a port
      void listen( uint16_t port );
      /// Set up endpoint for listening manually
      void listen( const boost::asio::ip::tcp::endpoint& ep );

      // Handlers //
      void set_open_handler( open_handler&& _handler )
      {
        std::lock_guard< std::mutex > _guard( connection_type::m_mutex );
        m_open_handler = _handler;
      }
      void set_message_handler( message_handler&& _handler )
      {
        std::lock_guard< std::mutex > _guard( connection_type::m_mutex );
        m_message_handler = _handler;
      }
      void set_close_handler( close_handler&& _handler )
      {
        std::lock_guard< std::mutex > _guard( connection_type::m_mutex );
        m_close_handler = _handler;
      }
      void set_fail_handler( fail_handler&& _handler )
      {
        std::lock_guard< std::mutex > _guard( connection_type::m_mutex );
        m_fail_handler = _handler;
      }
      void set_socket_init_handler( socket_init_handler&& _handler )
      {
        std::lock_guard< std::mutex > _guard( connection_type::m_mutex );
        m_socket_init_handler = _handler;
      }

    protected:
      open_handler        m_open_handler;
      message_handler     m_message_handler;
      close_handler       m_close_handler;
      fail_handler        m_fail_handler;
      socket_init_handler m_socket_init_handler;
    };

    template< typename ConnectionType >
    class client_endpoint : public endpoint< ConnectionType >, std::enable_shared_from_this< client_endpoint< ConnectionType > >
    {
    public:
      virtual ~client_endpoint() = default;

      /// Creates and returns a pointer to a new connection to the given URI suitable for passing to connect(connection_ptr)
      connection_ptr get_connection( const fc::url& _url, boost::system::error_code& ec );

      /// Initiates the opening connection handshake for connection con
      connection_ptr connect( connection_ptr& );

    protected:
    };

    template< typename ConnectionType >
    class server_endpoint : public endpoint< ConnectionType >, std::enable_shared_from_this< client_endpoint< ConnectionType > >
    {
    public:
      virtual ~server_endpoint() = default;

      /// Starts the server's async connection acceptance loop
      void start_accept();

      /// Create and initialize a new connection
      connection_ptr get_connection();

    protected:
    };

    template< typename ConnectionType >
    class http_connection_impl : public http_connection
    {
    public:
      typedef ConnectionType connection_type;

      http_connection_impl( connection_type con )
        : _http_connection( con )
      {}

      virtual ~http_connection_impl() = default;

      virtual void send_message( const std::string& message )override
      {
        idump((message));
        auto ec = _http_connection->send( message );
        FC_ASSERT( !ec, "http send failed: ${msg}", ("msg",ec.message() ) );
      }
      virtual void close( int64_t code, const std::string& reason )override
      {
          _http_connection->close(code,reason);
      }

      connection_type _http_connection;
    };

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
        : _client_thread( fc::thread::current() )
      {}

      fc::promise<void>::ptr  _connected;
      fc::url                 _url;

    private:
      fc::thread&             _client_thread;
    };

    class http_tls_client_impl
    {
    public:
      http_tls_client_impl( const std::string& ca_filename )
        : _client_thread( fc::thread::current() )
      {}

      fc::promise<void>::ptr  _connected;
      fc::url                 _url;

    private:
      fc::thread&             _client_thread;
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

  connection_ptr http_client::connect( const std::string& _url_str )
  { try {
    fc::url _url{ _url_str };
    FC_ASSERT( _url.proto() == "http", "Invalid protocol: \"{proto}\". Expected: \"http\"", ("proto", _url.proto()) );
    my->_url = _url;

    my->_connected = fc::promise<void>::ptr( new fc::promise<void>("http::connect") );

    boost::system::error_code ec;


    FC_ASSERT( !ec, "${con_desc}: Error: ${ec_msg}", ("con_desc",my->_connected->get_desc())("ec_msg",ec.message()) );

    my->_connected->wait();
    return nullptr;
  } FC_CAPTURE_AND_RETHROW( (_url_str) )}


  http_tls_client::http_tls_client( const std::string& ca_filename )
    : client( ca_filename ), my( new detail::http_tls_client_impl( ca_filename ) ) {}
  http_tls_client::~http_tls_client() {}

  connection_ptr http_tls_client::connect( const std::string& _url_str )
  { try {
    fc::url _url{ _url_str };
    FC_ASSERT( _url.proto() == "https", "Invalid protocol: \"{proto}\". Expected: \"https\"", ("proto", _url.proto()) );
    my->_url = _url;

    my->_connected = fc::promise<void>::ptr( new fc::promise<void>("https::connect") );

    boost::system::error_code ec;


    FC_ASSERT( !ec, "${con_desc}: Error: ${ec_msg}", ("con_desc",my->_connected->get_desc())("ec_msg",ec.message()) );

    my->_connected->wait();
    return nullptr;
  } FC_CAPTURE_AND_RETHROW( (_url_str) ) }

} } // fc::http
