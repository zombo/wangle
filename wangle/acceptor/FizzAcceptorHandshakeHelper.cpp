/*
 * Copyright 2016-present Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <wangle/acceptor/FizzAcceptorHandshakeHelper.h>
#include <wangle/acceptor/SSLAcceptorHandshakeHelper.h>
#include <wangle/ssl/SSLContextManager.h>

using namespace fizz::extensions;
using namespace fizz::server;

namespace wangle {

void FizzAcceptorHandshakeHelper::start(
    folly::AsyncSSLSocket::UniquePtr sock,
    AcceptorHandshakeHelper::Callback* callback) noexcept {
  callback_ = callback;
  sslContext_ = sock->getSSLContext();

  if (tokenBindingContext_) {
    extension_ =
        std::make_shared<TokenBindingServerExtension>(tokenBindingContext_);
  }

  transport_ = createFizzServer(std::move(sock), context_, extension_);
  transport_->accept(this);
}

AsyncFizzServer::UniquePtr FizzAcceptorHandshakeHelper::createFizzServer(
    folly::AsyncSSLSocket::UniquePtr sslSock,
    const std::shared_ptr<const FizzServerContext>& fizzContext,
    const std::shared_ptr<fizz::ServerExtensions>& extensions) {
  folly::AsyncSocket::UniquePtr asyncSock(
      new folly::AsyncSocket(std::move(sslSock)));
  asyncSock->cacheAddresses();
  return AsyncFizzServer::UniquePtr(
      new AsyncFizzServer(std::move(asyncSock), fizzContext, extensions));
}

void FizzAcceptorHandshakeHelper::fizzHandshakeSuccess(
    AsyncFizzServer* transport) noexcept {
  if (loggingCallback_) {
    loggingCallback_->logFizzHandshakeSuccess(*transport);
  }

  VLOG(3) << "Fizz handshake success";

  tinfo_.acceptTime = acceptTime_;
  tinfo_.secure = true;
  tinfo_.securityType = transport->getSecurityProtocol();
  tinfo_.sslSetupTime = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - acceptTime_);
  if (extension_ && extension_->getNegotiatedKeyParam().hasValue()) {
    tinfo_.negotiatedTokenBindingKeyParameters =
        static_cast<uint8_t>(*extension_->getNegotiatedKeyParam());
  }

  auto* handshakeLogging = transport->getState().handshakeLogging();
  if (handshakeLogging && handshakeLogging->clientSni) {
    tinfo_.sslServerName =
        std::make_shared<std::string>(*handshakeLogging->clientSni);
  }

  auto appProto = transport->getApplicationProtocol();
  callback_->connectionReady(std::move(transport_),
                             std::move(appProto),
                             SecureTransportType::TLS,
                             SSLErrorEnum::NO_ERROR);
}

void FizzAcceptorHandshakeHelper::fizzHandshakeError(
    AsyncFizzServer* transport, folly::exception_wrapper ex) noexcept {
  if (loggingCallback_) {
    loggingCallback_->logFizzHandshakeError(*transport, ex);
  }

  auto elapsedTime = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - acceptTime_);
  VLOG(3) << "Fizz handshake error after " << elapsedTime.count() << " ms; "
          << transport->getRawBytesReceived() << " bytes received & "
          << transport->getRawBytesWritten() << " bytes sent: " << ex.what();

  auto handshakeException =
      folly::make_exception_wrapper<FizzHandshakeException>(
          sslError_, elapsedTime, transport->getRawBytesReceived());

  callback_->connectionError(
      transport_.get(), std::move(handshakeException), sslError_);
}

folly::AsyncSSLSocket::UniquePtr FizzAcceptorHandshakeHelper::createSSLSocket(
    const std::shared_ptr<folly::SSLContext>& context,
    folly::EventBase* evb,
    int fd) {
  return folly::AsyncSSLSocket::UniquePtr(new folly::AsyncSSLSocket(
      context, evb, folly::NetworkSocket::fromFd(fd)));
}

void FizzAcceptorHandshakeHelper::fizzHandshakeAttemptFallback(
    std::unique_ptr<folly::IOBuf> clientHello) {
  VLOG(3) << "Fallback to OpenSSL";

  auto evb = transport_->getEventBase();
  auto fd = transport_->getUnderlyingTransport<folly::AsyncSocket>()
                ->detachNetworkSocket()
                .toFd();
  transport_.reset();

  sslSocket_ = createSSLSocket(sslContext_, evb, fd);
  sslSocket_->setPreReceivedData(std::move(clientHello));
  sslSocket_->enableClientHelloParsing();
  sslSocket_->forceCacheAddrOnFailure(true);
  sslSocket_->sslAccept(this);
}

void FizzAcceptorHandshakeHelper::handshakeSuc(
    folly::AsyncSSLSocket* sock) noexcept {
  auto appProto = sock->getApplicationProtocol();
  if (!appProto.empty()) {
    VLOG(3) << "Client selected next protocol " << appProto;
  } else {
    VLOG(3) << "Client did not select a next protocol";
  }

  // fill in SSL-related fields from TransportInfo
  // the other fields like RTT are filled in the Acceptor
  tinfo_.acceptTime = acceptTime_;
  tinfo_.sslSetupTime = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - acceptTime_);
  wangle::SSLAcceptorHandshakeHelper::fillSSLTransportInfoFields(sock, tinfo_);

  // The callback will delete this.
  callback_->connectionReady(std::move(sslSocket_),
                             std::move(appProto),
                             SecureTransportType::TLS,
                             SSLErrorEnum::NO_ERROR);
}

void FizzAcceptorHandshakeHelper::handshakeErr(
    folly::AsyncSSLSocket* sock,
    const folly::AsyncSocketException& ex) noexcept {
  auto elapsedTime = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - acceptTime_);
  VLOG(3) << "SSL handshake error after " << elapsedTime.count() << " ms; "
          << sock->getRawBytesReceived() << " bytes received & "
          << sock->getRawBytesWritten() << " bytes sent: " << ex.what();

  auto sslEx = folly::make_exception_wrapper<SSLException>(
      sslError_, elapsedTime, sock->getRawBytesReceived());

  // The callback will delete this.
  callback_->connectionError(sslSocket_.get(), sslEx, sslError_);
}
}
