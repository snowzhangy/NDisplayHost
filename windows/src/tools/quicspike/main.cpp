// quic spike: prove msquic v2.6 + Schannel on this machine before the real
// session is built. Loopback server+client: ALPN wvd/1, self-signed identity,
// TLS exporter equality (WIRE.md §7 — the make-or-break), stream echo and
// datagram echo.
#define QUIC_API_ENABLE_PREVIEW_FEATURES 1
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <msquic.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "identity.h"
#include "wdauth.h"

// Small auto-reset event for the spike's waits.
struct event {
  HANDLE h = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  ~event() { if (h) CloseHandle(h); }
  void set() { SetEvent(h); }
  bool wait(DWORD ms) { return WaitForSingleObject(h, ms) == WAIT_OBJECT_0; }
};

#pragma comment(lib, "msquic.lib")

static const QUIC_API_TABLE* MsQuic = nullptr;

struct Exporters {
  std::vector<uint8_t> sas, auth, pair;
};

static bool ExportAll(HQUIC conn, Exporters& out) {
  struct { const char* label; std::vector<uint8_t>* dst; } items[] = {
      {"EXPORTER-wvd-sas-v1", &out.sas},
      {"EXPORTER-wvd-auth-v1", &out.auth},
      {"EXPORTER-wvd-pair-v1", &out.pair},
  };
  for (auto& it : items) {
    QUIC_KEYING_MATERIAL_CONFIG cfg{};
    cfg.Label = it.label;
    cfg.ContextLength = 0;
    cfg.Context = nullptr;
    cfg.OutputLength = 32;
    uint8_t buf[32];
    QUIC_STATUS st = MsQuic->ConnectionExportKeyingMaterial(conn, &cfg, buf);
    if (QUIC_FAILED(st)) {
      printf("export %s failed 0x%x\n", it.label, st);
      return false;
    }
    it.dst->assign(buf, buf + 32);
  }
  return true;
}

// ---- Server ---------------------------------------------------------------

struct ServerCtx {
  HQUIC config = nullptr;
  wvd::HostIdentity identity;
  Exporters exporters;
  event evReady;
  std::vector<uint8_t> streamGot;
  std::vector<uint8_t> dgramGot;
  std::vector<uint8_t> dgramEcho;  // msquic owns the received buffer
  bool echoed = false;
  event evStream, evDgram;
};

static QUIC_STATUS _stdcall StreamCallback(HQUIC stream, void* ctx, QUIC_STREAM_EVENT* ev) {
  auto* s = static_cast<ServerCtx*>(ctx);
  printf("[srv] stream event %d\n", int(ev->Type));
  switch (ev->Type) {
    case QUIC_STREAM_EVENT_RECEIVE: {
      for (uint32_t i = 0; i < ev->RECEIVE.BufferCount; i++) {
        const uint8_t* p = ev->RECEIVE.Buffers[i].Buffer;
        s->streamGot.insert(s->streamGot.end(), p, p + ev->RECEIVE.Buffers[i].Length);
      }
      MsQuic->StreamReceiveComplete(stream, ev->RECEIVE.TotalBufferLength);
      // Echo as soon as the request is complete: msquic only delivers
      // PEER_SEND_SHUTDOWN after the FIN flush, and the spike should not
      // depend on that ordering.
      if (s->streamGot.size() >= 5 && !s->echoed) {
        s->echoed = true;
        QUIC_BUFFER buf{static_cast<UINT16>(s->streamGot.size()), s->streamGot.data()};
        QUIC_STATUS st = MsQuic->StreamSend(stream, &buf, 1, QUIC_SEND_FLAG_NONE, nullptr);
        printf("[srv] echo send status 0x%x\n", st);
      }
      return QUIC_STATUS_SUCCESS;  // consume
    }
    case QUIC_STREAM_EVENT_SEND_COMPLETE:
      return QUIC_STATUS_SUCCESS;
    default:
      return QUIC_STATUS_SUCCESS;
  }
}

