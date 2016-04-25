/*
 *  Copyright (c) 2016, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "McParser.h"

#include <algorithm>

#include <folly/Bits.h>
#include <folly/Format.h>
#include <folly/Memory.h>
#include <folly/ThreadLocal.h>
#include <folly/io/Cursor.h>

#include "mcrouter/lib/allocator/JemallocNodumpAllocator.h"
#include "mcrouter/lib/network/UmbrellaProtocol.h"

namespace {
// Adjust buffer size after this many requests
constexpr size_t kAdjustBufferSizeInterval = 10000;
}

namespace facebook { namespace memcache {

#ifdef CAN_USE_JEMALLOC_NODUMP_ALLOCATOR
namespace {

folly::ThreadLocal<JemallocNodumpAllocator> allocator;

folly::IOBuf copyToNodumpBuffer(
    const UmbrellaMessageInfo& umMsgInfo,
    const folly::IOBuf& readBuffer) {
  // Allocate buffer
  const size_t bufSize = umMsgInfo.headerSize + umMsgInfo.bodySize;
  void* p = allocator->allocate(bufSize);
  if (!p) {
    LOG(WARNING) << "Not enough memory to create a nodump buffer";
    throw std::bad_alloc();
  }
  // Copy data
  folly::io::Cursor c(&readBuffer);
  c.pull(p, readBuffer.length());
  // Transfer ownership to a new IOBuf
  return folly::IOBuf(folly::IOBuf::TAKE_OWNERSHIP,
                      p,
                      bufSize,
                      readBuffer.length(),
                      JemallocNodumpAllocator::deallocate,
                      reinterpret_cast<void*> (allocator->getFlags()));
}

}
#endif

McParser::McParser(ParserCallback& callback,
                   size_t minBufferSize,
                   size_t maxBufferSize,
                   const bool useJemallocNodumpAllocator)
    : callback_(callback),
      bufferSize_(minBufferSize),
      maxBufferSize_(maxBufferSize),
      readBuffer_(folly::IOBuf::CREATE, bufferSize_),
      useJemallocNodumpAllocator_(useJemallocNodumpAllocator) {
#ifndef CAN_USE_JEMALLOC_NODUMP_ALLOCATOR
  useJemallocNodumpAllocator_ = false;
#endif
}

void McParser::reset() {
  readBuffer_.clear();
}

void McParser::shrinkBuffer() {
  if (parsedMessages_ >= kAdjustBufferSizeInterval &&
      readBuffer_.capacity() > maxBufferSize_ && readBuffer_.length() == 0) {
    parsedMessages_ = 0;
    bufferSize_ = std::min(bufferSize_, maxBufferSize_);
    readBuffer_ = folly::IOBuf(folly::IOBuf::CREATE, bufferSize_);
  }
}

std::pair<void*, size_t> McParser::getReadBuffer() {
  readBuffer_.unshare();
  if (!readBuffer_.length() && readBuffer_.capacity() > 0) {
    /* If we read everything, reset pointers to 0 and re-use the buffer */
    readBuffer_.clear();
  } else if (readBuffer_.headroom() > 0) {
    /* Move partially read data to the beginning */
    readBuffer_.retreat(readBuffer_.headroom());
  } else {
    /* Reallocate more space if necessary */
    readBuffer_.reserve(0, bufferSize_);
  }
  return std::make_pair(readBuffer_.writableTail(),
                        std::min(readBuffer_.tailroom(), bufferSize_));
}

bool McParser::readUmbrellaOrCaretData() {
  while (!readBuffer_.empty()) {
    // Parse header
    UmbrellaParseStatus parseStatus;
    if (protocol_ == mc_umbrella_protocol) {
      parseStatus = umbrellaParseHeader(
          readBuffer_.data(), readBuffer_.length(), umMsgInfo_);
    } else {
      parseStatus = caretParseHeader(
          readBuffer_.data(), readBuffer_.length(), umMsgInfo_);
    }

    if (parseStatus == UmbrellaParseStatus::NOT_ENOUGH_DATA) {
      return true;
    }

    if (parseStatus != UmbrellaParseStatus::OK) {
      callback_.parseError(
          mc_res_remote_error,
          folly::sformat("Error parsing {} header",
                         mc_protocol_to_string(protocol_)));
      return false;
    }

    const auto messageSize = umMsgInfo_.headerSize + umMsgInfo_.bodySize;

    // Parse message body
    // Case 1: Entire message (and possibly part of next) is in the buffer
    if (readBuffer_.length() >= messageSize) {
      bool cbStatus;
      if (protocol_ == mc_umbrella_protocol) {
        cbStatus = callback_.umMessageReady(umMsgInfo_, readBuffer_);
      } else {
        cbStatus = callback_.caretMessageReady(umMsgInfo_, readBuffer_);
      }

      if (!cbStatus) {
        readBuffer_.clear();
        return false;
      }
      readBuffer_.trimStart(messageSize);
      continue;
    }

    // Case 2: We don't have full header, so return to wait for more data
    if (readBuffer_.length() < umMsgInfo_.headerSize) {
      return true;
    }

    // Case 3: We have the full header, but not the full body. If needed,
    // reallocate into a buffer large enough for full header and body. Then
    // return to wait for remaining data.
    if (readBuffer_.length() + readBuffer_.tailroom() < messageSize) {
      readBuffer_.unshare();
      bufferSize_ = std::max(bufferSize_, messageSize);
      readBuffer_.reserve(
          0 /* minHeadroom */,
          bufferSize_ - readBuffer_.length() /* minTailroom */);
    }
#ifdef CAN_USE_JEMALLOC_NODUMP_ALLOCATOR
    if (useJemallocNodumpAllocator_) {
      readBuffer_ = copyToNodumpBuffer(umMsgInfo_, readBuffer_);
    }
#endif
    return true;
  }
  return true;
}

bool McParser::readDataAvailable(size_t len) {
  // Caller is responsible for ensuring the read buffer has enough tailroom
  readBuffer_.append(len);
  if (UNLIKELY(readBuffer_.empty())) {
    return true;
  }

  if (UNLIKELY(!seenFirstByte_)) {
    seenFirstByte_ = true;
    protocol_ = determineProtocol(*readBuffer_.data());
    if (protocol_ == mc_umbrella_protocol || protocol_ == mc_caret_protocol) {
      outOfOrder_ = true;
    } else if (protocol_ == mc_ascii_protocol) {
      outOfOrder_ = false;
    } else {
      return false;
    }
  }

  if (protocol_ == mc_umbrella_protocol || protocol_ == mc_caret_protocol) {
    const auto ret = readUmbrellaOrCaretData();
    shrinkBuffer(); // no-op if buffer is not large
    return ret;
  } else {
    callback_.handleAscii(readBuffer_);
    return true;
  }
}

}}  // facebook::memcache
