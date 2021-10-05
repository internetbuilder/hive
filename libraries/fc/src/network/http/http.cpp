#include <fc/network/http/http.hpp>
#include <fc/thread/thread.hpp>
#include <fc/network/tcp_socket.hpp>
#include <fc/io/sstream.hpp>
#include <fc/network/ip.hpp>
#include <fc/io/stdio.hpp>
#include <fc/log/logger.hpp>
#include <fc/network/url.hpp>
#include <fc/asio.hpp>

#include <boost/system/error_code.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/rfc2818_verification.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/asio/steady_timer.hpp>

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

    /// acceptor being used
    typedef boost::asio::ip::tcp::acceptor                         acceptor_type;
    typedef std::shared_ptr< acceptor_type >                       acceptor_ptr;

    typedef boost::asio::io_service::strand                        strand_type;
    /// Type of a pointer to the Asio io_service::strand being used
    typedef std::shared_ptr< strand_type >                         strand_ptr;
    /// Type of a pointer to the Asio timer class
    typedef std::shared_ptr< boost::asio::steady_timer >           timer_ptr;

    /// The type and signature of the callback passed to the accept method
    typedef std::function< void( const boost::system::error_code& )> accept_handler;

    typedef std::function< void( const boost::system::error_code& )> shutdown_handler;

    class http_processor;
    typedef std::shared_ptr< http_processor >                      processor_ptr;

    enum class connection_state
    {
      uninitialized = 0,
      ready         = 1,
      reading       = 2
    };

    enum class endpoint_state
    {
      uninitialized = 0,
      ready         = 1,
      listening     = 2
    };

    enum class session_state
    {
      connecting = 0,
      open       = 1,
      closing    = 2,
      closed     = 3
    };

    enum class internal_state
    {
      user_init            = 0,
      transport_init       = 1,
      read_http_request    = 2,
      write_http_request   = 3,
      read_http_response   = 4,
      write_http_response  = 5,
      process_http_request = 6,
      process_connection   = 7
    };

    enum class terminate_status
    {
        failed  = 0,
        closed  = 1,
        unknown = 2
    };

    struct config
    {
      static constexpr bool enable_multithreading = true;

      /// Maximum size of close frame reason
      static constexpr uint8_t close_reason_size = 123;

      static constexpr float client_version = 1.1;
    };

    class http_processor
    {
    public:
      virtual ~http_processor() {}
    };

    namespace http_1_1 {
      class processor : public http_processor
      {
      public:
        virtual ~processor() {}
      };
    } // http_1_1

    class connection_base
    {
    public:
      typedef boost::asio::ip::tcp::socket   socket_type;
      typedef std::shared_ptr< socket_type > socket_ptr;

      virtual ~connection_base() {}

      virtual constexpr bool is_secure()const = 0;
      virtual void init_asio ( io_service_ptr service ) = 0;
      virtual socket_type& get_socket() = 0;
      virtual void async_shutdown( shutdown_handler hdl ) = 0;

      /// Close the connection
      void close( uint16_t code, const message_type& reason );

      void send(const message_type& payload );

      /// Set Connection Handle
      void set_handle( connection_hdl hdl )
      {
        m_hdl = hdl;
      }

    protected:
      /// Current connection state
      session_state       m_session_state;
      endpoint_state      m_endpoint_state;
      connection_state    m_connection_state;
      internal_state      m_internal_state;

      /// Handlers mutex
      std::mutex          m_handlers_mutex;
      std::mutex          m_connection_state_lock;

      timer_ptr           m_handshake_timer;

      connection_hdl      m_hdl;
      io_service_ptr      m_io_service;
      acceptor_ptr        m_acceptor;
      strand_ptr          m_strand;
    };

    namespace tls {
      class connection : public connection_base
      {
      public:
        typedef boost::asio::ssl::stream< boost::asio::ip::tcp::socket > socket_type;
        typedef std::shared_ptr< socket_type >                           socket_ptr;

        /// called after the socket object is created but before it is used
        typedef std::function< void( connection_hdl&, socket_type& ) >
            socket_init_handler;

        virtual ~connection() {}

        virtual constexpr bool is_secure()const override { return true; }

        // Handlers //
        void set_tls_init_handler( tls_init_handler&& _handler )
        {
          std::lock_guard< std::mutex > _guard( m_handlers_mutex );
          m_tls_init_handler = _handler;
        }

        /// Retrieve a reference to the wrapped socket
        virtual connection_base::socket_type& get_socket() override
        {
          return m_socket->next_layer();
        }

        virtual void async_shutdown( shutdown_handler hdl ) override
        {
          if ( m_strand )
            m_socket->async_shutdown( m_strand->wrap(hdl) );
          else
            m_socket->async_shutdown( hdl );
        }

        /// initialize asio transport with external io_service
        virtual void init_asio ( io_service_ptr service ) override
        {
          FC_ASSERT( connection_base::m_connection_state == connection_state::uninitialized, "Invalid state" );

          FC_ASSERT( m_tls_init_handler, "Missing tls init handler" );

          m_io_service = service;

          m_acceptor = std::make_shared< acceptor_type >(*service);

          if ( config::enable_multithreading )
            m_strand = std::make_shared< strand_type >( *service );

          m_context = m_tls_init_handler(m_hdl);
          FC_ASSERT( m_context, "Invalid tls context" );

          m_socket = std::make_shared< socket_type >(*service, *m_context);

          if ( m_socket_init_handler )
            m_socket_init_handler(m_hdl, *m_socket);

          if ( m_socket_init_handler )
            m_socket_init_handler( m_hdl, *m_socket );

          m_connection_state = connection_state::ready;
        }

      protected:
        // Handlers
        socket_init_handler m_socket_init_handler;
        tls_init_handler    m_tls_init_handler;

        socket_ptr          m_socket;
        ssl_context_ptr     m_context;
      };
    } // tls

    namespace unsecure {
      class connection : public connection_base
      {
      public:
        typedef boost::asio::ip::tcp::socket   socket_type;
        typedef std::shared_ptr< socket_type > socket_ptr;

        /// called after the socket object is created but before it is used
        typedef std::function< void( connection_hdl&, socket_type& ) >
            socket_init_handler;

        virtual ~connection() {}

        virtual constexpr bool is_secure()const override { return false; }

        /// Retrieve a reference to the socket
        virtual connection_base::socket_type& get_socket() override
        {
            return *m_socket;
        }

        /// initialize asio transport with external io_service
        virtual void init_asio ( io_service_ptr service ) override
        {
          FC_ASSERT( connection_base::m_connection_state == connection_state::uninitialized, "Invalid state" );

          m_io_service = service;

          m_acceptor = std::make_shared< acceptor_type >(*service);

          if ( config::enable_multithreading )
            m_strand = std::make_shared< strand_type >(*service);

          m_socket = std::make_shared< socket_type >(*service);

          if ( m_socket_init_handler )
            m_socket_init_handler( m_hdl, *m_socket );

          m_connection_state = connection_state::ready;
        }

        virtual void async_shutdown( shutdown_handler hdl ) override
        {
          boost::system::error_code ec;
          m_socket->shutdown( boost::asio::ip::tcp::socket::shutdown_both, ec );
          FC_ASSERT( !ec, "async shutdown error: ${err}", ("err",ec.message()) );
        }

      protected:
        // Handlers
        socket_init_handler m_socket_init_handler;

        socket_ptr          m_socket;
      };
    } // unsecure

    template< typename ConnectionType >
    class endpoint : public ConnectionType
    {
    public:
      typedef ConnectionType                     connection_type;
      typedef std::shared_ptr< connection_type > connection_ptr;

      virtual ~endpoint() {}

      /// Retrieves a connection_ptr from a connection_hdl
      connection_ptr get_con_from_hdl( connection_hdl& hdl )
      {
        connection_ptr con = std::static_pointer_cast< connection_type >( hdl.lock() );
        FC_ASSERT( con, "Bad connection" );
        return con;
      }

      /// Sets whether to use the SO_REUSEADDR flag when opening listening sockets
      void set_reuse_addr( bool value )
      {
        m_reuse_addr = value;
      }

      void start()
      {
        if ( connection_type::m_internal_state != internal_state::user_init )
        {
          terminate( boost::system::errc::make_error_code( boost::system::errc::already_connected ));
          return;
        }

        connection_type::m_internal_state = internal_state::transport_init;

        m_processor = get_processor( config::client_version );

        // At this point the transport is ready to read and write bytes.
        if ( m_is_server )
        {
          connection_type::m_internal_state = internal_state::read_http_request;
          // TODO: read_handshake( 1 );
        }
        else
        {
          // We are a client. Set the processor to the version specified in the
          // config file and send a handshake request.
          connection_type::m_internal_state = internal_state::write_http_request;
          // TODO: send_http_request();
        }
      }

      processor_ptr get_processor( float client_version )
      {
        switch( client_version )
        {
          case 1.1:
            return std::make_shared< http_1_1::processor >();
          default:
            FC_ASSERT( false, "Unimplemented http processor for version HTTP/${type}", ("type",client_version) );
        }
      }

      void terminate( const boost::system::error_code& ec )
      {
        // Cancel close handshake timer
        if ( connection_type::m_handshake_timer )
        {
          connection_type::m_handshake_timer->cancel();
          connection_type::m_handshake_timer.reset();
        }

        terminate_status tstat = terminate_status::unknown;

        if( connection_type::m_session_state == connection_type::session_state::connecting)
        {
          connection_type::m_session_state = session_state::closed;
          tstat = terminate_status::failed;
        }
        else if( connection_type::m_session_state != session_state::closed )
        {
          connection_type::m_session_state = session_state::closed;
          tstat = terminate_status::closed;
        }
        else return;

        connection_type::async_shutdown(
          std::bind(
            &handle_terminate,
            this,
            tstat,
            std::placeholders::_1
          )
        );
      }

      void handle_terminate( terminate_status tstat, const boost::system::error_code& ec )
      {
        if ( ec )
          elog( "asio::handle_terminate error: ${err}", ("err",ec.message()) );

        // clean shutdown
        switch( tstat )
        {
        case terminate_status::failed:
          if( connection_type::m_fail_handler )
              connection_type::m_fail_handler( connection_type::m_hdl );
          break;
        case terminate_status::closed:
          if( connection_type::m_close_handler )
              connection_type::m_close_handler( connection_type::m_hdl );
          break;
        default:
          break;
        }

        // call the termination handler if it exists
        // if it exists it might (but shouldn't) refer to a bad memory location.
        // If it does, we don't care and should catch and ignore it.
        if ( connection_type::m_termination_handler)
          try {
            connection_type::m_termination_handler( this );
          } FC_CAPTURE_AND_LOG( () );
      }

      /// Check if the endpoint is listening
      bool is_listening()const
      {
        return (connection_type::m_endpoint_state == connection_type::endpoint_state::listening);
      }

      /// Stop listening
      void stop_listening()
      {
        FC_ASSERT( connection_type::m_endpoint_state == endpoint_state::listening, "asio::listen called from the wrong state" );

        connection_type::m_acceptor->close();
        connection_type::m_endpoint_state = endpoint_state::listening;
      }

      /// Set up endpoint for listening on a port
      void listen( uint16_t port )
      {
        listen( typename boost::asio::ip::tcp::endpoint{ boost::asio::ip::tcp::v6(), port } );
      }
      /// Set up endpoint for listening manually
      void listen( const boost::asio::ip::tcp::endpoint& ep )
      {
        FC_ASSERT( connection_type::m_endpoint_state == endpoint_state::ready, "asio::listen called from the wrong state" );
        static const auto check_error = [&]( const boost::system::error_code& ec )
        {
          FC_ASSERT( !ec, "asio::listen error: ${err}", ("err", clean_up_listen_after_error( ec ).message()) );
        };

        boost::system::error_code ec;

        connection_type::m_acceptor->open( ep.protocol(), ec );
        check_error( ec );

        connection_type::m_acceptor->set_option( boost::asio::socket_base::reuse_address( m_reuse_addr ), ec );
        check_error( ec );

        connection_type::m_acceptor->bind( ep, ec );
        check_error( ec );

        connection_type::m_endpoint_state = endpoint_state::listening;
      }

      // Handlers //
      void set_open_handler( open_handler&& _handler )
      {
        std::lock_guard< std::mutex > _guard( connection_type::m_handlers_mutex );
        m_open_handler = _handler;
      }
      void set_message_handler( message_handler&& _handler )
      {
        std::lock_guard< std::mutex > _guard( connection_type::m_handlers_mutex );
        m_message_handler = _handler;
      }
      void set_close_handler( close_handler&& _handler )
      {
        std::lock_guard< std::mutex > _guard( connection_type::m_handlers_mutex );
        m_close_handler = _handler;
      }
      void set_fail_handler( fail_handler&& _handler )
      {
        std::lock_guard< std::mutex > _guard( connection_type::m_handlers_mutex );
        m_fail_handler = _handler;
      }

    protected:
      open_handler        m_open_handler;
      message_handler     m_message_handler;
      close_handler       m_close_handler;
      fail_handler        m_fail_handler;

      processor_ptr       m_processor;

      bool                m_reuse_addr;
      bool                m_is_server;

      boost::system::error_code clean_up_listen_after_error( boost::system::error_code& ec )
      {
        if ( connection_type::m_acceptor->is_open() )
            connection_type::m_acceptor->close();
        return ec;
      }
    };

    template< typename ConnectionType >
    class client_endpoint : public endpoint< ConnectionType >
    {
    public:
      typedef ConnectionType                     connection_type;
      typedef std::shared_ptr< connection_type > connection_ptr;
      typedef endpoint< connection_type >        endpoint_type;
      typedef std::shared_ptr< endpoint_type >   endpoint_ptr;

      virtual ~client_endpoint() {}

      /// Create and initialize a new connection. You should then call connect() in order to perform a handshake
      void create_connection( const fc::url& _url, boost::system::error_code& ec );

      /// Initiates the opening connection handshake for connection
      void connect();

    protected:
    };

    template< typename ConnectionType >
    class server_endpoint : public endpoint< ConnectionType >
    {
    public:
      typedef ConnectionType                     connection_type;
      typedef std::shared_ptr< connection_type > connection_ptr;
      typedef endpoint< connection_type >        endpoint_type;
      typedef std::shared_ptr< endpoint_type >   endpoint_ptr;

      virtual ~server_endpoint() {}

      /// Starts the server's async connection acceptance loop
      void start_accept()
      {
        FC_ASSERT( endpoint_type::is_listening(), "Not listening" );

        connection_type::m_is_server = true;

        boost::system::error_code ec;

        async_accept( std::bind( &handle_accept, this, std::placeholders::_1 ), ec );

        // If the connection was constructed but the accept failed,
        // terminate the connection to prevent memory leaks
        if( ec ) endpoint_type::terminate( ec );
      }

      /// Create and initialize a new connection
      void create_connection();

    protected:
      /// Accept the next connection attempt and assign it to con
      void async_accept( accept_handler callback, boost::system::error_code & ec )
      {
        if ( !endpoint_type::m_acceptor)
        {
          ec = boost::system::errc::make_error_code( boost::system::errc::not_connected );
          return;
        }

        if ( config::enable_multithreading )
        {
          endpoint_type::m_acceptor->async_accept(
            endpoint_type::get_socket(),
            endpoint_type::m_strand->wrap(std::bind(
              &handle_accept,
              this,
              callback,
              std::placeholders::_1
            ))
          );
        }
        else
        {
          endpoint_type::m_acceptor->async_accept(
            endpoint_type::get_socket(),
            std::bind(
              &handle_accept,
              this,
              callback,
              std::placeholders::_1
            )
          );
        }
      }

      /// Handler callback for start_accept
      void handle_accept( const boost::system::error_code& ec ) {
        if (ec) {
          endpoint_type::terminate(ec);
          edump((ec.message()));
        } else {
          endpoint_type::start();
        }

        start_accept();
      }
    };

    template< typename ConnectionType >
    class http_connection_impl : public http_connection
    {
    public:
      typedef ConnectionType connection_type;

      http_connection_impl( connection_type con )
        : _http_connection( con )
      {}

      virtual ~http_connection_impl() {};

      virtual void send_message( const std::string& message )override
      {
        idump((message));
        _http_connection->send( message );
      }
      virtual void close( int64_t code, const std::string& reason )override
      {
        _http_connection->close( code,reason );
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
    private:
      typedef std::shared_ptr< unsecure::connection >  con_impl_ptr;

    public:
      typedef client_endpoint< unsecure::connection >  endpoint_type;
      typedef std::shared_ptr< endpoint_type >         endpoint_ptr;
      typedef http_connection_impl< con_impl_ptr >     con_type;
      typedef std::shared_ptr< con_type >              con_ptr;

      http_client_impl()
        : _client_thread( fc::thread::current() )
      {
        _client.set_message_handler( [&]( connection_hdl hdl, message_type msg ){
          _client_thread.async( [&](){
            wdump((msg));
            fc::async( [=](){ if( _connection ) _connection->on_message(msg); });
          }).wait();
        });
        _client.set_close_handler( [=]( connection_hdl hdl ){
          _client_thread.async( [&](){ if( _connection ) {_connection->closed(); _connection.reset();} } ).wait();
          if( _closed ) _closed->set_value();
        });
        _client.set_fail_handler( [=]( connection_hdl hdl ){
          auto con = _client.get_con_from_hdl(hdl);
          if( _connection )
            _client_thread.async( [&](){ if( _connection ) _connection->closed(); _connection.reset(); } ).wait();
          if( _closed )
              _closed->set_value();
        });
        _client.init_asio( &fc::asio::default_io_service() );
      }
      ~http_client_impl()
      {
        if( _connection )
        {
          _connection->close( 0, "client closed" );
          _connection.reset();
          _closed->wait();
        }
      }

      fc::promise<void>::ptr  _connected;
      fc::promise<void>::ptr  _closed;
      endpoint_type           _client;
      con_ptr                 _connection;
      fc::url                 _url;

    private:
      fc::thread&             _client_thread;
    };

    class http_tls_client_impl
    {
    private:
      typedef std::shared_ptr< tls::connection >       con_impl_ptr;

    public:
      typedef client_endpoint< tls::connection >       endpoint_type;
      typedef std::shared_ptr< endpoint_type >         endpoint_ptr;
      typedef http_connection_impl< con_impl_ptr >     con_type;
      typedef std::shared_ptr< con_type >              con_ptr;

      http_tls_client_impl( const std::string& ca_filename )
        : _client_thread( fc::thread::current() )
      {
        _client.set_message_handler( [&]( connection_hdl hdl, message_type msg ){
          _client_thread.async( [&](){
            wdump((msg));
            fc::async( [=](){ if( _connection ) _connection->on_message(msg); });
          }).wait();
        });
        _client.set_close_handler( [=]( connection_hdl hdl ){
          _client_thread.async( [&](){ if( _connection ) {_connection->closed(); _connection.reset();} } ).wait();
          if( _closed ) _closed->set_value();
        });
        _client.set_fail_handler( [=]( connection_hdl hdl ){
          auto con = _client.get_con_from_hdl(hdl);
          if( _connection )
            _client_thread.async( [&](){ if( _connection ) _connection->closed(); _connection.reset(); } ).wait();
          if( _closed )
              _closed->set_value();
        });

        _client.set_tls_init_handler( [=]( connection_hdl ) {
          ssl_context_ptr ctx = std::make_shared< boost::asio::ssl::context >( boost::asio::ssl::context::tlsv1 );
          try {
            ctx->set_options(boost::asio::ssl::context::default_workarounds |
            boost::asio::ssl::context::no_sslv2 |
            boost::asio::ssl::context::no_sslv3 |
            boost::asio::ssl::context::single_dh_use);

            setup_peer_verify( ctx, ca_filename );
          } FC_CAPTURE_AND_LOG(());
          return ctx;
        });

        _client.init_asio( &fc::asio::default_io_service() );
      }
      ~http_tls_client_impl()
      {
        if( _connection )
        {
          _connection->close( 0, "client closed" );
          _connection.reset();
          _closed->wait();
        }
      }

      void setup_peer_verify( ssl_context_ptr& ctx, const std::string& ca_filename )
      {
        if( ca_filename == "_none" )
          return;
        ctx->set_verify_mode( boost::asio::ssl::verify_peer );
        if( ca_filename == "_default" )
          ctx->set_default_verify_paths();
        else
          ctx->load_verify_file( ca_filename );
        ctx->set_verify_depth(10);
        FC_ASSERT( _url.host().valid(), "Host not in given url: ${url}", ("url",_url) );
        ctx->set_verify_callback( boost::asio::ssl::rfc2818_verification( *_url.host() ) );
      }

      fc::promise<void>::ptr  _connected;
      fc::promise<void>::ptr  _closed;
      endpoint_type           _client;
      con_ptr                 _connection;
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
    : server( server_pem, ssl_password ), my( new detail::http_tls_server_impl(server::server_pem, server::ssl_password) ) {}
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

    my->_client.set_open_handler( [=]( detail::connection_hdl hdl ){
      my->_connection = std::make_shared< detail::http_client_impl::con_type >( my->_client.get_con_from_hdl( hdl ) );
      my->_closed = fc::promise<void>::ptr( new fc::promise<void>("http::closed") );
      my->_connected->set_value();
    });

    my->_connected->wait();
    my->_client.connect();
    return my->_connection;
  } FC_CAPTURE_AND_RETHROW( (_url_str) )}


  http_tls_client::http_tls_client( const std::string& ca_filename )
    : client( ca_filename ), my( new detail::http_tls_client_impl( client::ca_filename ) ) {}
  http_tls_client::~http_tls_client() {}

  connection_ptr http_tls_client::connect( const std::string& _url_str )
  { try {
    fc::url _url{ _url_str };
    FC_ASSERT( _url.proto() == "https", "Invalid protocol: \"{proto}\". Expected: \"https\"", ("proto", _url.proto()) );
    my->_url = _url;

    my->_connected = fc::promise<void>::ptr( new fc::promise<void>("https::connect") );

    my->_client.set_open_handler( [=]( detail::connection_hdl hdl ){
      my->_connection = std::make_shared< detail::http_tls_client_impl::con_type >( my->_client.get_con_from_hdl( hdl ) );
      my->_closed = fc::promise<void>::ptr( new fc::promise<void>("https::closed") );
      my->_connected->set_value();
    });

    my->_connected->wait();
    return nullptr;
  } FC_CAPTURE_AND_RETHROW( (_url_str) ) }

} } // fc::http