static QUIC_STATUS _stdcall ConnCallback(HQUIC conn, void* ctx, QUIC_CONNECTION_EVENT* ev) {
  auto* s = static_cast<ServerCtx*>(ctx);
  switch (ev->Type) {
    case QUIC_CONNECTION_EVENT_CONNECTED:
      printf("[srv] connected\n");
      printf("[srv] exporting\n");
      ExportAll(conn, s->exporters);
      printf("[srv] exported\n");
      s->evReady.set();
      return QUIC_STATUS_SUCCESS;
    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
      MsQuic->SetCallbackHandler(ev->PEER_STREAM_STARTED.Stream, (void*)StreamCallback, s);
      return QUIC_STATUS_SUCCESS;
    case QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED:
      if (ev->DATAGRAM_RECEIVED.Buffer) {
        const uint8_t* p = ev->DATAGRAM_RECEIVED.Buffer->Buffer;
        s->dgramGot.assign(p, p + ev->DATAGRAM_RECEIVED.Buffer->Length);
        s->evDgram.set();
        // Echo it back from our own copy: msquic frees the received buffer
        // when this callback returns, and DatagramSend is asynchronous.
        s->dgramEcho.assign(p, p + ev->DATAGRAM_RECEIVED.Buffer->Length);
        QUIC_BUFFER out{static_cast<UINT16>(s->dgramEcho.size()), s->dgramEcho.data()};
        QUIC_STATUS st = MsQuic->DatagramSend(conn, &out, 1, QUIC_SEND_FLAG_NONE, nullptr);
        printf("[srv] datagram echo status 0x%x\n", st);
      }
      return QUIC_STATUS_SUCCESS;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
      return QUIC_STATUS_SUCCESS;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
      printf("[srv] transport shutdown 0x%x%s", ev->SHUTDOWN_INITIATED_BY_TRANSPORT.Status,
             "\n");
      return QUIC_STATUS_SUCCESS;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
      MsQuic->ConnectionClose(conn);
      return QUIC_STATUS_SUCCESS;
    default:
      return QUIC_STATUS_SUCCESS;
  }
}

// ---- Client ---------------------------------------------------------------

struct ClientCtx {
  Exporters exporters;
  std::vector<uint8_t> echo;
  std::vector<uint8_t> dgramEcho;
  event evStream, evDgram;
  bool acceptCert = true;
};

static QUIC_STATUS _stdcall ClientStreamCb(HQUIC stream, void* ctx, QUIC_STREAM_EVENT* ev) {
  auto* c = static_cast<ClientCtx*>(ctx);
  printf("[cli] stream event %d\n", int(ev->Type));
  switch (ev->Type) {
    case QUIC_STREAM_EVENT_RECEIVE:
      for (uint32_t i = 0; i < ev->RECEIVE.BufferCount; i++) {
        const uint8_t* p = ev->RECEIVE.Buffers[i].Buffer;
        c->echo.insert(c->echo.end(), p, p + ev->RECEIVE.Buffers[i].Length);
      }
      MsQuic->StreamReceiveComplete(stream, ev->RECEIVE.TotalBufferLength);
      c->evStream.set();
      return QUIC_STATUS_SUCCESS;
    case QUIC_STREAM_EVENT_SEND_COMPLETE:
      return QUIC_STATUS_SUCCESS;
    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
      return QUIC_STATUS_SUCCESS;
    default:
      return QUIC_STATUS_SUCCESS;
  }
}

