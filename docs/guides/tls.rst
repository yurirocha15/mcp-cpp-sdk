TLS and Proxies
===============

Clients reach ``https://`` servers over TLS, with the server's certificate
checked against the system's certificate authorities, and can send their
connections through an HTTP proxy. The HTTP and WebSocket servers end TLS
themselves when given a certificate. This guide covers the settings of the MCP
HTTP client, the OAuth client, the servers and WebSocket.

Connecting over TLS
-------------------

An ``https://`` URL is all an ``HttpClientTransport`` or an
``Endpoint::http()`` client needs for a server whose certificate a public
authority issued. Settings for anything else go in a
``mcp::TlsClientContext``, made once from ``mcp::TlsClientOptions`` and shared
by every transport that should trust the same servers. The example builds one
that trusts a private authority as well as the system's:

.. literalinclude:: ../../examples/features/tls_client.cpp
   :language: cpp
   :start-after: // ========== TLS SETTINGS ==========
   :end-before: // ========== END TLS SETTINGS ==========
   :dedent: 4

and hands it to the transport in ``HttpEndpointOptions::configure``:

.. literalinclude:: ../../examples/features/tls_client.cpp
   :language: cpp
   :start-after: // ========== CONNECT ==========
   :end-before: // ========== END CONNECT ==========
   :dedent: 8

``set_tls()`` also takes ``TlsClientOptions`` directly, which builds a context
for that transport alone. A context reads its files when it is made, and
``create()`` throws ``mcp::TlsConfigError`` for a file it cannot read, a client
key that does not match its certificate, or settings that trust no authority.

What is checked
---------------

Every connection checks the server's certificate and host name; there is no
setting that turns either off. A certificate is accepted when:

- it chains to a trusted authority. The system's authorities come from the CA
  bundle of the Linux distribution or BSD, the Keychain on macOS, or the
  ``ROOT`` store on Windows; ``SSL_CERT_FILE`` and ``SSL_CERT_DIR`` replace them
  when set. They are read once per process, so a running program does not see
  a change to them. ``ca_files``, ``ca_dirs`` and ``ca_pem`` add authorities of
  your own, and an intermediate authority named there is trusted as an anchor.
  ``use_system_roots = false`` trusts only those;
- it is valid now;
- a subject alternative name carries the host: its DNS name for a URL with a
  host name, its IP address for a URL with an address. A wildcard covers one
  whole label (``*.example.com`` covers ``api.example.com``, not
  ``example.com`` or ``a.api.example.com``), and a certificate that names its
  host only in the subject's common name is refused.

Revocation (CRL, OCSP) is not checked. The client sends the host name as the TLS
server name (SNI), except for an address. ``set_server_name()`` checks the
certificate against another name, and sends that name, for a server reached by an
address or an alias its certificate does not carry. ``min_version`` raises the
lowest version accepted from TLS 1.2 to TLS 1.3.

Client certificates
-------------------

For a server that requires a client certificate (mutual TLS), set
``client_certificate_file``, the certificate followed by its intermediates in
PEM, and ``client_key_file``. ``key_password`` returns the passphrase of an
encrypted key; it is called while the context is made.

Errors
------

A connection that fails its handshake throws ``mcp::TlsError``, whose ``kind()``
says why. A ``Client`` call nests it in the ``McpError`` it throws, with code
``mcp::constants::jsonrpc::g_CONNECTION_CLOSED``, as the example shows:

.. literalinclude:: ../../examples/features/tls_client.cpp
   :language: cpp
   :start-after: // ========== FAILURE ==========
   :end-before: // ========== END FAILURE ==========
   :dedent: 8

.. list-table::
   :header-rows: 1

   * - ``TlsError::Kind``
     - Meaning
   * - ``eUntrustedCertificate``
     - The certificate does not chain to a trusted authority.
   * - ``eExpiredCertificate``
     - The certificate has expired or is not valid yet.
   * - ``eHostnameMismatch``
     - The certificate does not name the host.
   * - ``eProtocolVersion``
     - The two sides have no TLS version in common.
   * - ``eNotTls``
     - The server does not speak TLS, such as a plain HTTP server on that port.
   * - ``eHandshakeFailed``
     - The handshake failed for another reason, such as a server that refused
       the client certificate.
   * - ``eHandshakeTimeout``
     - The handshake did not finish in time.
   * - ``eTruncated``
     - The connection ended in the middle of a message without the server's
       ``close_notify``.

