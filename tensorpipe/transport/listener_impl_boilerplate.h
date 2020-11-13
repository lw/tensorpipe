/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <string>
#include <utility>

#include <tensorpipe/common/callback.h>
#include <tensorpipe/common/defs.h>
#include <tensorpipe/common/error.h>
#include <tensorpipe/common/error_macros.h>
#include <tensorpipe/transport/error.h>
#include <tensorpipe/transport/listener.h>

namespace tensorpipe {
namespace transport {

template <typename TImpl, typename TContextImpl>
class ListenerImplBoilerplate : public std::enable_shared_from_this<TImpl> {
 public:
  ListenerImplBoilerplate(
      std::shared_ptr<TContextImpl> context,
      std::string id);

  // Initialize member fields that need `shared_from_this`.
  void init();

  // Queue a callback to be called when a connection comes in.
  using accept_callback_fn = Listener::accept_callback_fn;
  void accept(accept_callback_fn fn);

  // Obtain the listener's address.
  std::string addr() const;

  // Tell the listener what its identifier is.
  void setId(std::string id);

  // Shut down the listener and its resources.
  void close();

  virtual ~ListenerImplBoilerplate() = default;

 protected:
  virtual void initImplFromLoop() = 0;
  virtual void acceptImplFromLoop(accept_callback_fn fn) = 0;
  virtual std::string addrImplFromLoop() const = 0;
  virtual void handleErrorImpl() = 0;

  void setError(Error error);

  const std::shared_ptr<TContextImpl> context_;

  Error error_{Error::kSuccess};

  // An identifier for the listener, composed of the identifier for the context,
  // combined with an increasing sequence number. It will be used as a prefix
  // for the identifiers of connections. All of them will only be used for
  // logging and debugging purposes.
  std::string id_;

 private:
  // Initialize member fields that need `shared_from_this`.
  void initFromLoop();

  // Queue a callback to be called when a connection comes in.
  void acceptFromLoop(accept_callback_fn fn);

  // Obtain the listener's address.
  std::string addrFromLoop() const;

  void setIdFromLoop(std::string id);

  // Shut down the connection and its resources.
  void closeFromLoop();

  // Deal with an error.
  void handleError();

  ClosingReceiver closingReceiver_;

  // A sequence number for the calls to accept.
  uint64_t nextConnectionBeingAccepted_{0};

  // A sequence number for the invocations of the callbacks of accept.
  uint64_t nextAcceptCallbackToCall_{0};
};

template <typename TImpl, typename TContextImpl>
ListenerImplBoilerplate<TImpl, TContextImpl>::ListenerImplBoilerplate(
    std::shared_ptr<TContextImpl> context,
    std::string id)
    : context_(std::move(context)),
      id_(std::move(id)),
      closingReceiver_(context_, context_->getClosingEmitter()) {}

template <typename TImpl, typename TContextImpl>
void ListenerImplBoilerplate<TImpl, TContextImpl>::init() {
  context_->deferToLoop(
      [impl{this->shared_from_this()}]() { impl->initFromLoop(); });
}

template <typename TImpl, typename TContextImpl>
void ListenerImplBoilerplate<TImpl, TContextImpl>::initFromLoop() {
  closingReceiver_.activate(*this);

  initImplFromLoop();
}

template <typename TImpl, typename TContextImpl>
void ListenerImplBoilerplate<TImpl, TContextImpl>::accept(
    accept_callback_fn fn) {
  context_->deferToLoop(
      [impl{this->shared_from_this()}, fn{std::move(fn)}]() mutable {
        impl->acceptFromLoop(std::move(fn));
      });
}

template <typename TImpl, typename TContextImpl>
void ListenerImplBoilerplate<TImpl, TContextImpl>::acceptFromLoop(
    accept_callback_fn fn) {
  TP_DCHECK(context_->inLoop());

  uint64_t sequenceNumber = nextConnectionBeingAccepted_++;
  TP_VLOG(7) << "Listener " << id_ << " received an accept request (#"
             << sequenceNumber << ")";

  fn = [this, sequenceNumber, fn{std::move(fn)}](
           const Error& error, std::shared_ptr<Connection> connection) {
    TP_DCHECK_EQ(sequenceNumber, nextAcceptCallbackToCall_++);
    TP_VLOG(7) << "Listener " << id_ << " is calling an accept callback (#"
               << sequenceNumber << ")";
    fn(error, std::move(connection));
    TP_VLOG(7) << "Listener " << id_ << " done calling an accept callback (#"
               << sequenceNumber << ")";
  };

  if (error_) {
    fn(error_, std::shared_ptr<Connection>());
    return;
  }

  acceptImplFromLoop(std::move(fn));
}

template <typename TImpl, typename TContextImpl>
std::string ListenerImplBoilerplate<TImpl, TContextImpl>::addr() const {
  std::string addr;
  context_->runInLoop([this, &addr]() { addr = addrFromLoop(); });
  return addr;
}

template <typename TImpl, typename TContextImpl>
std::string ListenerImplBoilerplate<TImpl, TContextImpl>::addrFromLoop() const {
  TP_DCHECK(context_->inLoop());

  return addrImplFromLoop();
}

template <typename TImpl, typename TContextImpl>
void ListenerImplBoilerplate<TImpl, TContextImpl>::setId(std::string id) {
  context_->deferToLoop(
      [impl{this->shared_from_this()}, id{std::move(id)}]() mutable {
        impl->setIdFromLoop(std::move(id));
      });
}

template <typename TImpl, typename TContextImpl>
void ListenerImplBoilerplate<TImpl, TContextImpl>::setIdFromLoop(
    std::string id) {
  TP_DCHECK(context_->inLoop());
  TP_VLOG(7) << "Listener " << id_ << " was renamed to " << id;
  id_ = std::move(id);
}

template <typename TImpl, typename TContextImpl>
void ListenerImplBoilerplate<TImpl, TContextImpl>::close() {
  context_->deferToLoop(
      [impl{this->shared_from_this()}]() { impl->closeFromLoop(); });
}

template <typename TImpl, typename TContextImpl>
void ListenerImplBoilerplate<TImpl, TContextImpl>::closeFromLoop() {
  TP_DCHECK(context_->inLoop());
  TP_VLOG(7) << "Listener " << id_ << " is closing";
  setError(TP_CREATE_ERROR(ListenerClosedError));
}

template <typename TImpl, typename TContextImpl>
void ListenerImplBoilerplate<TImpl, TContextImpl>::setError(Error error) {
  // Don't overwrite an error that's already set.
  if (error_ || !error) {
    return;
  }

  error_ = std::move(error);

  handleError();
}

template <typename TImpl, typename TContextImpl>
void ListenerImplBoilerplate<TImpl, TContextImpl>::handleError() {
  TP_DCHECK(context_->inLoop());
  TP_VLOG(8) << "Listener " << id_ << " is handling error " << error_.what();

  handleErrorImpl();
}

} // namespace transport
} // namespace tensorpipe