static QUIC_STATUS _stdcall ClientConnCb(HQUIC conn, void* ctx, QUIC_CONNECTION_EVENT* ev) {
  auto* c = static_cast<ClientCtx*>(ctx);
  switch (ev->Type) {
    case QUIC_CONNECTION_EVENT_PEER_CERTIFICATE_RECEIVED:
      printf("[cli] cert received%s", "\n");
      return c->acceptCert ? QUIC_STATUS_SUCCESS : QUIC_STATUS_INTERNAL_ERROR;
    case QUIC_CONNECTION_EVENT_CONNECTED:
      printf("[cli] exporting...\n");
      ExportAll(conn, c->exporters);
      printf("[cli] exported\n");
      return QUIC_STATUS_SUCCESS;
    case QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED:
      if (ev->DATAGRAM_RECEIVED.Buffer) {
        const uint8_t* p = ev->DATAGRAM_RECEIVED.Buffer->Buffer;
        c->dgramEcho.assign(p, p + ev->DATAGRAM_RECEIVED.Buffer->Length);
        c->evDgram.set();
      }
      return QUIC_STATUS_SUCCESS;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
      printf("[cli] transport shutdown 0x%x%s", ev->SHUTDOWN_INITIATED_BY_TRANSPORT.Status,
             "\n");
      return QUIC_STATUS_SUCCESS;
    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
      MsQuic->ConnectionClose(conn);
      return QUIC_STATUS_SUCCESS;
    default:
      return QUIC_STATUS_SUCCESS;
  }
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  printf("== NDisplay QUIC spike ==\n");
  if (QUIC_FAILED(MsQuicOpenVersion(QUIC_API_VERSION_2, (const void**)&MsQuic))) {
    printf("MsQuicOpen failed\n");
    return 1;
  }

  wvd::HostIdentity id;
  std::string err;
  if (!id.Load("NDisplayHostWin", &err)) {
    printf("identity load failed: %s\n", err.c_str());
    return 1;
  }
  printf("identity ok, cert %zu bytes, sha256 ", id.certDer().size());
  for (int i = 0; i < 8; i++) printf("%02x", id.certSha256()[i]);
  printf("\n");

  QUIC_REGISTRATION_CONFIG reg{"wvdspike", QUIC_EXECUTION_PROFILE_LOW_LATENCY};
  HQUIC registration = nullptr;
  if (QUIC_FAILED(MsQuic->RegistrationOpen(&reg, &registration))) {
    printf("RegistrationOpen failed\n");
    return 1;
  }

  const char* alpn = "wvd/1";
  QUIC_BUFFER alpnBuf{5, (uint8_t*)alpn};

  QUIC_SETTINGS settings{};
  settings.IsSetFlags = 0;
  settings.IdleTimeoutMs = 15000;
  settings.IsSet.IdleTimeoutMs = TRUE;
  settings.DatagramReceiveEnabled = TRUE;
  settings.IsSet.DatagramReceiveEnabled = TRUE;
  settings.PeerBidiStreamCount = 4;
  settings.IsSet.PeerBidiStreamCount = TRUE;

  HQUIC serverConfig = nullptr;
  if (QUIC_FAILED(MsQuic->ConfigurationOpen(registration, &alpnBuf, 1, &settings,
                                            sizeof(settings), nullptr, &serverConfig))) {
    printf("server ConfigurationOpen failed\n");
    return 1;
  }
  QUIC_CREDENTIAL_CONFIG serverCred{};
  serverCred.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_CONTEXT;
  serverCred.CertificateContext = (QUIC_CERTIFICATE*)id.certContext();
  serverCred.Flags = QUIC_CREDENTIAL_FLAG_NONE;
  if (QUIC_FAILED(MsQuic->ConfigurationLoadCredential(serverConfig, &serverCred))) {
    printf("server ConfigurationLoadCredential failed\n");
    return 1;
  }

  // Static: msquic can still deliver SHUTDOWN_COMPLETE on a worker thread
  // while RegistrationClose() runs, and the contexts are its callbacks' state.
  static ServerCtx server;
  server.config = serverConfig;  // handed to connections in NEW_CONNECTION
  HQUIC listener = nullptr;
  if (QUIC_FAILED(MsQuic->ListenerOpen(registration,
                                       [](HQUIC l, void* c, QUIC_LISTENER_EVENT* ev) -> QUIC_STATUS {
                                         auto* s = static_cast<ServerCtx*>(c);
                                         if (ev->Type == QUIC_LISTENER_EVENT_NEW_CONNECTION) {
                                           MsQuic->SetCallbackHandler(
                                               ev->NEW_CONNECTION.Connection, (void*)ConnCallback, s);
                                           return MsQuic->ConnectionSetConfiguration(
                   ev->NEW_CONNECTION.Connection,
                   static_cast<HQUIC>(s->config));
                                         }
                                         return QUIC_STATUS_SUCCESS;
                                       },
                                       &server, &listener))) {
    printf("ListenerOpen failed\n");
    return 1;
  }
  SOCKADDR_IN addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(47800);
  QUIC_ADDR qa;
  memcpy(&qa, &addr, sizeof(addr));
  if (QUIC_FAILED(MsQuic->ListenerStart(listener, &alpnBuf, 1, &qa))) {
    printf("ListenerStart failed\n");
    return 1;
  }
  printf("listener on 127.0.0.1:47800\n");

  // Client.
  HQUIC clientConfig = nullptr;
  if (QUIC_FAILED(MsQuic->ConfigurationOpen(registration, &alpnBuf, 1, &settings,
                                            sizeof(settings), nullptr, &clientConfig))) {
    printf("client ConfigurationOpen failed\n");
    return 1;
  }
  QUIC_CREDENTIAL_CONFIG clientCred{};
  clientCred.Type = QUIC_CREDENTIAL_TYPE_NONE;
  clientCred.Flags = QUIC_CREDENTIAL_FLAG_CLIENT |
                     QUIC_CREDENTIAL_FLAG_INDICATE_CERTIFICATE_RECEIVED |
                     QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
  if (QUIC_FAILED(MsQuic->ConfigurationLoadCredential(clientConfig, &clientCred))) {
    printf("client ConfigurationLoadCredential failed\n");
    return 1;
  }

  static ClientCtx client;  // same lifetime rule as ServerCtx
  HQUIC conn = nullptr;
  if (QUIC_FAILED(MsQuic->ConnectionOpen(registration, ClientConnCb, &client, &conn))) {
    printf("ConnectionOpen failed\n");
    return 1;
  }
  if (QUIC_FAILED(MsQuic->ConnectionStart(conn, clientConfig, QUIC_ADDRESS_FAMILY_INET,
                                             "127.0.0.1", 47800))) {
    printf("ConnectionStart failed\n");
    return 1;
  }
  if (!server.evReady.wait(5000)) {
    printf("server never saw CONNECTED\n");
    return 1;
  }
  printf("connected both sides\n");

  // Stream echo.
  printf("opening stream\n");
  HQUIC stream = nullptr;
  if (QUIC_FAILED(MsQuic->StreamOpen(conn, QUIC_STREAM_OPEN_FLAG_NONE, ClientStreamCb, &client,
                                     &stream))) {
    printf("StreamOpen failed\n");
    return 1;
  }
  MsQuic->StreamStart(stream, QUIC_STREAM_START_FLAG_NONE);
  printf("stream started, sending\n");
  const char* hello = "hello";
  QUIC_BUFFER hb{5, (uint8_t*)hello};
  MsQuic->StreamSend(stream, &hb, 1, QUIC_SEND_FLAG_FIN, nullptr);
  if (!client.evStream.wait(5000)) {
    printf("no stream echo\n");
    return 1;
  }
  bool streamOk = client.echo.size() == 5 && memcmp(client.echo.data(), "hello", 5) == 0;
  printf("stream echo: %s\n", streamOk ? "ok" : "FAIL");

  // Datagram echo.
  const char* dgmsg = "ping";
  QUIC_BUFFER db{4, (uint8_t*)dgmsg};
  MsQuic->DatagramSend(conn, &db, 1, QUIC_SEND_FLAG_NONE, nullptr);
  if (!client.evDgram.wait(5000)) {
    printf("no datagram echo\n");
    return 1;
  }
  bool dgramOk = client.dgramEcho.size() == 4 && memcmp(client.dgramEcho.data(), "ping", 4) == 0;
  printf("datagram echo: %s\n", dgramOk ? "ok" : "FAIL");

  // Exporters: the crux — both sides must derive the same material.
  bool exporterOk = server.exporters.sas == client.exporters.sas &&
                    server.exporters.auth == client.exporters.auth &&
                    server.exporters.pair == client.exporters.pair &&
                    server.exporters.sas.size() == 32 &&
                    server.exporters.sas != server.exporters.auth;
  printf("exporters %s\n", exporterOk ? "match" : "MISMATCH");
  if (exporterOk) {
    // Derive the SAS the pairing flow would show, to eyeball later.
    std::vector<uint8_t> fakePk(65, 0x04);
    printf("sample SAS for a 0x04-padded pk: %s\n",
           wvd::SasCode(server.exporters.sas, id.certSha256(), fakePk).c_str());
  }

  MsQuic->StreamClose(stream);
  MsQuic->ConnectionShutdown(conn, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
  MsQuic->ListenerClose(listener);
  MsQuic->ConfigurationClose(serverConfig);
  MsQuic->ConfigurationClose(clientConfig);
  MsQuic->RegistrationClose(registration);
  MsQuicClose(MsQuic);

  bool ok = streamOk && dgramOk && exporterOk;
  printf("== spike %s ==\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
