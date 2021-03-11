/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <tensorpipe/channel/cuda_ipc/channel_impl.h>

#include <memory>
#include <string>
#include <utility>

#include <cuda.h>
#include <cuda_runtime.h>
#include <nop/serializer.h>
#include <nop/structure.h>
#include <nop/types/variant.h>

#include <tensorpipe/channel/cuda_ipc/context_impl.h>
#include <tensorpipe/channel/helpers.h>
#include <tensorpipe/common/cuda.h>
#include <tensorpipe/common/defs.h>
#include <tensorpipe/common/error.h>
#include <tensorpipe/transport/connection.h>

namespace tensorpipe {
namespace channel {
namespace cuda_ipc {

namespace {

struct Descriptor {
  std::string allocationId;
  std::string handle;
  size_t offset;
  std::string startEvHandle;
  NOP_STRUCTURE(Descriptor, allocationId, handle, offset, startEvHandle);
};

struct Reply {
  std::string stopEvHandle;
  NOP_STRUCTURE(Reply, stopEvHandle);
};

struct Ack {
  NOP_STRUCTURE(Ack);
};

Descriptor makeDescriptor(
    SendOperation& op,
    const CudaLib& cudaLib,
    const std::string& processIdentifier) {
  CudaDeviceGuard guard(op.deviceIdx);
  cudaIpcMemHandle_t handle;
  TP_CUDA_CHECK(cudaIpcGetMemHandle(&handle, const_cast<void*>(op.ptr)));
  CUdeviceptr basePtr;
  TP_CUDA_DRIVER_CHECK(
      cudaLib,
      cudaLib.memGetAddressRange(
          &basePtr, nullptr, reinterpret_cast<CUdeviceptr>(op.ptr)));
  size_t offset = reinterpret_cast<const uint8_t*>(op.ptr) -
      reinterpret_cast<uint8_t*>(basePtr);

  unsigned long long bufferId;
  TP_CUDA_DRIVER_CHECK(
      cudaLib,
      cudaLib.pointerGetAttribute(
          &bufferId, CU_POINTER_ATTRIBUTE_BUFFER_ID, basePtr));

  return Descriptor{
      // FIXME The process identifier will be the same each time, hence we could
      // just send it once during the setup of the channel and omit it later.
      processIdentifier + "_" + std::to_string(bufferId),
      std::string(reinterpret_cast<const char*>(&handle), sizeof(handle)),
      offset,
      op.startEv.serializedHandle()};
}

} // namespace

SendOperation::SendOperation(
    TSendCallback callback,
    int deviceIdx,
    const void* ptr,
    cudaStream_t stream)
    : ptr(ptr),
      deviceIdx(deviceIdx),
      stream(stream),
      callback(std::move(callback)),
      startEv(deviceIdx, /*interprocess=*/true) {
  startEv.record(stream);
}

RecvOperation::RecvOperation(
    int deviceIdx,
    void* ptr,
    cudaStream_t stream,
    size_t length)
    : ptr(ptr),
      length(length),
      deviceIdx(deviceIdx),
      stream(stream),
      stopEv(deviceIdx, /*interprocess=*/true) {}

ChannelImpl::ChannelImpl(
    ConstructorToken token,
    std::shared_ptr<ContextImpl> context,
    std::string id,
    std::shared_ptr<transport::Connection> replyConnection,
    std::shared_ptr<transport::Connection> ackConnection)
    : ChannelImplBoilerplate<CudaBuffer, ContextImpl, ChannelImpl>(
          token,
          std::move(context),
          std::move(id)),
      replyConnection_(std::move(replyConnection)),
      ackConnection_(std::move(ackConnection)) {}

void ChannelImpl::initImplFromLoop() {
  context_->enroll(*this);
}

void ChannelImpl::sendImplFromLoop(
    uint64_t sequenceNumber,
    CudaBuffer buffer,
    TDescriptorCallback descriptorCallback,
    TSendCallback callback) {
  int deviceIdx = cudaDeviceForPointer(context_->getCudaLib(), buffer.ptr);

  SendOpIter opIter = sendOps_.emplaceBack(
      sequenceNumber,
      std::move(callback),
      deviceIdx,
      buffer.ptr,
      buffer.stream);

  sendOps_.advanceOperation(opIter);

  NopHolder<Descriptor> nopHolder;
  nopHolder.getObject() = makeDescriptor(
      *opIter, context_->getCudaLib(), context_->getProcessIdentifier());
  descriptorCallback(Error::kSuccess, saveDescriptor(nopHolder));
}

void ChannelImpl::advanceSendOperation(
    SendOpIter opIter,
    SendOperation::State prevOpState) {
  TP_DCHECK(context_->inLoop());

  SendOperation& op = *opIter;

  sendOps_.attemptTransition(
      opIter,
      /*from=*/SendOperation::UNINITIALIZED,
      /*to=*/SendOperation::FINISHED,
      /*cond=*/error_,
      /*actions=*/{&ChannelImpl::callSendCallback});

  // Needs to go after previous op to ensure predictable and consistent ordering
  // of read calls on reply control connection.
  sendOps_.attemptTransition(
      opIter,
      /*from=*/SendOperation::UNINITIALIZED,
      /*to=*/SendOperation::READING_REPLY,
      /*cond=*/!error_ && prevOpState >= SendOperation::READING_REPLY,
      /*actions=*/{&ChannelImpl::readReply});

  sendOps_.attemptTransition(
      opIter,
      /*from=*/SendOperation::READING_REPLY,
      /*to=*/SendOperation::FINISHED,
      /*cond=*/error_ && op.doneReadingReply,
      /*actions=*/{&ChannelImpl::callSendCallback});

  // Needs to go after previous op to ensure predictable and consistent ordering
  // of write calls on ack control connection.
  sendOps_.attemptTransition(
      opIter,
      /*from=*/SendOperation::READING_REPLY,
      /*to=*/SendOperation::FINISHED,
      /*cond=*/!error_ && op.doneReadingReply &&
          prevOpState >= SendOperation::FINISHED,
      /*actions=*/
      {&ChannelImpl::waitOnStopEvent,
       &ChannelImpl::callSendCallback,
       &ChannelImpl::writeAck});
}

void ChannelImpl::readReply(SendOpIter opIter) {
  SendOperation& op = *opIter;

  auto nopReplyHolder = std::make_shared<NopHolder<Reply>>();
  TP_VLOG(6) << "Channel " << id_ << " is reading nop object (reply #"
             << op.sequenceNumber << ")";
  replyConnection_->read(
      *nopReplyHolder,
      callbackWrapper_([opIter, nopReplyHolder](ChannelImpl& impl) {
        TP_VLOG(6) << "Channel " << impl.id_
                   << " done reading nop object (reply #"
                   << opIter->sequenceNumber << ")";
        opIter->doneReadingReply = true;
        if (!impl.error_) {
          opIter->stopEvHandle =
              std::move(nopReplyHolder->getObject().stopEvHandle);
        }
        impl.sendOps_.advanceOperation(opIter);
      }));
}

void ChannelImpl::waitOnStopEvent(SendOpIter opIter) {
  SendOperation& op = *opIter;

  const cudaIpcEventHandle_t* stopEvHandle =
      reinterpret_cast<const cudaIpcEventHandle_t*>(op.stopEvHandle.c_str());
  CudaEvent stopEv(op.deviceIdx, *stopEvHandle);
  stopEv.wait(op.stream, op.deviceIdx);
}

void ChannelImpl::callSendCallback(SendOpIter opIter) {
  SendOperation& op = *opIter;

  op.callback(error_);
  // Reset callback to release the resources it was holding.
  op.callback = nullptr;
}

void ChannelImpl::writeAck(SendOpIter opIter) {
  SendOperation& op = *opIter;

  TP_VLOG(6) << "Channel " << id_ << " is writing ACK notification (#"
             << op.sequenceNumber << ")";
  auto nopAckHolder = std::make_shared<NopHolder<Ack>>();
  ackConnection_->write(
      *nopAckHolder,
      callbackWrapper_(
          [nopAckHolder, sequenceNumber{op.sequenceNumber}](ChannelImpl& impl) {
            TP_VLOG(6) << "Channel " << impl.id_
                       << " done writing ACK notification (#" << sequenceNumber
                       << ")";
          }));
}

void ChannelImpl::recvImplFromLoop(
    uint64_t sequenceNumber,
    TDescriptor descriptor,
    CudaBuffer buffer,
    TRecvCallback callback) {
  int deviceIdx = cudaDeviceForPointer(context_->getCudaLib(), buffer.ptr);
  RecvOpIter opIter = recvOps_.emplaceBack(
      sequenceNumber, deviceIdx, buffer.ptr, buffer.stream, buffer.length);

  opIter->callback = std::move(callback);

  NopHolder<Descriptor> nopHolder;
  loadDescriptor(nopHolder, descriptor);
  Descriptor& nopDescriptor = nopHolder.getObject();
  opIter->allocationId = std::move(nopDescriptor.allocationId);
  opIter->startEvHandle = std::move(nopDescriptor.startEvHandle);
  opIter->bufferHandle = std::move(nopDescriptor.handle);
  opIter->offset = nopDescriptor.offset;

  recvOps_.advanceOperation(opIter);
}

void ChannelImpl::advanceRecvOperation(
    RecvOpIter opIter,
    RecvOperation::State prevOpState) {
  TP_DCHECK(context_->inLoop());

  RecvOperation& op = *opIter;

  recvOps_.attemptTransition(
      opIter,
      /*from=*/RecvOperation::UNINITIALIZED,
      /*to=*/RecvOperation::FINISHED,
      /*cond=*/error_,
      /*actions=*/{&ChannelImpl::callRecvCallback});

  // Needs to go after previous op to ensure predictable and consistent ordering
  // of write calls on reply control connection and read calls on ack control
  // connection.
  recvOps_.attemptTransition(
      opIter,
      /*from=*/RecvOperation::UNINITIALIZED,
      /*to=*/RecvOperation::READING_ACK,
      /*cond=*/!error_ && prevOpState >= RecvOperation::READING_ACK,
      /*actions=*/
      {&ChannelImpl::waitOnStartEventAndCopyAndRecordStopEvent,
       &ChannelImpl::callRecvCallback,
       &ChannelImpl::writeReplyAndReadAck});

  // This transition is needed just to keep the operation (and thus its stop
  // event) alive until the remote acknowledged having finished using the event.
  recvOps_.attemptTransition(
      opIter,
      /*from=*/RecvOperation::READING_ACK,
      /*to=*/RecvOperation::FINISHED,
      /*cond=*/op.doneReadingAck,
      /*actions=*/{});
}

void ChannelImpl::waitOnStartEventAndCopyAndRecordStopEvent(RecvOpIter opIter) {
  RecvOperation& op = *opIter;

  const cudaIpcEventHandle_t* startEvHandle =
      reinterpret_cast<const cudaIpcEventHandle_t*>(op.startEvHandle.c_str());
  const cudaIpcMemHandle_t* remoteHandle =
      reinterpret_cast<const cudaIpcMemHandle_t*>(op.bufferHandle.c_str());

  TP_VLOG(6) << "Channel " << id_ << " is copying payload (#"
             << op.sequenceNumber << ")";

  CudaEvent startEv(op.deviceIdx, *startEvHandle);
  startEv.wait(op.stream, op.deviceIdx);

  void* remoteBasePtr =
      context_->openIpcHandle(op.allocationId, *remoteHandle, op.deviceIdx);
  {
    CudaDeviceGuard guard(op.deviceIdx);
    TP_CUDA_CHECK(cudaMemcpyAsync(
        op.ptr,
        static_cast<uint8_t*>(remoteBasePtr) + op.offset,
        op.length,
        cudaMemcpyDeviceToDevice,
        op.stream));
  }

  op.stopEv.record(op.stream);

  TP_VLOG(6) << "Channel " << id_ << " done copying payload (#"
             << op.sequenceNumber << ")";
}

void ChannelImpl::callRecvCallback(RecvOpIter opIter) {
  RecvOperation& op = *opIter;

  op.callback(error_);
  // Reset callback to release the resources it was holding.
  op.callback = nullptr;
}

void ChannelImpl::writeReplyAndReadAck(RecvOpIter opIter) {
  RecvOperation& op = *opIter;

  TP_VLOG(6) << "Channel " << id_ << " is writing reply notification (#"
             << op.sequenceNumber << ")";
  auto nopReplyHolder = std::make_shared<NopHolder<Reply>>();
  Reply& nopReply = nopReplyHolder->getObject();
  nopReply.stopEvHandle = op.stopEv.serializedHandle();
  replyConnection_->write(
      *nopReplyHolder,
      callbackWrapper_([nopReplyHolder,
                        sequenceNumber{op.sequenceNumber}](ChannelImpl& impl) {
        TP_VLOG(6) << "Channel " << impl.id_
                   << " done writing reply notification (#" << sequenceNumber
                   << ")";
      }));

  TP_VLOG(6) << "Channel " << id_ << " is reading ACK notification (#"
             << op.sequenceNumber << ")";
  auto nopAckHolder = std::make_shared<NopHolder<Ack>>();
  ackConnection_->read(
      *nopAckHolder,
      callbackWrapper_([opIter, nopAckHolder](ChannelImpl& impl) {
        TP_VLOG(6) << "Channel " << impl.id_
                   << " done reading ACK notification (#"
                   << opIter->sequenceNumber << ")";
        opIter->doneReadingAck = true;
        impl.recvOps_.advanceOperation(opIter);
      }));
}

void ChannelImpl::handleErrorImpl() {
  sendOps_.advanceAllOperations();
  recvOps_.advanceAllOperations();

  replyConnection_->close();
  ackConnection_->close();

  context_->unenroll(*this);
}

} // namespace cuda_ipc
} // namespace channel
} // namespace tensorpipe