Proxies
-------

A client connects directly unless it is given ``mcp::ProxyOptions``.
``ProxyOptions::from_environment()`` reads ``HTTPS_PROXY``, ``HTTP_PROXY`` and
``NO_PROXY`` (lower-case forms win), as the example does; the fields can also be
set directly:

.. code-block:: cpp

   mcp::ProxyOptions proxy;
   proxy.https_proxy = "http://user:secret@proxy.internal:3128";
   proxy.no_proxy = {"internal.example.com", "10.0.0.0/8"};
   transport.set_proxy(proxy);

A connection to an ``https://`` server goes through the proxy in a ``CONNECT``
tunnel, so TLS runs end to end and the certificate check is unchanged. A request
to an ``http://`` server goes to the proxy with the full URL as its target.
Credentials in the proxy URL are sent as Basic ``Proxy-Authorization``. A
``no_proxy`` entry is a host name, which covers its subdomains, an address, a
CIDR range, any of these with ``:port``, or ``*``. Connections to ``localhost``
and loopback addresses never use the proxy. Only ``http://`` proxies are
supported.

OAuth
-----

The OAuth client takes the same settings. Each authenticator configuration
(``OAuthAuthorizationConfig``, ``ClientCredentialsConfig``,
``EnterpriseAuthorizationConfig``) has ``tls`` and ``proxy`` fields, and
``OAuthHttpClient::configure()`` takes them with the fetch policy and resolver,
as one change:

.. code-block:: cpp

   config.tls = tls;  // the same context the MCP transport uses
   config.proxy = mcp::ProxyOptions::from_environment();

Without ``tls`` the system's authorities are trusted. The certificate is checked
against the host in the URL, even though the connection goes to the address the
fetch policy checked. With a proxy, the proxy resolves the server's name, so the
policy's address checks apply only to an address written in a URL.

Servers
-------

``HttpServerTransport`` and ``StreamableHttpSessionManager`` serve TLS once
``set_tls()`` gives them ``mcp::TlsServerOptions``, before they listen.
``run_http()`` hands its manager to the configure callback, where the example
sets them:

.. literalinclude:: ../../examples/features/tls_server.cpp
   :language: cpp
   :start-after: // ========== TLS SETTINGS ==========
   :end-before: // ========== END TLS SETTINGS ==========
   :dedent: 4

.. literalinclude:: ../../examples/features/tls_server.cpp
   :language: cpp
   :start-after: // ========== SERVE ==========
   :end-before: // ========== END SERVE ==========
   :dedent: 8

The certificate chain and the key come from PEM files or from PEM text
(``certificate_chain_pem``, ``private_key_pem``), and ``key_password`` returns
the passphrase of an encrypted key. ``mcp::TlsServerContext::create()`` builds
the settings once for servers that share them; it, and ``set_tls()`` with
options, throw ``TlsConfigError`` for a certificate or key that cannot be
loaded or a key that does not match the certificate.

A client must finish its handshake within ``handshake_timeout``, five seconds
by default, before a request is read. ``max_pending_handshakes`` handshakes run
at once, a quarter of the connection limit by default, and a connection past
that is closed as soon as it is accepted, so clients that connect and say
nothing cannot hold every connection. A connection the server ends gets its
``close_notify``.

With ``client_auth = TlsServerOptions::ClientAuth::eRequired``, every client
must show a certificate that chains to one of ``client_ca_files`` or
``client_ca_pem`` (mutual TLS), or its handshake fails. Naming client
authorities without requiring a certificate is refused, so that no server
believes it checks clients when it does not.

A server can also stay behind a reverse proxy that ends TLS for it, as
:doc:`deployment` describes.

WebSocket
---------

``WebSocketClientTransport::set_tls()`` connects over TLS (wss), with the
options and checks of the HTTP client; ``set_server_name()`` works as it does
there. A ``WebSocketServerTransport`` made with a ``TlsServerContext`` runs the
TLS handshake on its accepted socket together with the WebSocket upgrade,
within its handshake timeout:

.. code-block:: cpp

   const auto tls = mcp::TlsServerContext::create(options);  // once
   mcp::WebSocketServerTransport transport(std::move(socket), tls);  // per connection
